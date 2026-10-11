import json
import os
import random

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
)

from AppState import AppState
import AppLayout
from ClassCustomTitleBar import CustomTitleBar
from ClassLongVerticalLabelSeries import LongVLabels
from ClassMusicSelectWindow import PICKER_STYLE, MIN_RANDOM_MINUTES, MAX_RANDOM_MINUTES
import SavedSettings
from WorkoutHelpers import MinimizedDuration
import ZwoConvert

#the picker's list from last time: {"files": [path, ...], "last": path}
LIST_FILE = "workoutPickerANT.json"
#the list to start from with nothing saved: the workouts folder next to the app
DEFAULT_FOLDER = "awoWorkouts"
#a random pick takes any workout within this fraction of the minutes asked for (else the closest one)
RANDOM_TOLERANCE = 0.2


#Awooo workouts, and Zwift workouts (ridden as the .awo they convert to, see ZwoConvert.py)
WORKOUT_EXTENSIONS = (".awo", ".zwo")


#WorkoutSetupWindow.preview_workout() results, by (path, size, modified time, FTP, target unit), so the picker only
#reads a file again when it (or what its blocks are worked out from) has changed, not every time it's opened
_previewCache = {}


def cached_preview(setup, path):
    try:
        stat = os.stat(path)
    except OSError:
        return setup.preview_workout(path)
    key = (path, stat.st_size, stat.st_mtime_ns, AppState.ftp, AppState.target_unit())
    preview = _previewCache.get(key)
    if preview is None:
        #(an older read of the same file is dropped)
        for old in [k for k in _previewCache if k[0] == path]:
            del _previewCache[old]
        preview = _previewCache[key] = setup.preview_workout(path)
    return preview


def folder_workouts(folder):
    return [os.path.join(folder, f).replace("\\", "/") for f in sorted(os.listdir(folder), key=str.lower)
            if f.lower().endswith(WORKOUT_EXTENSIONS)]


###Workout Mode's workout picker, laid out like Musical Cadence Mode's song picker: a list of .awo and .zwo files (a
###folder of them, or files) with each one's title and length, and the one picked previewed beside it, block by block.
###Each file is read as it's added (WorkoutSetupWindow.preview_workout), so one that can't be used says why. A .zwo
###(Zwift) file is ridden as is, or can be converted to an .awo file beside it.
class WorkoutSelectWindow(QDialog):
    def __init__(self, setup):
        super(WorkoutSelectWindow, self).__init__(setup)
        self.setup = setup
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Dialog)
        self.setWindowTitle("   Pick a Workout   ")
        self.setStyleSheet(PICKER_STYLE % "WorkoutSelectWindow")
        self.resize(1300, 860)

        self.previews = {} #path: WorkoutSetupWindow.preview_workout() result

        layout = QVBoxLayout(self)
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        layout.addWidget(self.title_bar)

        addRow = QHBoxLayout()
        for text, slot in (("Add Folder", self.add_folder), ("Add Workouts", self.add_files),
                           ("Remove", self.remove_selected), ("Clear", self.clear_workouts),
                           ("Convert to .awo", self.convert_selected)):
            button = QPushButton(text)
            button.clicked.connect(slot)
            addRow.addWidget(button)
        #(only for a .zwo file)
        self.convertButton = button
        layout.addLayout(addRow)

        middle = QHBoxLayout()
        self.workoutList = QListWidget()
        self.workoutList.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.workoutList.currentItemChanged.connect(lambda item, _: self.show_preview(item))
        self.workoutList.itemDoubleClicked.connect(lambda item: self.accept())
        middle.addWidget(self.workoutList, 1)
        previewColumn = QVBoxLayout()
        self.previewTitle = QLabel()
        self.previewTitle.setWordWrap(True)
        self.previewTitle.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.previewTitle.setFixedWidth(500)
        previewColumn.addWidget(self.previewTitle)
        #the blocks as the setup screen's workout list shows them (colored by power zone)
        self.preview = LongVLabels(True, AppState.ftp, AppState.ftpRanges)
        self.preview.setFixedWidth(500)
        previewColumn.addWidget(self.preview, 1)
        middle.addLayout(previewColumn)
        layout.addLayout(middle, 1)

        options = QHBoxLayout()
        options.addStretch()
        options.addWidget(QLabel("Random workout of about"))
        self.randomMinutes = QSpinBox()
        self.randomMinutes.setRange(MIN_RANDOM_MINUTES, MAX_RANDOM_MINUTES)
        self.randomMinutes.setSuffix(" min")
        options.addWidget(self.randomMinutes)
        randomButton = QPushButton("Pick Random Workout")
        randomButton.clicked.connect(self.pick_random)
        options.addWidget(randomButton)
        options.addStretch()
        layout.addLayout(options)

        self.summaryLabel = QLabel()
        self.summaryLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.summaryLabel)

        buttons = QHBoxLayout()
        self.okButton = QPushButton("Ride This Workout")
        self.okButton.clicked.connect(self.accept)
        cancelButton = QPushButton("Cancel")
        cancelButton.clicked.connect(self.reject)
        buttons.addWidget(self.okButton)
        buttons.addWidget(cancelButton)
        layout.addLayout(buttons)

        #the list from last time (or the workouts folder), with the workout picked last time (or the loaded one) picked
        self.randomMinutes.setValue(int(SavedSettings.load().get("workoutRandomMinutes", 60)))
        saved = self.load_list()
        files = [f for f in saved.get("files", []) if os.path.isfile(f)]
        if not files and os.path.isdir(DEFAULT_FOLDER):
            files = [os.path.abspath(f).replace("\\", "/") for f in folder_workouts(DEFAULT_FOLDER)]
        self.add_to_list(files)
        self.select_path(AppState.workoutFilename or saved.get("last"))
        self.update_summary()

        #scaled for this screen, and kept on it (see AppLayout.py)
        AppLayout.fit_dialog(self)

    ###Adding workouts
    def add_folder(self):
        folder = QFileDialog.getExistingDirectory(self, "Add a Folder of Workouts", DEFAULT_FOLDER)
        if folder:
            self.add_to_list(folder_workouts(folder))

    def add_files(self):
        paths, _ = QFileDialog.getOpenFileNames(self, "Add Workouts", DEFAULT_FOLDER,
                                                "Workout Files (*.awo *.zwo);; Awooo Workout Files (*.awo);; "
                                                "Zwift Workout Files (*.zwo);; All Files (*)")
        self.add_to_list(paths)

    def add_to_list(self, paths):
        listed = set(self.paths())
        first = None
        for path in paths:
            path = path.replace("\\", "/")
            if path in listed:
                continue
            listed.add(path)
            if path not in self.previews:
                self.previews[path] = cached_preview(self.setup, path)
            item = QListWidgetItem(self.item_text(path))
            item.setData(Qt.ItemDataRole.UserRole, path)
            item.setToolTip(path)
            if not self.previews[path]["ok"]:
                item.setForeground(Qt.GlobalColor.darkRed)
            self.workoutList.addItem(item)
            first = first or item
        if first is not None and self.workoutList.currentItem() is None:
            self.workoutList.setCurrentItem(first)
        self.update_summary()

    def item_text(self, path):
        preview = self.previews[path]
        name = os.path.basename(path)
        if not preview["ok"]:
            return f"{name}   (can't be used)"
        title = f"{preview['title']}   ({name})" if preview["title"] else name
        return f"{title}   {MinimizedDuration(preview['seconds'])}"

    def convert_selected(self):
        """The selected .zwo file written as an .awo file beside it, which is then added and picked"""
        zwoPath = self.selected_path()
        if not zwoPath.lower().endswith(".zwo"):
            return
        awoPath = ZwoConvert.awo_path(zwoPath).replace("\\", "/")
        if os.path.exists(awoPath) and QMessageBox.question(
                self, "Convert to .awo", f"{os.path.basename(awoPath)} already exists. Replace it?") \
                != QMessageBox.StandardButton.Yes:
            return
        try:
            _, warnings = ZwoConvert.write_awo(zwoPath, awoPath)
        except (ZwoConvert.ZwoError, OSError) as e:
            QMessageBox.warning(self, "Convert to .awo", f"Couldn't convert {os.path.basename(zwoPath)}:\n{e}")
            return
        #read again: it may have been listed before, as an older conversion
        self.previews.pop(awoPath, None)
        for i in range(self.workoutList.count()):
            if self.workoutList.item(i).data(Qt.ItemDataRole.UserRole) == awoPath:
                self.workoutList.takeItem(i)
                break
        self.add_to_list([awoPath])
        self.select_path(awoPath)
        if warnings:
            QMessageBox.information(self, "Convert to .awo", f"Saved {os.path.basename(awoPath)}\n\n" + "\n".join(warnings))

    def remove_selected(self):
        item = self.workoutList.currentItem()
        if item is not None:
            self.workoutList.takeItem(self.workoutList.row(item))
        self.update_summary()

    def clear_workouts(self):
        self.workoutList.clear()
        self.update_summary()

    ###Picking
    def paths(self):
        return [self.workoutList.item(i).data(Qt.ItemDataRole.UserRole) for i in range(self.workoutList.count())]

    def select_path(self, path):
        for i in range(self.workoutList.count()):
            if self.workoutList.item(i).data(Qt.ItemDataRole.UserRole) == path:
                self.workoutList.setCurrentRow(i)
                self.workoutList.scrollToItem(self.workoutList.item(i), QAbstractItemView.ScrollHint.PositionAtCenter)
                return

    def selected_path(self):
        item = self.workoutList.currentItem()
        return "" if item is None else item.data(Qt.ItemDataRole.UserRole)

    def show_preview(self, item):
        preview = self.previews.get(item.data(Qt.ItemDataRole.UserRole)) if item is not None else None
        if preview is None:
            self.previewTitle.setText("")
            self.preview.ui.widget.setFullText("")
        elif preview["ok"]:
            name = preview["title"] or os.path.basename(item.data(Qt.ItemDataRole.UserRole))
            self.previewTitle.setText(f"{name}\n{MinimizedDuration(preview['seconds'])}")
            self.preview.ui.widget.setFullText(preview["text"])
        else:
            self.previewTitle.setText(f"Can't be used:\n{preview['error']}")
            self.preview.ui.widget.setFullText("")
        self.update_summary()

    def pick_random(self):
        """A random workout from the list that's about as long as asked (within RANDOM_TOLERANCE), else the closest"""
        target = self.randomMinutes.value()*60
        usable = [p for p in self.paths() if self.previews[p]["ok"]]
        if not usable:
            return
        near = [p for p in usable if abs(self.previews[p]["seconds"] - target) <= RANDOM_TOLERANCE*target]
        if near:
            choices = [p for p in near if p != self.selected_path()] or near
            self.select_path(random.choice(choices))
        else:
            self.select_path(min(usable, key=lambda p: abs(self.previews[p]["seconds"] - target)))

    def update_summary(self):
        paths = self.paths()
        unusable = sum(1 for p in paths if not self.previews[p]["ok"])
        text = f"{len(paths)} workout{'s' if len(paths) != 1 else ''}"
        if unusable:
            text += f"   ·   {unusable} can't be used"
        self.summaryLabel.setText(text)
        path = self.selected_path()
        self.okButton.setEnabled(bool(path) and self.previews[path]["ok"])
        self.convertButton.setEnabled(path.lower().endswith(".zwo") and bool(path) and self.previews[path]["ok"])

    ###Done
    def accept(self):
        if not self.okButton.isEnabled():
            return
        self.save()
        super().accept()

    def reject(self):
        self.save()
        super().reject()

    @staticmethod
    def load_list():
        try:
            with open(LIST_FILE, encoding="utf-8") as f:
                saved = json.load(f)
            return saved if isinstance(saved, dict) else {}
        except (OSError, ValueError):
            return {}

    def save(self):
        try:
            with open(LIST_FILE, "w", encoding="utf-8") as f:
                json.dump({"files": self.paths(), "last": self.selected_path()}, f)
        except OSError:
            pass
        SavedSettings.update(workoutRandomMinutes=self.randomMinutes.value())

import os
import re

from PyQt6.QtCore import QSize, Qt, QTimer
from PyQt6.QtGui import QColor, QIcon, QPainter, QPixmap
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
)

import AppLayout
import AppTheme
import ClassAntLogger
from ClassCustomTitleBar import CustomTitleBar
from ClassMusicSelectWindow import PICKER_STYLE
import FtpColors
import KeyShortcuts
import PreviewRide
import SavedSettings

#the picker's own additions to the pickers' look: the list the arrow keys move in stands out
THEME_PICKER_STYLE = PICKER_STYLE + """
    QListWidget:focus {border: 3px solid #1CB5E0;}
    QLabel#themePreview {font: 900 14pt 'Orbitron'; background-color: transparent;}
"""
#the lists' fixed width, and the swatches beside each name (design px)
LIST_WIDTH = 440
SWATCH_SIZE = (110, 26)
#a choice is tried this long after the list stops moving (arrowing down a list doesn't load every theme on the way)
APPLY_DELAY_MS = 150
#the colors a colors file's swatch shows: the window, panels, highlights and text
SWATCH_COLORS = ("#28275e", "#9fc5d1", "#bdd6de", "#1cb5e0", "#000000")
BASE_TEXT = {"colors": "Base Theme's Colors", "ftpColors": "Base Theme's Gradients"}
BASE_TIP = "The theme's own, with no colors or gradients file on top of it"


def display_name(path):
    """"05-night-city.json" -> "Night City" """
    name = re.sub(r"^\d+[-_ ]*", "", os.path.splitext(os.path.basename(path))[0])
    return name.replace("-", " ").replace("_", " ").title() or os.path.basename(path)


def same_file(a, b):
    if a is None or b is None:
        return a is b
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def swatch(colors):
    """An icon of stripes in these colors"""
    width, height = (max(1, round(v*AppTheme.ui_scale)) for v in SWATCH_SIZE)
    pixmap = QPixmap(width, height)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setPen(Qt.PenStyle.NoPen)
    for i, color in enumerate(colors):
        left, right = round(i*width/len(colors)), round((i + 1)*width/len(colors))
        painter.setBrush(QColor(color))
        painter.drawRect(left, 0, right - left, height)
    painter.setPen(QColor("#000000"))
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawRect(0, 0, width - 1, height - 1)
    painter.end()
    return QIcon(pixmap)


def part_swatch(part, value):
    """A colors file's main colors, or a gradients file's power zones (the workout block colors)"""
    if part == "colors":
        colors = {k.lower(): v for k, v in value.items()}
        return swatch([colors.get(c, c) for c in SWATCH_COLORS])
    blocks = {**FtpColors.DEFAULTS["blocks"], **value.get("blocks", {})}
    return swatch([blocks[zone][0] for zone in FtpColors.ZONE_KEYS[:5]])


def read_part(path, part):
    """The part a colors/gradients file holds (raises OSError/ValueError for a bad one)"""
    data = AppTheme._read(path)
    if not isinstance(data.get(part), dict):
        raise ValueError(f"not a {AppTheme.PARTS[part]} file")
    return data[part]


###The theme picker, laid out like the song and workout pickers and filling the screen: the theme's colors (a file
###from themes/colors) and its power/cadence gradients (themes/gradients) picked from two lists, each tried on the app
###right away, with the workout screen beside them, part way through a ride, in the look picked (see
###PreviewRide.WorkoutPreview). Use This Look keeps it for next time; Cancel goes back to the look from before.
class ThemeSelectWindow(QDialog):
    def __init__(self, parent=None):
        super(ThemeSelectWindow, self).__init__(parent)
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Dialog)
        self.setWindowTitle("   Pick a Theme   ")
        self.setStyleSheet(THEME_PICKER_STYLE % "ThemeSelectWindow")

        #the look from before, for Cancel; the colors/gradients files tried on the app now (None: the base theme's)
        self.original = AppTheme.snapshot()
        self.applied = {part: AppTheme.part_files().get(part) for part in AppTheme.PARTS}
        self.preview = None #PreviewRide.WorkoutPreview, built once the window is up
        self.previewPicture = None

        layout = QVBoxLayout(self)
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        layout.addWidget(self.title_bar)

        addRow = QHBoxLayout()
        loadButton = QPushButton("Load Theme File")
        loadButton.clicked.connect(self.load_file)
        loadButton.setToolTip("A theme, colors or gradients .json from anywhere (e.g. saved from LayoutViewer.py)")
        addRow.addWidget(loadButton)
        baseButton = QPushButton("Base Theme")
        baseButton.clicked.connect(self.pick_base)
        baseButton.setToolTip(BASE_TIP)
        addRow.addWidget(baseButton)
        layout.addLayout(addRow)

        middle = QHBoxLayout()
        listColumn = QVBoxLayout()
        self.lists = {}
        for part, heading in (("colors", "Theme"), ("ftpColors", "Gradients")):
            label = QLabel(heading)
            label.setFixedWidth(LIST_WIDTH)
            listColumn.addWidget(label)
            partList = QListWidget()
            partList.setFixedWidth(LIST_WIDTH)
            partList.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
            partList.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
            partList.setIconSize(QSize(*(max(1, round(v*AppTheme.ui_scale)) for v in SWATCH_SIZE)))
            partList.currentItemChanged.connect(lambda item, _, partList=partList: self.choice_changed(partList))
            partList.itemDoubleClicked.connect(lambda item: self.accept())
            listColumn.addWidget(partList, 1)
            self.lists[part] = partList
        self.activeList = self.lists["colors"]
        middle.addLayout(listColumn)
        #the workout screen in the look picked (a picture of it, drawn offscreen, see PreviewRide.WorkoutPreview)
        self.previewLabel = QLabel("Drawing the workout screen...")
        self.previewLabel.setObjectName("themePreview")
        self.previewLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.previewLabel.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Ignored)
        middle.addWidget(self.previewLabel, 1)
        layout.addLayout(middle, 1)

        self.summaryLabel = QLabel()
        self.summaryLabel.setWordWrap(True)
        self.summaryLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.summaryLabel)

        buttons = QHBoxLayout()
        self.okButton = QPushButton("Use This Look")
        self.okButton.clicked.connect(self.accept)
        cancelButton = QPushButton("Cancel")
        cancelButton.clicked.connect(self.reject)
        buttons.addWidget(self.okButton)
        buttons.addWidget(cancelButton)
        layout.addLayout(buttons)

        self.applyTimer = QTimer(self)
        self.applyTimer.setSingleShot(True)
        self.applyTimer.setInterval(APPLY_DELAY_MS)
        self.applyTimer.timeout.connect(self.apply_choice)

        #keys (Ctrl shows them; Esc is the X and Cancel): ↑ ↓ move in a list, ← → go from one list to the other
        KeyShortcuts.add(loadButton, Qt.Key.Key_L, label="L")
        KeyShortcuts.add(baseButton, Qt.Key.Key_B, label="B")
        KeyShortcuts.add(self.okButton, KeyShortcuts.ENTER, label="Enter")
        for part, partList in self.lists.items():
            KeyShortcuts.add(partList, (Qt.Key.Key_Left, Qt.Key.Key_Right), field=partList, action=self.switch_list)
        KeyShortcuts.add(self.lists["colors"], label="↑ ↓", place="inside")
        KeyShortcuts.add(self.lists["ftpColors"], (Qt.Key.Key_Left, Qt.Key.Key_Right), label="← →", place="inside",
                         action=self.switch_list)
        KeyShortcuts.add(self.lists["colors"], Qt.Key.Key_Up, repeat=True,
                         action=lambda: KeyShortcuts.step_list(self.activeList, -1))
        KeyShortcuts.add(self.lists["colors"], Qt.Key.Key_Down, repeat=True,
                         action=lambda: KeyShortcuts.step_list(self.activeList, 1))

        #the files to pick from, the ones in use picked
        for part, partList in self.lists.items():
            self.fill_list(part)
        self.update_summary()

        #scaled for this screen, then filling it (see AppLayout.py)
        AppLayout.fit_dialog(self)
        screen = (parent or self).screen()
        if screen is not None:
            self.setGeometry(screen.availableGeometry())

    ###The lists
    def fill_list(self, part):
        partList = self.lists[part]
        partList.blockSignals(True)
        partList.clear()
        base = QListWidgetItem(BASE_TEXT[part])
        base.setData(Qt.ItemDataRole.UserRole, None)
        base.setToolTip(BASE_TIP)
        base.setIcon(part_swatch(part, AppTheme.base_part(part)))
        partList.addItem(base)
        folder = AppTheme.PART_DIRS[part]
        paths = [os.path.join(folder, name) for name in sorted(os.listdir(folder), key=str.lower)
                 if name.lower().endswith(".json")] if os.path.isdir(folder) else []
        inUse = self.applied[part]
        if inUse and not any(same_file(inUse, path) for path in paths):
            paths.append(inUse) #(loaded from somewhere else)
        for path in paths:
            self.add_item(part, path)
        partList.setCurrentItem(self.item_for(part, inUse) or base)
        partList.blockSignals(False)

    def add_item(self, part, path):
        item = QListWidgetItem(display_name(path))
        item.setData(Qt.ItemDataRole.UserRole, path)
        item.setToolTip(path)
        try:
            item.setIcon(part_swatch(part, read_part(path, part)))
        except (OSError, ValueError) as e:
            item.setForeground(Qt.GlobalColor.darkRed)
            item.setToolTip(f"{path}\nCan't be used: {e}")
        self.lists[part].addItem(item)
        return item

    def item_for(self, part, path):
        partList = self.lists[part]
        for row in range(partList.count()):
            item = partList.item(row)
            if same_file(item.data(Qt.ItemDataRole.UserRole), path):
                return item
        return None

    def select(self, part, path):
        """Picks the file's row (without trying it: it's already applied)"""
        partList = self.lists[part]
        item = self.item_for(part, path) or (self.add_item(part, path) if path else None)
        partList.blockSignals(True)
        partList.setCurrentItem(item)
        partList.blockSignals(False)
        if item is not None:
            partList.scrollToItem(item)

    def switch_list(self):
        other = self.lists["ftpColors"] if self.activeList is self.lists["colors"] else self.lists["colors"]
        self.activeList = other
        other.setFocus()
        if other.currentItem() is not None:
            other.scrollToItem(other.currentItem())

    def choice_changed(self, partList):
        self.activeList = partList
        self.applyTimer.start()

    ###Trying a choice on
    def apply_choice(self):
        """The lists' choices on the app (and the preview), those that changed"""
        changed = False
        for part, partList in self.lists.items():
            item = partList.currentItem()
            if item is None:
                continue
            path = item.data(Qt.ItemDataRole.UserRole)
            if same_file(path, self.applied[part]):
                continue
            try:
                kind = AppTheme.load_file(path) if path else AppTheme.drop_part(part)
            except (OSError, ValueError) as e:
                ClassAntLogger.log_event("ERROR", f"Failed to load {AppTheme.PARTS[part]} {path}: {e}")
                item.setForeground(Qt.GlobalColor.darkRed)
                item.setToolTip(f"{path}\nCan't be used: {e}")
                continue
            changed = True
            self.applied = {p: AppTheme.part_files().get(p) for p in AppTheme.PARTS}
            if kind == "theme": #(a whole theme in the colors or gradients folder: the new base theme)
                for p in AppTheme.PARTS:
                    self.fill_list(p)
                break
        if changed:
            self.redraw_preview()
            self.update_summary()

    def pick_base(self):
        for partList in self.lists.values():
            partList.setCurrentRow(0)
        self.applyTimer.stop()
        self.apply_choice()

    def load_file(self):
        folder = AppTheme.THEMES_DIR if os.path.isdir(AppTheme.THEMES_DIR) else os.getcwd()
        path, _ = QFileDialog.getOpenFileName(self, "Load Theme, Colors or Gradients", folder, "Theme files (*.json)")
        if not path:
            return
        self.applyTimer.stop()
        self.apply_choice() #(anything still waiting first, so the file goes on top of it)
        try:
            kind = AppTheme.load_file(path)
        except (OSError, ValueError) as e:
            ClassAntLogger.log_event("ERROR", f"Failed to load theme {path}: {e}")
            self.summaryLabel.setText(f"Couldn't load {os.path.basename(path)}: {e}")
            return
        self.applied = {part: AppTheme.part_files().get(part) for part in AppTheme.PARTS}
        if kind == "theme":
            #a new base theme: its own colors and gradients, the swatches for them redrawn
            for part in AppTheme.PARTS:
                self.fill_list(part)
        else:
            self.select(kind, path)
        self.redraw_preview()
        self.update_summary()

    def update_summary(self):
        themeFile = AppTheme.theme_file()
        base = f"Base theme: {display_name(themeFile)}" if themeFile else "Base theme: the app's default"
        parts = [base]
        for part, what in (("colors", "Colors"), ("ftpColors", "Gradients")):
            path = self.applied[part]
            parts.append(f"{what}: " + (display_name(path) if path else "the base theme's"))
        self.summaryLabel.setText("   ·   ".join(parts))

    ###The preview
    def showEvent(self, event):
        super().showEvent(event)
        self.activeList.setFocus()
        if self.preview is None:
            #once the window is up (riding the preview workout takes a moment)
            QTimer.singleShot(0, self.build_preview)

    def build_preview(self):
        if self.preview is not None or not self.isVisible():
            return
        setup = getattr(self.parentWidget(), "setupWidget", None)
        if setup is None:
            self.previewLabel.setText("No workout screen to preview")
            return
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            self.preview = PreviewRide.WorkoutPreview(setup, self.screen().availableGeometry().size())
        except Exception as e: #(the picker still works without it)
            ClassAntLogger.log_event("ERROR", f"Theme picker: couldn't build the workout screen preview: {e!r}")
            self.previewLabel.setText("The workout screen preview couldn't be drawn")
            return
        finally:
            QApplication.restoreOverrideCursor()
        self.redraw_preview()

    def redraw_preview(self):
        if self.preview is None:
            return
        self.previewPicture = self.preview.picture()
        self.show_picture()

    def show_picture(self):
        if self.previewPicture is None:
            return
        dpr = self.devicePixelRatioF()
        size = self.previewLabel.size()*dpr
        picture = self.previewPicture.scaled(size, Qt.AspectRatioMode.KeepAspectRatio,
                                             Qt.TransformationMode.SmoothTransformation)
        picture.setDevicePixelRatio(dpr)
        self.previewLabel.setPixmap(picture)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.show_picture()

    ###Done
    def accept(self):
        if self.applyTimer.isActive():
            self.applyTimer.stop()
            self.apply_choice()
        SavedSettings.update(themeFile=AppTheme.theme_file(), themePartFiles=AppTheme.part_files())
        ClassAntLogger.log_event("SETUP", f"Theme picked: {self.summaryLabel.text()}")
        self.close_preview()
        super().accept()

    def reject(self):
        self.applyTimer.stop()
        if AppTheme.snapshot() != self.original:
            AppTheme.restore(self.original)
        self.close_preview()
        super().reject()

    def close_preview(self):
        if self.preview is not None:
            self.preview.close()
            self.preview = None

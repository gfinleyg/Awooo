import os

from PyQt6.QtCore import Qt, QThread, pyqtSignal
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QDialog,
    QFileDialog,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
)

from ClassCustomTitleBar import CustomTitleBar
import AppLayout
import KeyShortcuts
import MusicCadence
import SavedSettings
from WorkoutHelpers import MinimizedDuration

#songs a random pick can be asked for, in minutes
MIN_RANDOM_MINUTES, MAX_RANDOM_MINUTES = 5, 600

#the picker windows' look (this one and the workout picker, ClassWorkoutSelectWindow.py); % the window's class name
PICKER_STYLE = """
    %s {background-color:#9fc5d1;}
    QLabel {font: 900 12pt 'Orbitron';}
    QPushButton {font: 14pt 'Segoe UI Black'; border-radius: 10px; padding: 6px 12px;}
    QListWidget {font: 11pt 'Segoe UI'; background-color:#bdd6de; border: 2px solid black; border-radius: 6px;}
    QListWidget::item:selected {background-color:#1CB5E0; color: black;}
    QSpinBox {font: 900 12pt 'Orbitron'; background-color:#bdd6de; min-width: 130px; min-height: 30px;}
    QProgressBar {font: 900 10pt 'Orbitron'; background-color:#bdd6de; border: 1px solid black; text-align: center;}
    QProgressBar::chunk {background-color:#1CB5E0;}
"""


class AnalysisThread(QThread):
    """Works out the songs' tempos (MusicCadence.analyze_songs, worker processes) off the UI thread"""
    progressed = pyqtSignal(int, int)
    analyzed = pyqtSignal(dict)

    def __init__(self, paths):
        super().__init__()
        self.paths = paths

    def run(self):
        results = MusicCadence.analyze_songs(self.paths, progress=self.progressed.emit,
                                             cancelled=self.isInterruptionRequested)
        self.analyzed.emit(results)


###Musical Cadence Mode's song picker: a folder, songs, or an .m3u playlist; checked songs make up the ride, in
###list order (drag to reorder). Each song is analyzed as it's added (cached, see MusicCadence.py).
class MusicSelectWindow(QDialog):
    def __init__(self, parent=None):
        super(MusicSelectWindow, self).__init__(parent)
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Dialog)
        self.setWindowTitle("   Musical Cadence: Pick Songs   ")
        self.setStyleSheet(PICKER_STYLE % "MusicSelectWindow")
        self.resize(1100, 820)

        self.analyses = {} #path: MusicCadence.analyze_file() result
        self.missing = [] #playlist entries that weren't found
        self.thread = None
        self.pending = [] #songs added while an analysis was running, analyzed after it

        layout = QVBoxLayout(self)
        self.title_bar = CustomTitleBar(self, False)
        self.title_bar.setStyleSheet("background-color: #9fc5d1")
        layout.addWidget(self.title_bar)

        addRow = QHBoxLayout()
        addButtons = {}
        for text, slot in (("Add Folder", self.add_folder), ("Add Songs", self.add_songs),
                           ("Add Playlist (.m3u)", self.add_playlist), ("Remove", self.remove_selected),
                           ("Clear", self.clear_songs)):
            button = QPushButton(text)
            button.clicked.connect(slot)
            addRow.addWidget(button)
            addButtons[text] = button
        layout.addLayout(addRow)

        self.songList = QListWidget()
        self.songList.setDragDropMode(QAbstractItemView.DragDropMode.InternalMove)
        self.songList.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.songList.itemChanged.connect(lambda item: self.update_summary())
        layout.addWidget(self.songList, 1)

        self.progressBar = QProgressBar()
        self.progressBar.hide()
        layout.addWidget(self.progressBar)

        options = QGridLayout()
        options.addWidget(QLabel("Random ride of"), 0, 0, Qt.AlignmentFlag.AlignRight)
        self.randomMinutes = QSpinBox()
        self.randomMinutes.setRange(MIN_RANDOM_MINUTES, MAX_RANDOM_MINUTES)
        self.randomMinutes.setSuffix(" min")
        options.addWidget(self.randomMinutes, 0, 1)
        self.randomButton = QPushButton("Pick Random Songs")
        self.randomButton.clicked.connect(self.pick_random)
        options.addWidget(self.randomButton, 0, 2)
        allButton = QPushButton("Check All")
        allButton.clicked.connect(lambda: self.check_all(True))
        options.addWidget(allButton, 0, 3)
        noneButton = QPushButton("Check None")
        noneButton.clicked.connect(lambda: self.check_all(False))
        options.addWidget(noneButton, 0, 4)
        options.addWidget(QLabel("Preferred cadence"), 1, 0, Qt.AlignmentFlag.AlignRight)
        self.preferredRpm = QSpinBox()
        self.preferredRpm.setRange(MusicCadence.MIN_RPM, MusicCadence.MAX_RPM)
        self.preferredRpm.setSuffix(" RPM")
        self.preferredRpm.setToolTip("A song's beat is doubled or halved to the speed nearest this")
        options.addWidget(self.preferredRpm, 1, 1)
        options.addWidget(QLabel("Shortest cadence block"), 1, 2, Qt.AlignmentFlag.AlignRight)
        self.minSegment = QSpinBox()
        self.minSegment.setRange(8, 120)
        self.minSegment.setSuffix(" s")
        self.minSegment.setToolTip("Tempo changes shorter than this are folded into the cadence around them")
        options.addWidget(self.minSegment, 1, 3)
        layout.addLayout(options)

        self.summaryLabel = QLabel()
        self.summaryLabel.setWordWrap(True)
        self.summaryLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.summaryLabel)

        buttons = QHBoxLayout()
        self.okButton = QPushButton("Ride These Songs")
        self.okButton.clicked.connect(self.accept)
        cancelButton = QPushButton("Cancel")
        cancelButton.clicked.connect(self.reject)
        buttons.addWidget(self.okButton)
        buttons.addWidget(cancelButton)
        layout.addLayout(buttons)

        #keys (Ctrl shows them; Esc is the X and Cancel; Space checks the current song with the list focused).
        #Enter in the minutes box picks random songs
        for text, key, label in (("Add Folder", Qt.Key.Key_D, "D"), ("Add Songs", Qt.Key.Key_A, "A"),
                                 ("Add Playlist (.m3u)", Qt.Key.Key_P, "P"), ("Remove", Qt.Key.Key_Delete, "Del"),
                                 ("Clear", Qt.Key.Key_C, "C")):
            KeyShortcuts.add(addButtons[text], key, label=label)
        KeyShortcuts.add(self.randomButton, Qt.Key.Key_R, label="R")
        KeyShortcuts.add(self.randomButton, KeyShortcuts.ENTER, field=self.randomMinutes)
        KeyShortcuts.add(allButton, Qt.Key.Key_K, label="K")
        KeyShortcuts.add(noneButton, Qt.Key.Key_U, label="U")
        KeyShortcuts.add(self.okButton, KeyShortcuts.ENTER, label="Enter")
        KeyShortcuts.add(self.songList, Qt.Key.Key_Up, label="↑ ↓", place="inside", repeat=True,
                         action=lambda: KeyShortcuts.step_list(self.songList, -1))
        KeyShortcuts.add(self.songList, Qt.Key.Key_Down, repeat=True,
                         action=lambda: KeyShortcuts.step_list(self.songList, 1))

        #the songs and settings from last time
        settings = SavedSettings.load()
        self.randomMinutes.setValue(int(settings.get("musicRandomMinutes", 60)))
        self.preferredRpm.setValue(int(settings.get("musicPreferredRpm", MusicCadence.DEFAULT_PREFERRED_RPM)))
        self.minSegment.setValue(int(settings.get("musicMinSegment", MusicCadence.DEFAULT_MIN_SEGMENT_SECONDS)))
        saved = MusicCadence.load_selection()
        self.add_to_list([(path, title) for path, title, _ in saved if os.path.isfile(path)],
                         checked={path for path, _, isChecked in saved if isChecked})

        #scaled for this screen, and kept on it (see AppLayout.py)
        AppLayout.fit_dialog(self)

    ###Adding songs
    def add_folder(self):
        folder = QFileDialog.getExistingDirectory(self, "Add a Folder of Songs")
        if folder:
            self.add_to_list([(path, None) for path in MusicCadence.folder_songs(folder)])

    def add_songs(self):
        paths, _ = QFileDialog.getOpenFileNames(self, "Add Songs", "", "Songs (*.mp3 *.wav *.flac);; All Files (*)")
        self.add_to_list([(os.path.normpath(p), None) for p in paths if MusicCadence.is_audio(p)])

    def add_playlist(self):
        path, _ = QFileDialog.getOpenFileName(self, "Add a Playlist", "", "Playlists (*.m3u *.m3u8);; All Files (*)")
        if path:
            songs, missing = MusicCadence.read_playlist(path)
            self.missing += missing
            self.add_to_list(songs)

    def add_to_list(self, songs, checked=None):
        """Adds (path, title or None) songs not already listed, checked unless checked says otherwise"""
        listed = set(self.paths())
        added = []
        self.songList.blockSignals(True)
        for path, title in songs:
            if path in listed:
                continue
            listed.add(path)
            item = QListWidgetItem()
            item.setData(Qt.ItemDataRole.UserRole, path)
            item.setData(Qt.ItemDataRole.UserRole + 1, title or MusicCadence.song_title(path))
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked if checked is None or path in checked else Qt.CheckState.Unchecked)
            self.songList.addItem(item)
            self.label_item(item)
            added.append(path)
        self.songList.blockSignals(False)
        self.analyze(added)
        self.update_summary()

    def remove_selected(self):
        for item in self.songList.selectedItems():
            self.songList.takeItem(self.songList.row(item))
        self.update_summary()

    def clear_songs(self):
        self.songList.clear()
        self.missing = []
        self.update_summary()

    def check_all(self, checked):
        self.songList.blockSignals(True)
        for item in self.items():
            if self.usable(item):
                item.setCheckState(Qt.CheckState.Checked if checked else Qt.CheckState.Unchecked)
        self.songList.blockSignals(False)
        self.update_summary()

    ###Analysis
    def analyze(self, paths):
        paths = [p for p in paths if p not in self.analyses]
        if not paths:
            return
        if self.thread is not None:
            self.pending += paths
            return
        self.thread = AnalysisThread(paths)
        self.thread.progressed.connect(self.analysis_progress)
        self.thread.analyzed.connect(self.analysis_done)
        self.progressBar.setRange(0, len(paths))
        self.progressBar.setValue(0)
        self.progressBar.show()
        self.thread.start()
        self.update_summary()

    def analysis_progress(self, done, total):
        self.progressBar.setRange(0, total)
        self.progressBar.setValue(done)
        self.progressBar.setFormat(f"Analyzing songs: {done} / {total}")

    def analysis_done(self, results):
        self.analyses.update(results)
        self.thread.wait()
        self.thread = None
        self.progressBar.hide()
        self.songList.blockSignals(True)
        for item in self.items():
            self.label_item(item)
        self.songList.blockSignals(False)
        pending, self.pending = self.pending, []
        self.analyze(pending)
        self.update_summary()

    def stop_analysis(self):
        if self.thread is not None:
            self.thread.requestInterruption()
            self.thread.wait()

    ###List helpers
    def items(self):
        return [self.songList.item(i) for i in range(self.songList.count())]

    def paths(self):
        return [item.data(Qt.ItemDataRole.UserRole) for item in self.items()]

    def usable(self, item):
        analysis = self.analyses.get(item.data(Qt.ItemDataRole.UserRole))
        return analysis is None or "error" not in analysis

    def label_item(self, item):
        title = item.data(Qt.ItemDataRole.UserRole + 1)
        analysis = self.analyses.get(item.data(Qt.ItemDataRole.UserRole))
        if analysis is None:
            item.setText(f"{title}   (analyzing...)")
        elif "error" in analysis:
            item.setText(f"{title}   (can't be read)")
            item.setCheckState(Qt.CheckState.Unchecked)
            item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsUserCheckable)
        else:
            item.setText(f"{title}   {MinimizedDuration(int(analysis['duration']))}")

    def checked_songs(self):
        """(path, title) of the checked songs that have been analyzed, in list order"""
        songs = []
        for item in self.items():
            path = item.data(Qt.ItemDataRole.UserRole)
            analysis = self.analyses.get(path)
            if item.checkState() == Qt.CheckState.Checked and analysis and "error" not in analysis:
                songs.append((path, item.data(Qt.ItemDataRole.UserRole + 1)))
        return songs

    def pick_random(self):
        """Checks a random set of the analyzed songs adding up to about the minutes asked for, and moves them to
        the top of the list in the order they'll play"""
        durations = {p: a["duration"] for p, a in self.analyses.items() if "error" not in a}
        picked = MusicCadence.random_selection([p for p in self.paths() if p in durations],
                                               self.randomMinutes.value()*60, durations)
        if not picked:
            return
        order = {path: i for i, path in enumerate(picked)}
        self.songList.blockSignals(True)
        rows = [self.songList.takeItem(0) for _ in range(self.songList.count())]
        rows.sort(key=lambda item: order.get(item.data(Qt.ItemDataRole.UserRole), len(order)))
        for item in rows:
            if self.usable(item):
                item.setCheckState(Qt.CheckState.Checked if item.data(Qt.ItemDataRole.UserRole) in order
                                   else Qt.CheckState.Unchecked)
            self.songList.addItem(item)
        self.songList.blockSignals(False)
        self.songList.scrollToTop()
        self.update_summary()

    def update_summary(self):
        songs = self.checked_songs()
        seconds = sum(self.analyses[p]["duration"] for p, _ in songs)
        parts = [f"Ride: {len(songs)} song{'s' if len(songs) != 1 else ''}, {MinimizedDuration(int(seconds))}"]
        unreadable = sum(1 for a in self.analyses.values() if "error" in a)
        if unreadable:
            parts.append(f"{unreadable} can't be read")
        if self.missing:
            parts.append(f"{len(self.missing)} playlist entr{'ies' if len(self.missing) != 1 else 'y'} not found")
        if self.thread is not None:
            parts.append("still analyzing")
        self.summaryLabel.setText("   ·   ".join(parts))
        self.okButton.setEnabled(bool(songs) and self.thread is None)

    ###Done
    def accept(self):
        if not self.checked_songs():
            return
        self.save()
        super().accept()

    def reject(self):
        self.stop_analysis()
        self.save()
        super().reject()

    def save(self):
        MusicCadence.save_selection([[item.data(Qt.ItemDataRole.UserRole), item.data(Qt.ItemDataRole.UserRole + 1),
                                      item.checkState() == Qt.CheckState.Checked] for item in self.items()])
        SavedSettings.update(
            musicPreferredRpm=self.preferredRpm.value(), musicMinSegment=self.minSegment.value(),
            musicRandomMinutes=self.randomMinutes.value())

    def selection(self):
        """What the ride is built from, see MusicCadence.build_workout"""
        return {"songs": self.checked_songs(), "analyses": dict(self.analyses),
                "preferred": self.preferredRpm.value(), "minSegment": self.minSegment.value()}

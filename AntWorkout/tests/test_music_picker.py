"""The song picker (ClassMusicSelectWindow.py): last time's songs whose files are gone taken off the list (and counted),
the songs from an older version of the analysis marked and counted, and Reprocess Songs analyzing those and the
unreadable ones again (enabled only when there are any)"""

import json

import pytest
from PyQt6.QtCore import Qt

import MusicCadence


@pytest.fixture
def picker_class(qapp):
    from ClassMusicSelectWindow import MusicSelectWindow

    class Picker(MusicSelectWindow):
        """Analyzes at once: "old" songs from an older version and "bad" ones unreadable, until reprocessed"""
        def analyze(self, paths, redo=False):
            self.redone = getattr(self, "redone", []) + (list(paths) if redo else [])
            for path in paths:
                if "bad" in path and not redo:
                    self.analyses[path] = {"error": "can't be read"}
                else:
                    self.analyses[path] = {"duration": 60.0}
                    if "old" in path and not redo:
                        self.analyses[path]["version"] = {**MusicCadence.cache_version(), "analysis": [0, 0]}
            for item in self.items():
                self.label_item(item)
            self.update_summary()

        def save(self):
            pass
    return Picker


def test_missing_songs_removed_and_reprocess(picker_class, tmp_path):
    songs = {name: tmp_path / f"{name}.mp3" for name in ("new", "old", "bad")}
    for path in songs.values():
        path.write_bytes(b"x")
    saved = [[str(songs["new"]), "New", True], [str(tmp_path / "gone1.mp3"), "Gone 1", True],
             [str(songs["old"]), "Old", True], [str(songs["bad"]), "Bad", True], [str(tmp_path / "gone2.mp3"), "Gone 2", False]]
    with open(MusicCadence.SELECTION_FILE, "w", encoding="utf-8") as f:
        json.dump(saved, f)
    picker = picker_class()
    texts = [picker.songList.item(i).text() for i in range(picker.songList.count())]
    assert texts == ["New   1m", "Old   1m   (older version)", "Bad   (can't be read)"]
    assert picker.summaryLabel.text() == ("Ride: 2 songs, 2m   ·   1 can't be read   ·   1 from an older version   ·   "
                                          "2 missing songs removed")
    assert picker.reprocessButton.isEnabled()
    #the older one and the unreadable one, analyzed again (not from the cache)
    picker.reprocess()
    assert picker.redone == [str(songs["old"]), str(songs["bad"])]
    texts = [picker.songList.item(i).text() for i in range(picker.songList.count())]
    assert texts == ["New   1m", "Old   1m", "Bad   1m"]
    #(the one that couldn't be read can be checked now)
    bad = picker.songList.item(2)
    assert bad.flags() & Qt.ItemFlag.ItemIsUserCheckable and bad.checkState() == Qt.CheckState.Unchecked
    assert picker.summaryLabel.text() == "Ride: 2 songs, 2m   ·   2 missing songs removed"
    assert not picker.reprocessButton.isEnabled()
    #nothing more to do; Clear forgets the removed ones too
    picker.clear_songs()
    assert picker.summaryLabel.text() == "Ride: 0 songs, 0s"
    picker.deleteLater()


def test_counts_only_the_listed_songs(picker_class, tmp_path):
    picker = picker_class()
    picker.add_to_list([("C:/Music/bad.mp3", "Bad"), ("C:/Music/old.mp3", "Old")])
    assert "1 can't be read   ·   1 from an older version" in picker.summaryLabel.text()
    picker.songList.selectAll()
    picker.remove_selected()
    assert picker.summaryLabel.text() == "Ride: 0 songs, 0s" and not picker.reprocessButton.isEnabled()
    picker.deleteLater()

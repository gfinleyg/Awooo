"""The theme picker (ClassThemeSelectWindow.py) on the real main window: its colors and gradients lists tried on the app
as they're picked, Cancel going back to the look from before, Use This Look saving it, and its workout screen preview
(PreviewRide.WorkoutPreview) ridden in its own AppState and WorkoutData, leaving the app's untouched.
(Named to run before test_screens.py: each pick re-styles every widget there is, and that one leaves thousands)"""

import json
import os

import pytest
from PyQt6.QtCore import Qt

from AppState import AppState
from conftest import APP_DIR
import WorkoutData
from test_app_flow import app_window, load_workout  # noqa: F401  (the fixture)


pytestmark = pytest.mark.whole_app

COLORS = {"colors": {"#9fc5d1": "#2a2a3a", "#000000": "#f4f4f8"}}
#(zone "1" dark navy: it stands out on the default background, but has to be moved on Night City's dark one)
GRADIENTS = {"ftpColors": {"zones": {"1": "#112233"}}}


@pytest.fixture
def picker(app_window, monkeypatch, tmp_path):  # noqa: F811
    window, native, errors = app_window
    import AppTheme
    import PreviewRide
    from ClassThemeSelectWindow import ThemeSelectWindow
    dirs = {"colors": tmp_path / "themes" / "colors", "ftpColors": tmp_path / "themes" / "gradients"}
    for folder in dirs.values():
        folder.mkdir(parents=True)
    (dirs["colors"] / "01-night-city.json").write_text(json.dumps(COLORS))
    (dirs["ftpColors"] / "01-plasma.json").write_text(json.dumps(GRADIENTS))
    (dirs["ftpColors"] / "02-broken.json").write_text("{not json")
    monkeypatch.setattr(AppTheme, "PART_DIRS", {part: str(folder) for part, folder in dirs.items()})
    monkeypatch.setattr(PreviewRide, "PREVIEW_WORKOUT", os.path.join(APP_DIR, "awoWorkouts",
                                                                     "0_Little_bit_of_everything.awo"))
    dialog = ThemeSelectWindow(window)
    yield window, dialog, AppTheme
    dialog.close_preview()
    dialog.deleteLater()
    AppTheme.clear()


def pick(dialog, part, row):
    dialog.lists[part].setCurrentRow(row)
    dialog.applyTimer.stop()
    dialog.apply_choice()


def test_lists_show_the_theme_files(picker):
    window, dialog, AppTheme = picker
    colors = [dialog.lists["colors"].item(i).text() for i in range(dialog.lists["colors"].count())]
    gradients = [dialog.lists["ftpColors"].item(i).text() for i in range(dialog.lists["ftpColors"].count())]
    assert colors == ["Base Theme's Colors", "Night City"]
    assert gradients == ["Base Theme's Gradients", "Plasma", "Broken"]
    #picking dark colors: the gradients' swatches are fitted to them, a gradient moved to stand out marked
    from ClassThemeSelectWindow import ADJUSTED_MARK
    pick(dialog, "colors", 1)
    plasma = dialog.lists["ftpColors"].item(1)
    assert plasma.text() == "Plasma" + ADJUSTED_MARK and "zone 2 moved" in plasma.toolTip()
    assert dialog.lists["ftpColors"].item(0).text() == "Base Theme's Gradients"
    pick(dialog, "colors", 0)
    assert dialog.lists["ftpColors"].item(1).text() == "Plasma"
    #nothing on top of the theme: the base rows are picked
    assert dialog.lists["colors"].currentRow() == 0 and dialog.lists["ftpColors"].currentRow() == 0
    #fills the screen
    assert dialog.geometry() == window.screen().availableGeometry()


def test_picks_are_tried_on_and_cancel_goes_back(picker):
    window, dialog, AppTheme = picker
    before = AppTheme.snapshot()
    pick(dialog, "colors", 1)
    assert AppTheme.THEME.colors["#9fc5d1"] == "#2a2a3a"
    pick(dialog, "ftpColors", 1)
    assert AppTheme.THEME.ftp["zones"]["1"] == "#112233"
    #a file that can't be read is marked, and the look stays as it was
    pick(dialog, "ftpColors", 2)
    assert AppTheme.THEME.ftp["zones"]["1"] == "#112233"
    assert dialog.lists["ftpColors"].item(2).foreground().color() == Qt.GlobalColor.darkRed
    #back to the base theme's colors
    pick(dialog, "colors", 0)
    assert "#9fc5d1" not in AppTheme.THEME.colors
    assert "colors" not in AppTheme.part_files()
    dialog.reject()
    assert AppTheme.snapshot() == before


def test_use_this_look_saves_it(picker):
    window, dialog, AppTheme = picker
    import SavedSettings
    pick(dialog, "colors", 1)
    pick(dialog, "ftpColors", 1)
    dialog.accept()
    saved = SavedSettings.load()["themePartFiles"]
    assert os.path.basename(saved["colors"]) == "01-night-city.json"
    assert os.path.basename(saved["ftpColors"]) == "01-plasma.json"
    assert AppTheme.THEME.colors["#9fc5d1"] == "#2a2a3a"


def test_preview_leaves_the_apps_ride_alone(picker):
    window, dialog, AppTheme = picker
    setup = window.setupWidget
    load_workout(setup)
    AppState.secondsPassed = 12.0
    AppState.savedPowerVals = ([1.0, 2.0], [150, 160])
    WorkoutData.power.append(150)
    workoutList, ftpRanges = list(AppState.workoutList), list(AppState.ftpRanges)
    dialog.show()
    dialog.build_preview()
    assert dialog.preview is not None
    assert not dialog.previewPicture.isNull()
    assert dialog.previewLabel.pixmap() is not None and not dialog.previewLabel.pixmap().isNull()
    #the preview rode its own workout, part way through
    hud = dialog.preview.hud
    assert hud.workoutSecondsPassed > 0
    assert dialog.preview.sandbox.state["workoutFilenameShort"] == "0_Little_bit_of_everything.awo"
    #the app's own ride is just as it was
    assert AppState.workoutList == workoutList and AppState.ftpRanges == ftpRanges
    assert AppState.workoutFilenameShort == "app_flow.awo"
    assert AppState.secondsPassed == 12.0
    assert AppState.savedPowerVals == ([1.0, 2.0], [150, 160])
    assert list(WorkoutData.power.columns[0]) == [150]
    #a new look redraws it, still apart from the app's ride
    first = dialog.previewPicture.toImage()
    pick(dialog, "colors", 1)
    assert dialog.previewPicture.toImage() != first
    assert AppState.workoutList == workoutList and AppState.secondsPassed == 12.0
    #(not redrawn along with the app's screens: only in its own sandbox)
    import FtpColors
    assert all(ref() is not hud for ref, _ in FtpColors._watchers)
    dialog.reject()
    assert dialog.preview is None

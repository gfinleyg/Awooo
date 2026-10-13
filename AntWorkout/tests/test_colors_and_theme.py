"""FtpColors (the power zone colors) and AppTheme's Theme (fonts and colors swapped into stylesheets)"""

import json

import pytest
from PyQt6.QtGui import QColor, QFont

from AppState import AppState
import AppTheme
import FtpColors
import ClassAntLogger


@pytest.fixture(autouse=True)
def fresh_theme():
    AppTheme.THEME.reset()
    AppTheme._partFiles.clear()
    FtpColors._values.clear()
    FtpColors._watchers.clear()
    yield
    AppTheme.THEME.reset()
    AppTheme._partFiles.clear()
    FtpColors._values.clear()


###Zones
def test_zone_ftp():
    AppState.ftp = "0"
    assert FtpColors.zone_ftp() == FtpColors.UNSET_FTP_WATTS
    AppState.ftpTestMode = "ramp20"
    assert FtpColors.zone_ftp() == 100
    AppState.ftp = "250"
    assert FtpColors.zone_ftp() == 250


def test_power_ranges():
    AppState.ftp = "200"
    assert FtpColors.power_ranges() == pytest.approx([72, 174, 238, 374])


@pytest.mark.parametrize("power, zone", [(0, "free"), (5, "0"), (10, "1"), (29.9, "2"), (30, "3"), (45, "4")])
def test_zone_of(power, zone):
    assert FtpColors.zone_of(power, [10, 20, 30, 40]) == zone


def test_zone_of_uses_the_app_ranges():
    AppState.ftpRanges[:] = [100, 200, 300, 400]
    assert FtpColors.zone_of(250) == "2"


###The table and the theme's changes
def test_get_is_a_copy():
    blocks = FtpColors.get("blocks")
    blocks["2"] = ["#000000", "#000000"]
    assert FtpColors.get("blocks")["2"] == FtpColors.DEFAULTS["blocks"]["2"]


def test_set_value_zone_and_back():
    FtpColors.set_value("blocks", ["#010101", "#020202"], zone="2")
    assert not FtpColors.is_default("blocks", "2") and FtpColors.is_default("blocks", "3")
    assert FtpColors.get("blocks")["2"] == ["#010101", "#020202"]
    assert AppTheme.THEME.ftp == {"blocks": {"2": ["#010101", "#020202"]}}
    #setting the default again, or None, drops the change
    FtpColors.set_value("blocks", FtpColors.DEFAULTS["blocks"]["2"], zone="2")
    assert AppTheme.THEME.ftp == {} and FtpColors.is_default("blocks")


def test_set_value_whole_entry():
    FtpColors.set_value("blockSingleEnd", "#123456")
    assert FtpColors.get("blockSingleEnd") == "#123456" and not FtpColors.is_default("blockSingleEnd")
    FtpColors.set_value("blockSingleEnd", None)
    assert FtpColors.get("blockSingleEnd") == FtpColors.DEFAULTS["blockSingleEnd"]


def test_theme_changes_are_picked_up():
    assert FtpColors.gauge_ring(0, [10, 20, 30, 40]) == "d9d9d9"
    #a theme loaded: a new changes dict
    AppTheme.THEME.ftp = {"gaugeRing": {"free": "#abcdef"}}
    assert FtpColors.gauge_ring(0, [10, 20, 30, 40]) == "abcdef"
    assert FtpColors.gauge_ring(15, [10, 20, 30, 40]) == "38813b"


def test_part_colors():
    ranges = [10, 20, 30, 40]
    assert FtpColors.gauge_arc(25, ranges) == ("e0d151", "fdec5a")
    assert FtpColors.spiral(50, ranges) == ("f2161a", "d11e14")
    assert "#38813b" in FtpColors.all_colors()


def test_block_colors():
    ranges = [10, 20, 30, 40]
    #a single power: its zone's start to blockSingleEnd
    assert FtpColors.block_colors(25, 0, ranges) == ("#e0d151", FtpColors.DEFAULTS["blockSingleEnd"])
    #a range: low power's start to high power's end
    assert FtpColors.block_colors(5, 45, ranges) == ("#d1d1d1", "#f2463a")
    #0 W shows in the lowest zone
    assert FtpColors.block_colors(0, 0, ranges)[0] == "#d1d1d1"
    css = FtpColors.block_css(25, 0, ranges)
    assert "#e0d151" in css and FtpColors.BLOCK_TEXT in css
    assert FtpColors.free_ride_css().count("#") == 3


def test_block_colors_in_musical_cadence_mode_follow_the_band():
    AppState.workoutMode = AppState.cadenceMode = True
    AppState.ftpRanges[:] = FtpColors.cadence_ranges(70)
    stops = FtpColors.cadence_scale_stops(AppState.ftpRanges)
    start, end = FtpColors.block_colors(90, 100)
    assert start == FtpColors.stops_color(stops, 90).name() and end == FtpColors.stops_color(stops, 100).name()


def test_graph_brushes():
    brushes = FtpColors.graph_brushes("past")
    assert len(brushes) == 5 and brushes[0].alpha() == 0x88
    assert brushes[1].name() == "#42a045"
    AppState.graphWorkoutBrushDark = []
    FtpColors.update_graph_brushes()
    assert len(AppState.graphWorkoutBrushDark) == 5


def test_gauge_scale_stops():
    stops = FtpColors.gauge_scale_stops(200)
    assert [watts for watts, _ in stops] == [70, 90, 184, 526]
    assert stops[1][1].name() == "#00ff00"


def test_cadence_ranges():
    ranges = FtpColors.cadence_ranges(60)
    assert len(ranges) == 4 and ranges == sorted(ranges)
    assert ranges[-1] == FtpColors.CADENCE_SCALE_TOP_RPM
    assert ranges[0] == pytest.approx(60 + (115 - 60)/4)
    #a very high low end still leaves room for the zones
    high = FtpColors.cadence_ranges(200)
    assert high == sorted(high) and high[-1] == 115 and len(set(high)) == 4


def test_cadence_scale_stops():
    assert FtpColors.cadence_scale_stops([60]) == []
    stops = FtpColors.cadence_scale_stops([70, 80, 90, 100])
    assert [rpm for rpm, _ in stops] == [60, 70, 80, 90, 100]


def test_stops_color():
    stops = [(0, QColor("#000000")), (100, QColor("#ffffff"))]
    assert FtpColors.stops_color(stops, -5).name() == "#000000"
    assert FtpColors.stops_color(stops, 150).name() == "#ffffff"
    assert FtpColors.stops_color(stops, 50).name() in ("#7f7f7f", "#808080")
    assert FtpColors.stops_color([], 50).name() == "#ffffff"
    #two stops at the same value
    same = [(10, QColor("#000000")), (10, QColor("#ff0000"))]
    assert FtpColors.stops_color(same, 10).name() == "#000000"


def test_notify_watchers():
    class Watcher:
        calls = 0
        def changed(self):
            self.calls += 1
    watcher = Watcher()
    FtpColors.watch(watcher, "changed")
    FtpColors.set_value("blockSingleEnd", "#111111")
    assert watcher.calls == 1
    FtpColors.notify_if_changed()
    assert watcher.calls == 1
    AppTheme.THEME.ftp = {}
    FtpColors.notify_if_changed()
    assert watcher.calls == 2


###Theme
def test_map_hex():
    theme = AppTheme.Theme()
    theme.colors = {"#9fc5d1": "#ffcc66"}
    assert theme.map_hex("#9FC5D1") == "#ffcc66"
    #Qt's #aarrggbb keeps its alpha
    assert theme.map_hex("#809fc5d1") == "#80ffcc66"
    assert theme.map_hex("#123456") == "#123456"
    assert theme.map_color((159, 197, 209)) == "#ffcc66"
    assert theme.map_color("#9fc5d1") == "#ffcc66"
    assert theme.map_color(None) is None


def test_css_colors_fonts_and_lengths():
    theme = AppTheme.Theme()
    theme.colors = {"#9fc5d1": "#ffcc66", "#000000": "#222222"}
    theme.fonts["main"].update({"family": "Inter", "scale": 150, "weight": "700"})
    css = theme.css("QLabel {font: bold 20pt 'Orbitron'; color: #9fc5d1; background-color: black; "
                    "padding: 4px; margin: 0px; border-radius: -3px;}", scale=0.5)
    assert "font: 700 15pt 'Inter'" in css
    assert "color: #ffcc66" in css and "background-color: #222222" in css
    assert "padding: 2px" in css and "margin: 0px" in css and "border-radius: -2px" in css
    #a small length never rounds away to nothing
    assert theme.css("padding: 1px", scale=0.1) == "padding: 1px"


def test_css_unknown_font_keeps_its_family():
    theme = AppTheme.Theme()
    assert theme.css("font: 10pt 'Arial'", scale=2) == "font: 20pt 'Arial'"


def test_map_font():
    theme = AppTheme.Theme()
    theme.fonts["button"].update({"family": "Inter", "scale": 200, "weight": "300"})
    font = QFont("Segoe UI Black")
    font.setPointSizeF(10)
    themed = theme.map_font(font)
    assert themed.family() == "Inter" and themed.pointSizeF() == 20 and themed.weight() == 300
    assert theme.map_font(QFont("Arial")).family() == "Arial"


def test_theme_json_round_trip():
    theme = AppTheme.Theme()
    theme.from_json({"fonts": {"main": {"family": "Inter"}, "bogus": {"family": "x"}},
                     "colors": {"#AABBCC": "#DDEEFF"}, "ftpColors": {"blockSingleEnd": "#000000"},
                     "layouts": {"old": 1}})
    assert theme.fonts["main"]["family"] == "Inter" and "bogus" not in theme.fonts
    assert theme.colors == {"#aabbcc": "#ddeeff"}
    data = theme.to_json()
    assert set(data) == {"fonts", "colors", "ftpColors"}
    assert theme.to_json("colors") == {"colors": {"#aabbcc": "#ddeeff"}}
    assert theme.to_json("ftpColors") == {"ftpColors": {"blockSingleEnd": "#000000"}}


def test_colors_in_css_and_value():
    assert AppTheme.colors_in_css("color: #ABCDEF; background: #80123456; border-color: red") == \
        {"#abcdef", "#123456", "#ff0000"}
    assert AppTheme.colors_in_value("#80123456") == {"#123456"}
    assert AppTheme.colors_in_value((255, 0, 0)) == {"#ff0000"}
    assert AppTheme.colors_in_value(5) == set()


@pytest.mark.parametrize("data, kind", [
    ({"fonts": {}}, "theme"), ({"colors": {}}, "colors"), ({"ftpColors": {}}, "ftpColors"),
    ({"colors": {}, "ftpColors": {}}, "theme"),
])
def test_file_kind(data, kind):
    assert AppTheme.file_kind(data) == kind


def test_file_kind_rejects_other_files():
    with pytest.raises(ValueError):
        AppTheme.file_kind({"something": 1})


def test_load_and_save_files(monkeypatch):
    monkeypatch.setattr(AppTheme, "refresh", lambda: None)
    monkeypatch.setattr(AppTheme, "register_fonts", lambda: [])
    AppTheme.THEME.colors = {"#111111": "#222222"}
    AppTheme.save_file("colors.json", "colors")
    with open("colors.json") as f:
        assert json.load(f) == {"colors": {"#111111": "#222222"}}
    AppTheme.THEME.reset()
    assert AppTheme.load_file("colors.json") == "colors"
    assert AppTheme.THEME.colors == {"#111111": "#222222"}
    assert AppTheme.part_files() == {"colors": "colors.json"}
    #a whole theme drops the part files
    with open("theme.json", "w") as f:
        json.dump({"fonts": {"main": {"family": "Inter"}}}, f)
    assert AppTheme.load_file("theme.json") == "theme"
    assert AppTheme.part_files() == {} and AppTheme.THEME.colors == {}
    with open("list.json", "w") as f:
        json.dump([1, 2], f)
    with pytest.raises(ValueError):
        AppTheme.load_file("list.json")


def test_default_theme_and_clear(monkeypatch):
    monkeypatch.setattr(AppTheme, "refresh", lambda: None)
    monkeypatch.setattr(AppTheme, "register_fonts", lambda: [])
    assert AppTheme.default_source() is None
    AppTheme.THEME.colors = {"#111111": "#222222"}
    AppTheme.clear()
    assert AppTheme.THEME.colors == {}
    with open(AppTheme.DEFAULT_THEME_FILE, "w") as f:
        json.dump({"fonts": {"main": {"family": "Inter"}}, "colors": {"#333333": "#444444"}}, f)
    #part files stay on top when asked
    with open("grad.json", "w") as f:
        json.dump({"ftpColors": {"blockSingleEnd": "#555555"}}, f)
    AppTheme.load_file("grad.json")
    AppTheme.clear(keepParts=True)
    assert AppTheme.THEME.colors == {"#333333": "#444444"}
    assert AppTheme.THEME.ftp == {"blockSingleEnd": "#555555"}
    #a broken default theme falls back to the built in one, and says so
    with open(AppTheme.DEFAULT_THEME_FILE, "w") as f:
        f.write("{broken")
    AppTheme.clear()
    assert AppTheme.THEME.colors == {} and "Failed to load default theme" in ClassAntLogger.read_log()


def test_vanished_part_file_is_dropped(monkeypatch):
    monkeypatch.setattr(AppTheme, "refresh", lambda: None)
    monkeypatch.setattr(AppTheme, "register_fonts", lambda: [])
    with open("c.json", "w") as f:
        json.dump({"colors": {"#111111": "#222222"}}, f)
    AppTheme.load_file("c.json")
    import os
    os.remove("c.json")
    AppTheme.clear(keepParts=True)
    assert AppTheme.part_files() == {} and "Failed to reload colors" in ClassAntLogger.read_log()


@pytest.mark.parametrize("path", sorted(__import__("glob").glob(__import__("os").path.join(
    __import__("conftest").APP_DIR, "themes", "*", "*.json"))), ids=lambda p: p.split("\\")[-1].split("/")[-1])
def test_shipped_theme_parts_load(path):
    data = AppTheme._read(path)
    kind = AppTheme.file_kind(data)
    assert kind in AppTheme.PARTS
    if kind == "colors":
        for old, new in data["colors"].items():
            assert AppTheme.HEX_RE.fullmatch(old) and AppTheme.HEX_RE.fullmatch(new)
    else:
        for key, value in data["ftpColors"].items():
            assert key in FtpColors.DEFAULTS
            if isinstance(FtpColors.DEFAULTS[key], dict):
                assert set(value) <= set(FtpColors.DEFAULTS[key])

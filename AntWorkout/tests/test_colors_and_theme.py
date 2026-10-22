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
    zones = FtpColors.get("zones")
    zones["2"] = "#000000"
    assert FtpColors.get("zones")["2"] == FtpColors.DEFAULTS["zones"]["2"]


def test_set_value_zone_and_back():
    FtpColors.set_value("zones", "#010101", zone="2")
    assert not FtpColors.is_default("zones", "2") and FtpColors.is_default("zones", "3")
    assert FtpColors.get("zones")["2"] == "#010101"
    assert AppTheme.THEME.ftp == {"zones": {"2": "#010101"}}
    #setting the default again, or None, drops the change
    FtpColors.set_value("zones", FtpColors.DEFAULTS["zones"]["2"], zone="2")
    assert AppTheme.THEME.ftp == {} and FtpColors.is_default("zones")


def test_set_value_whole_entry():
    FtpColors.set_value("free", "#123456")
    assert FtpColors.get("free") == "#123456" and not FtpColors.is_default("free")
    FtpColors.set_value("free", None)
    assert FtpColors.get("free") == FtpColors.DEFAULTS["free"]


def test_older_gradient_entries_are_dropped():
    AppTheme.THEME.set_part("ftpColors", {"blocks": {"2": ["#010101", "#020202"]}, "zones": {"1": "#123456"}})
    assert AppTheme.THEME.ftp == {"zones": {"1": "#123456"}}
    assert FtpColors.file_palette({"zones": {"1": "#123456"}})[1:3] == ["#123456", FtpColors.DEFAULTS["zones"]["2"]]


def test_theme_changes_are_picked_up():
    assert FtpColors.gauge_ring(0, [10, 20, 30, 40]) == "d9d9d9"
    #a theme loaded: a new changes dict
    AppTheme.THEME.ftp = {"free": "#abcdef"}
    assert FtpColors.gauge_ring(0, [10, 20, 30, 40]) == "abcdef"


###The zones' fades
RANGES = [10, 20, 30, 40]


@pytest.mark.parametrize("value, zone, through", [(0, 0, 0), (5, 0, .5), (10, 1, 0), (15, 1, .5), (39.9, 3, .99),
                                                  (40, 4, 0), (55, 4, 1), (100, 4, 1)])
def test_zone_position(value, zone, through):
    #(the top zone is brightest half the span of the zones below past where it starts: 40 + 15)
    assert FtpColors.zone_position(value, RANGES) == (zone, pytest.approx(through, abs=0.01))


def test_each_zone_fades_from_pale_to_its_color_and_jumps_at_the_next():
    fitted = FtpColors.palette()
    for zone in range(5):
        pale, color, _ = fitted[zone]
        start, end = FtpColors.zone_span(zone, RANGES)
        assert FtpColors.zone_hex(start, RANGES) == pale
        assert FtpColors.difference(FtpColors.zone_hex(end - 1e-9, RANGES), color) < 0.01
        #closer to the color the further through the zone
        steps = [FtpColors.difference(FtpColors.zone_hex(start + (end - start)*k/4, RANGES), color) for k in range(4)]
        assert steps == sorted(steps, reverse=True)
        #paler: less colorful
        assert FtpColors.to_oklch(pale)[1] <= FtpColors.to_oklch(color)[1] + 1e-6
    #the next zone starts pale, not where the last one ended
    assert FtpColors.difference(FtpColors.zone_hex(19.99, RANGES), FtpColors.zone_hex(20, RANGES)) > 0.1


def test_zone_color_defaults_to_the_app_ranges():
    AppState.ftpRanges[:] = [100, 200, 300, 400]
    assert FtpColors.zone_color(250).name() == FtpColors.zone_hex(250, [100, 200, 300, 400])


def theme_colors(name):
    with open(__import__("os").path.join(__import__("conftest").APP_DIR, "themes", "colors", name)) as f:
        return {k.lower(): v.lower() for k, v in json.load(f)["colors"].items()}


def test_colors_stand_out_on_every_shipped_theme():
    import glob, os
    themes = glob.glob(os.path.join(__import__("conftest").APP_DIR, "themes", "colors", "*.json"))
    gradients = glob.glob(os.path.join(__import__("conftest").APP_DIR, "themes", "gradients", "*.json"))
    assert themes and gradients
    for theme in themes:
        colors = theme_colors(os.path.basename(theme))
        backgrounds = FtpColors.theme_backgrounds(colors)
        for gradient in gradients:
            for pale, color, _ in FtpColors.palette(FtpColors.file_palette(AppTheme._read(gradient)["ftpColors"]), colors):
                assert min(FtpColors.difference(color, bg) for bg in backgrounds) >= FtpColors.COLOR_MIN_DIFFERENCE, \
                    (theme, gradient, color)
                assert (pale == color or min(FtpColors.difference(pale, bg) for bg in backgrounds)
                        >= FtpColors.PALE_MIN_DIFFERENCE), (theme, gradient, pale)


def test_zones_fades_dont_repeat_each_other():
    fitted = FtpColors.palette()
    def fade(zone):
        pale, color, _ = fitted[zone]
        return [FtpColors._blend(pale, color, k/20) for k in range(21) if k/20 >= FtpColors.ZONE_FADE_FROM]
    for zone in range(5):
        pale, color, _ = fitted[zone]
        assert pale != color #(every zone keeps a fade)
        for other in range(5):
            if other != zone:
                assert min(FtpColors.difference(pale, c) for c in fade(other)) >= FtpColors.ZONE_MIN_DIFFERENCE - 0.01, \
                    (zone, other)
    #red starts red, not the peach orange has already shown
    assert FtpColors.to_oklch(fitted[4][0])[1] > 0.1


def test_adjusted_for_the_theme():
    #the orange zone on DSA's red background is moved to stand out; on the default background nothing moves
    assert "3" in FtpColors.adjusted_zones(themeColors=theme_colors("41-dsa.json"))
    assert FtpColors.adjusted_zones() == []
    pale, color, adjusted = FtpColors.palette(themeColors=theme_colors("41-dsa.json"))[3]
    assert adjusted and color != FtpColors.DEFAULTS["zones"]["3"]


def test_zone_stops():
    stops = FtpColors.zone_stops(RANGES)
    values = [v for v, _ in stops]
    assert values == sorted(values) and values[0] == 0 and values[-1] == pytest.approx(55)
    #a hard edge where each zone starts: the last stop before it the zone's color, the one at it the next's pale end
    fitted = FtpColors.palette()
    for zone, limit in enumerate(RANGES):
        i = next(i for i, v in enumerate(values) if v >= limit)
        assert stops[i - 1][1].name() == fitted[zone][1] and stops[i][1].name() == fitted[zone + 1][0]
        assert limit - values[i - 1] < 0.01


###What each part draws with
def test_part_colors():
    assert FtpColors.gauge_arc(25, RANGES) == (FtpColors.pale_hex(2)[1:], FtpColors.zone_hex(25, RANGES)[1:])
    assert FtpColors.gauge_arc(0, RANGES) == ("d9d9d9", "d9d9d9")
    assert FtpColors.gauge_ring(15, RANGES) == FtpColors.zone_hex(15, RANGES)[1:]
    assert "#38813b" in FtpColors.all_colors()


def test_block_colors():
    #a single power: its color to its zone's pale end
    assert FtpColors.block_colors(25, 0, RANGES) == (FtpColors.zone_hex(25, RANGES), FtpColors.pale_hex(2))
    #a range: low power's color to high power's
    assert FtpColors.block_colors(5, 45, RANGES) == (FtpColors.zone_hex(5, RANGES), FtpColors.zone_hex(45, RANGES))
    css = FtpColors.block_css(25, 0, RANGES)
    assert FtpColors.zone_hex(25, RANGES) in css and FtpColors.BLOCK_TEXT in css
    assert FtpColors.free_ride_css().count("#") == 3


def test_block_colors_in_musical_cadence_mode():
    AppState.workoutMode = AppState.cadenceMode = True
    AppState.ftpRanges[:] = FtpColors.cadence_ranges(FtpColors.CADENCE_SCALE_BOTTOM_RPM)
    #50 RPM is over 40: green (zone "1"), as the graph and gauges show it
    assert FtpColors.zone_of(50) == "1"
    assert FtpColors.block_colors(50)[0] == FtpColors.zone_hex(50, AppState.ftpRanges)
    #the cadence zones start part way along their fade: 40 RPM is already clearly green, not washed out
    pale, green, _ = FtpColors.palette()[1]
    assert FtpColors.zone_hex(40) == FtpColors._blend(pale, green, FtpColors.CADENCE_FADE_FROM)
    assert FtpColors.pale_hex(1) == FtpColors.zone_hex(40)
    #(the power zones, shown beside them, still start at their pale end)
    power = FtpColors.power_ranges()
    assert FtpColors.zone_hex(power[0], power) == FtpColors.palette()[1][0]


def test_graph_colors():
    past = FtpColors.graph_color(25, RANGES, "past")
    assert past.alpha() == FtpColors.GRAPH_ALPHA and past.name() == FtpColors.zone_hex(25, RANGES)
    #not ridden yet: greyer, more so paused
    chroma = [FtpColors.to_oklch(FtpColors.graph_color(25, RANGES, state).name())[1]
              for state in ("past", "upcoming", "paused")]
    assert chroma == sorted(chroma, reverse=True)
    FtpColors.update_graph_brushes()
    assert AppState.graphWorkoutBrushDark(25, RANGES) == past
    assert AppState.graphWorkoutBrushLight(25, RANGES) == FtpColors.graph_color(25, RANGES, "paused")


def test_cadence_ranges():
    ranges = FtpColors.cadence_ranges(60)
    assert ranges == pytest.approx([60, 60 + 55/3, 60 + 110/3, 115])
    assert FtpColors.zone_of(61, ranges) == "1" and FtpColors.zone_of(59, ranges) == "0"
    #a very high low end still leaves room for the zones
    high = FtpColors.cadence_ranges(200)
    assert high == sorted(high) and high[-1] == 115 and len(set(high)) == 4


def test_notify_watchers():
    class Watcher:
        calls = 0
        def changed(self):
            self.calls += 1
    watcher = Watcher()
    FtpColors.watch(watcher, "changed")
    FtpColors.set_value("free", "#111111")
    assert watcher.calls == 1
    FtpColors.notify_if_changed()
    assert watcher.calls == 1
    AppTheme.THEME.ftp = {}
    FtpColors.notify_if_changed()
    assert watcher.calls == 2
    #a new background: the colors are fitted to it again
    AppTheme.THEME.colors = theme_colors("41-dsa.json")
    FtpColors.notify_if_changed()
    assert watcher.calls == 3


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
                     "colors": {"#AABBCC": "#DDEEFF"}, "ftpColors": {"free": "#000000"},
                     "layouts": {"old": 1}})
    assert theme.fonts["main"]["family"] == "Inter" and "bogus" not in theme.fonts
    assert theme.colors == {"#aabbcc": "#ddeeff"}
    data = theme.to_json()
    assert set(data) == {"fonts", "colors", "ftpColors"}
    assert theme.to_json("colors") == {"colors": {"#aabbcc": "#ddeeff"}}
    assert theme.to_json("ftpColors") == {"ftpColors": {"free": "#000000"}}


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
        json.dump({"ftpColors": {"free": "#555555"}}, f)
    AppTheme.load_file("grad.json")
    AppTheme.clear(keepParts=True)
    assert AppTheme.THEME.colors == {"#333333": "#444444"}
    assert AppTheme.THEME.ftp == {"free": "#555555"}
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

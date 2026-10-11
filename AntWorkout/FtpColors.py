"""
FtpColors.py

The colors that follow the power zones (by % of FTP), all in one place:

  gaugeScale      the power gauge's colored band: [% of FTP, color] stops. A stop sits exactly where
                  the needle points at that % of FTP, whatever the FTP (see gauge_scale_stops)
  gaugeArc        the power gauge's Ride At target arc, per zone: [color, color] (a gradient)
                  (Musical Cadence Mode's cadence gauge band is the second colors, see cadence_scale_stops)
  gaugeRing       the power gauge's ring behind the reading, by the power being ridden, per zone
  blocks          the workout block labels (setup list, HUD current and up next blocks), per zone:
                  [start, end]. A power range goes from its low power's start to its high power's end;
                  a single power from its start to blockSingleEnd
  blockSingleEnd  see blocks
  graph           the workout graph's bars per zone: [paused (or not started), upcoming, past]; the
                  synopsis graph uses past
  spiral          the HUD spiral's line for the current block, per zone: [line, blink]

Zones "0" to "4" split power at AppState.ftpRanges (36/87/119/187% of FTP, see
ClassWorkoutSetupWindow.py); "free" is power 0 (free ride). Transparency is added where the colors are
used, so these are all plain "#rrggbb".

A theme can change any of them ("ftpColors" in the theme file, see AppTheme.py), and LayoutViewer.py's
FTP Colors tab edits them. Whatever shows these colors registers with watch() to be redrawn when they
change.
"""

import copy
import weakref

from PyQt6.QtGui import QColor

from AppState import AppState
import FtpTest

ZONE_KEYS = ["0", "1", "2", "3", "4", "free"]

#the FTP the zones are worked out from while none is set (a ramp FTP test can be ridden without one, and has its
#own, see FtpTest.TESTS)
UNSET_FTP_WATTS = 50


def zone_ftp():
    """The FTP the power zones are worked out from: the FTP, or while it isn't set the selected ramp test's
    unsetFtpWatts (UNSET_FTP_WATTS outside one)"""
    test = FtpTest.test() or {}
    return int(AppState.ftp) or test.get("unsetFtpWatts", UNSET_FTP_WATTS)

DEFAULTS = {
    "gaugeScale": [[35, "#ffffff"], [45, "#00ff00"], [92, "#ffff00"], [263, "#8b0000"]],
    "gaugeArc": {"0": ["#38813b", "#ececec"], "1": ["#38813b", "#42ee45"], "2": ["#e0d151", "#fdec5a"],
                 "3": ["#d77004", "#f37d04"], "4": ["#d11e14", "#f2161a"], "free": ["#d9d9d9", "#ffffff"]},
    "gaugeRing": {"0": "#38813b", "1": "#38813b", "2": "#e0d151", "3": "#d77004", "4": "#d11e14",
                  "free": "#d9d9d9"},
    "blocks": {"0": ["#d1d1d1", "#ececec"], "1": ["#38813b", "#429645"], "2": ["#e0d151", "#fdec5a"],
               "3": ["#d77004", "#f37d04"], "4": ["#d63e34", "#f2463a"], "free": ["#d9d9d9", "#ffffff"]},
    "blockSingleEnd": "#ececec",
    "graph": {"0": ["#ffffff", "#c1c1c1", "#969696"], "1": ["#dfffd9", "#42ff45", "#42a045"],
              "2": ["#fffac0", "#ffff88", "#e1e500"], "3": ["#ffe7b8", "#ffb464", "#ff5e14"],
              "4": ["#ff9ea6", "#ff1c06", "#c02100"]},
    "spiral": {"0": ["#ececec", "#38813b"], "1": ["#42ee45", "#38813b"], "2": ["#fdec5a", "#e0d151"],
               "3": ["#f37d04", "#d77004"], "4": ["#f2161a", "#d11e14"], "free": ["#ffffff", "#d9d9d9"]},
}
#the graph's bars are see-through
GRAPH_ALPHA = "#88"
GRAPH_STATES = ("paused", "upcoming", "past")


###----------------------------------------------------------------------------------------------
### The table: the defaults with the theme's changes on top
###----------------------------------------------------------------------------------------------
def _theme_changes():
    import AppTheme
    return AppTheme.THEME.ftp


#each entry's current value, worked out once (the gauges and labels ask for them with every reading) until the theme's
#changes are replaced (a theme loaded) or changed (set_value, notify)
_values = {}
_valuesSource = None


def _current(key):
    """The current value of one entry, shared: not to be changed (get() gives a copy that can be)"""
    global _valuesSource
    changes = _theme_changes()
    if changes is not _valuesSource:
        _values.clear()
        _valuesSource = changes
    value = _values.get(key)
    if value is None:
        value = copy.deepcopy(DEFAULTS[key])
        if key in changes:
            if isinstance(value, dict):
                value.update(copy.deepcopy(changes[key]))
            else:
                value = copy.deepcopy(changes[key])
        _values[key] = value
    return value


def get(key):
    """The current value of one entry (a copy)"""
    return copy.deepcopy(_current(key))


def set_value(key, value, zone=None):
    """Changes one entry (or one zone of it) in the theme; None goes back to the default"""
    changes = _theme_changes()
    if zone is None:
        if value is None or value == DEFAULTS[key]:
            changes.pop(key, None)
        else:
            changes[key] = copy.deepcopy(value)
    else:
        zones = changes.setdefault(key, {})
        if value is None or value == DEFAULTS[key][zone]:
            zones.pop(zone, None)
        else:
            zones[zone] = copy.deepcopy(value)
        if not zones:
            changes.pop(key, None)
    notify()


def is_default(key, zone=None):
    changes = _theme_changes()
    if zone is None:
        return key not in changes
    return zone not in changes.get(key, {})


#the power zones' upper limits as fractions of FTP (AppState.ftpRanges, see WorkoutSetupWindow.get_ftp_ranges)
POWER_ZONE_FTP = (.36, .87, 1.19, 1.87)


def power_ranges():
    """The power zones' limits in watts; the same as AppState.ftpRanges except in Musical Cadence Mode, where
    those are cadence zones"""
    return [zone_ftp()*fraction for fraction in POWER_ZONE_FTP]


def zone_of(power, ranges=None):
    """"free" for 0 W, else "0" to "4" by the FTP ranges"""
    if power == 0:
        return "free"
    ranges = AppState.ftpRanges if ranges is None else ranges
    for i, limit in enumerate(ranges):
        if power < limit:
            return str(i)
    return str(len(ranges))


###----------------------------------------------------------------------------------------------
### What each part draws with
###----------------------------------------------------------------------------------------------
def gauge_arc(power, ranges=None):
    """The target arc's two colors for a power, without the "#" (the gauge adds transparency)"""
    return tuple(c[1:] for c in _current("gaugeArc")[zone_of(power, ranges)])


def gauge_ring(power, ranges=None):
    """The ring color for the power being ridden, without the "#" """
    return _current("gaugeRing")[zone_of(power, ranges)][1:]


def spiral(power, ranges=None):
    """(line, blink) for the current block's power, without the "#" """
    return tuple(c[1:] for c in _current("spiral")[zone_of(power, ranges)])


def all_colors():
    """Every color in use here, as "#rrggbb" """
    found = set()
    def walk(value):
        if isinstance(value, str) and value.startswith("#"):
            found.add(value.lower())
        elif isinstance(value, (list, tuple)):
            for item in value:
                walk(item)
        elif isinstance(value, dict):
            for item in value.values():
                walk(item)
    for key in DEFAULTS:
        walk(_current(key))
    return found


#the block labels' text: always dark on their light zone colors, whatever a theme does with the app's black
#text (a theme's colors can still change it, it's its own color)
BLOCK_TEXT = "#111111"


def block_css(power0, power1=0, ranges=None):
    """A block label's background: a range from its low power's start color to its high power's end
    color, a single power from its start color to blockSingleEnd"""
    blocks = _current("blocks")
    start = blocks[_block_zone(power0, ranges)][0]
    end = blocks[_block_zone(power1, ranges)][1] if power1 else _current("blockSingleEnd")
    return f"background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 {start} stop:1 {end}); color:{BLOCK_TEXT};"


def _block_zone(power, ranges):
    #the block labels put 0 W in the lowest zone (free ride text has no watts, see free_ride_css)
    zone = zone_of(power, ranges)
    return "0" if zone == "free" else zone


def free_ride_css():
    start, end = _current("blocks")["free"]
    return f"background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 {start} stop:1 {end}); color:{BLOCK_TEXT};"


def graph_brushes(state):
    """The bars' colors (one per zone, see transparent) for "paused", "upcoming" or "past" """
    index = GRAPH_STATES.index(state)
    graph = _current("graph")
    return [QColor(GRAPH_ALPHA + graph[zone][index][1:]) for zone in ZONE_KEYS[:5]]


def update_graph_brushes():
    """AppState's bar color lists, changed in place so graphs already drawn with them pick up the change"""
    for name, state in (("graphWorkoutBrushLight", "paused"), ("graphWorkoutBrushMid", "upcoming"),
                        ("graphWorkoutBrushDark", "past")):
        current = getattr(AppState, name)
        if isinstance(current, list):
            current[:] = graph_brushes(state)
        else:
            setattr(AppState, name, graph_brushes(state))


def gauge_scale_stops(ftp):
    """The gauge band's stops as (watts, QColor)"""
    return [(float(ftp)*pct/100, QColor(color)) for pct, color in sorted(_current("gaugeScale"))]


#Musical Cadence Mode's cadence scale: the gradient starts here (the least green, zone "0"'s color), and the top
#zone (red) starts here
CADENCE_SCALE_BOTTOM_RPM = 58
CADENCE_SCALE_TOP_RPM = 115


def cadence_ranges(low):
    """Musical Cadence Mode's zone limits: zone "0" up to low + a step, the rest a step apart, the top zone
    from CADENCE_SCALE_TOP_RPM"""
    zones = len(ZONE_KEYS) - 1 #(not counting free ride)
    low = min(low, CADENCE_SCALE_TOP_RPM - zones) #(room for the zones below the top)
    step = (CADENCE_SCALE_TOP_RPM - low)/(zones - 1)
    return [low + step*k for k in range(1, zones)]


def cadence_scale_stops(ranges):
    """Musical Cadence Mode's cadence gauge band as (rpm, QColor): each zone's target arc color where the zone
    starts (zone "0"'s a step below the first limit), blended between"""
    if len(ranges) < 2:
        return []
    starts = [ranges[0] - (ranges[1] - ranges[0])] + list(ranges)
    arc = _current("gaugeArc")
    return [(rpm, QColor(arc[str(zone)][1])) for zone, rpm in enumerate(starts)]


def stops_color(stops, value):
    """The color a band of (value, QColor) stops has at value"""
    if not stops:
        return QColor("#ffffff")
    if value <= stops[0][0]:
        return QColor(stops[0][1])
    for (v0, c0), (v1, c1) in zip(stops, stops[1:]):
        if value <= v1:
            t = (value - v0)/(v1 - v0) if v1 != v0 else 1
            return QColor.fromRgbF(*(a + (b - a)*t for a, b in zip(c0.getRgbF(), c1.getRgbF())))
    return QColor(stops[-1][1])


###----------------------------------------------------------------------------------------------
### Redrawing what shows them
###----------------------------------------------------------------------------------------------
_watchers = []
_lastApplied = None


def watch(obj, methodName):
    """obj.methodName() is called whenever the colors change (held weakly: a deleted window drops out)"""
    _watchers.append((weakref.ref(obj), methodName))


def notify():
    global _lastApplied
    _values.clear()
    _lastApplied = copy.deepcopy(_theme_changes())
    update_graph_brushes()
    for ref, methodName in list(_watchers):
        obj = ref()
        if obj is None:
            _watchers.remove((ref, methodName))
            continue
        try:
            getattr(obj, methodName)()
        except RuntimeError:
            #its Qt side is gone
            _watchers.remove((ref, methodName))


def notify_if_changed():
    """After a theme is loaded or cleared (AppTheme.apply)"""
    if _theme_changes() != _lastApplied:
        notify()

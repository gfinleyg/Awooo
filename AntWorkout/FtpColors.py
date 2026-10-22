"""
FtpColors.py

The colors that follow the power zones (by % of FTP), all in one place. There are five zones, "0" to "4", split at
AppState.ftpRanges (36/87/119/187% of FTP, see ClassWorkoutSetupWindow.py; in Musical Cadence Mode the cadence
zones, see cadence_ranges). Each zone has one color, its brightest:

  zones   "0" to "4": the zone's color. Each zone fades on its own: pale where the zone starts (the same hue, washed
          out), its color where it ends (the top zone's at top_of(ranges)) - neighboring zones don't blend
  free    free ride (power 0): the block labels with no target

Everything colored by zone asks zone_color() (or the helpers built on it):
  the power and cadence gauges' bands (zone_stops), their center rings, the Ride At target arc (gauge_arc), the
  ring behind the power reading (gauge_ring), the HUD's spiral (ClassPowerSpiral.py), the workout block labels
  (block_colors / block_css) and the workout graph's bars (graph_color: upcoming and paused mixed with grey)

The colors are fitted to the theme (fitted_palette): a color too close to the window's background behind the
gauges, spiral and graph (its gradient part way along, see BACKGROUND_AT) has its lightness moved until it stands
out, and a pale end too faint, or looking like somewhere along another zone's fade (washed out, orange and red
look alike), is taken back toward its zone's color. The hue is kept. A palette whose colors moved noticeably for a
theme is "adjusted" for it (the theme picker marks it).

A theme can change any of them ("ftpColors" in the theme file, see AppTheme.py; themes/gradients holds ready
made ones), and LayoutViewer.py's FTP Colors tab edits them. Whatever shows these colors registers with watch()
to be redrawn when they change.
"""

import bisect
import copy
import functools
import math
import weakref

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QPen

from AppState import AppState
import FtpTest

ZONE_KEYS = ["0", "1", "2", "3", "4", "free"]
ZONES = ZONE_KEYS[:5]

#the FTP the zones are worked out from while none is set (a ramp FTP test can be ridden without one, and has its
#own, see FtpTest.TESTS)
UNSET_FTP_WATTS = 50


def zone_ftp():
    """The FTP the power zones are worked out from: the FTP, or while it isn't set the selected ramp test's
    unsetFtpWatts (UNSET_FTP_WATTS outside one)"""
    test = FtpTest.test() or {}
    return int(AppState.ftp) or test.get("unsetFtpWatts", UNSET_FTP_WATTS)

DEFAULTS = {
    "zones": {"0": "#d1d1d1", "1": "#38813b", "2": "#e0c200", "3": "#f07000", "4": "#d11e14"},
    "free": "#d9d9d9",
}
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
                value.update({zone: color for zone, color in changes[key].items() if zone in value})
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


def file_palette(value):
    """A gradients file's (or theme's "ftpColors") zone colors, with the defaults for any it leaves out"""
    zones = value.get("zones", {}) if isinstance(value, dict) else {}
    return [zones.get(zone, DEFAULTS["zones"][zone]) for zone in ZONES]


###----------------------------------------------------------------------------------------------
### The zones
###----------------------------------------------------------------------------------------------
#the power zones' upper limits as fractions of FTP (AppState.ftpRanges, see WorkoutSetupWindow.get_ftp_ranges)
POWER_ZONE_FTP = (.36, .87, 1.19, 1.87)
#the top zone is at its brightest this far past where it starts, as a share of the span of the zones below it
#(power: 187% + half of 36-187% = 262.5% of FTP; cadence: 115 + half of 60-115 = 142.5 RPM)
TOP_ZONE_SHARE = 0.5


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


def top_of(ranges):
    """Where the top zone is at its brightest"""
    return ranges[-1] + (ranges[-1] - ranges[0])*TOP_ZONE_SHARE


def zone_span(zone, ranges):
    """(start, end) of zone (0 - 4): where it's palest and where it's its color"""
    start = 0.0 if zone == 0 else ranges[zone - 1]
    end = ranges[zone] if zone < len(ranges) else top_of(ranges)
    return start, end


#Musical Cadence Mode's zones: zone "0" (grey) is under CADENCE_SCALE_BOTTOM_RPM, the top zone (red) from
#CADENCE_SCALE_TOP_RPM, the zones between them evenly spread
CADENCE_SCALE_BOTTOM_RPM = 40
CADENCE_SCALE_TOP_RPM = 115


def cadence_ranges(low):
    """Musical Cadence Mode's zone limits: zone "0" up to low, the top zone from CADENCE_SCALE_TOP_RPM, the ones
    between a step apart"""
    limits = len(ZONES) - 1
    low = min(low, CADENCE_SCALE_TOP_RPM - (limits - 1)) #(room for the zones between)
    step = (CADENCE_SCALE_TOP_RPM - low)/(limits - 1)
    return [low + step*k for k in range(limits)]


###----------------------------------------------------------------------------------------------
### Color math: OKLab/OKLCH (lightness, chroma, hue), where equal steps look equal
###----------------------------------------------------------------------------------------------
def _to_linear(c):
    return c/12.92 if c <= 0.04045 else ((c + 0.055)/1.055)**2.4


def _to_srgb(c):
    return 12.92*c if c <= 0.0031308 else 1.055*c**(1/2.4) - 0.055


@functools.lru_cache(maxsize=512)
def to_oklch(hexColor):
    """"#rrggbb" -> (L, C, h)"""
    color = QColor(hexColor)
    r, g, b = (_to_linear(c) for c in (color.redF(), color.greenF(), color.blueF()))
    l = (0.4122214708*r + 0.5363325363*g + 0.0514459929*b)**(1/3)
    m = (0.2119034982*r + 0.6806995451*g + 0.1073969566*b)**(1/3)
    s = (0.0883024619*r + 0.2817188376*g + 0.6299787005*b)**(1/3)
    L = 0.2104542553*l + 0.7936177850*m - 0.0040720468*s
    a = 1.9779984951*l - 2.4285922050*m + 0.4505937099*s
    bb = 0.0259040371*l + 0.7827717662*m - 0.8086757660*s
    return L, math.hypot(a, bb), math.atan2(bb, a)


def _linear_rgb(L, C, h):
    a, b = C*math.cos(h), C*math.sin(h)
    l = (L + 0.3963377774*a + 0.2158037573*b)**3
    m = (L - 0.1055613458*a - 0.0638541728*b)**3
    s = (L - 0.0894841775*a - 1.2914855480*b)**3
    return (4.0767416621*l - 3.3077115913*m + 0.2309699292*s,
            -1.2684380046*l + 2.6097574011*m - 0.3413193965*s,
            -0.0041960863*l - 0.7034186147*m + 1.7076147010*s)


def from_oklch(L, C, h):
    """(L, C, h) -> "#rrggbb"; a color past what the screen shows loses chroma (keeping its lightness and hue)"""
    L = min(1.0, max(0.0, L))
    rgb = _linear_rgb(L, C, h)
    for _ in range(40):
        if all(-1e-4 <= c <= 1 + 1e-4 for c in rgb):
            break
        C *= 0.92
        rgb = _linear_rgb(L, C, h)
    return "#" + "".join(f"{round(min(1.0, max(0.0, _to_srgb(min(1.0, max(0.0, c)))))*255):02x}" for c in rgb)


def _oklab(hexColor):
    L, C, h = to_oklch(hexColor)
    return L, C*math.cos(h), C*math.sin(h)


def difference(a, b):
    """How different two colors look (OKLab distance: hue counts, not just lightness)"""
    return math.dist(_oklab(a), _oklab(b))


def mix(a, b, amount):
    """a taken amount of the way to b ("#rrggbb")"""
    a, b = QColor(a), QColor(b)
    return QColor.fromRgbF(*(x + (y - x)*amount for x, y in
                             zip((a.redF(), a.greenF(), a.blueF()), (b.redF(), b.greenF(), b.blueF())))).name()


###----------------------------------------------------------------------------------------------
### The palette fitted to the theme
###----------------------------------------------------------------------------------------------
#the window's background gradient (ClassMainWindow.py, before the theme's colors replace them), and how far along it
#the colors have to stand out from: the gauges and spiral sit around the middle, the graph toward the end
WINDOW_GRADIENT = ("#28275e", "#9fc5d1")
BACKGROUND_AT = (0.5, 0.85)
#how different (see difference) a zone's color, and its pale end, have to look from those backgrounds
COLOR_MIN_DIFFERENCE = 0.16
PALE_MIN_DIFFERENCE = 0.09
#how different a zone's pale end has to look from the other zones' fades, from this far along them (their pale ends
#all look alike, washed out: that's what pale is), and how far toward its color it may be taken for that (every zone
#keeps some fade, even with colors near each other in hue all round)
ZONE_MIN_DIFFERENCE = 0.08
ZONE_FADE_FROM = 0.3
ZONE_MAX_PULL = 0.75
#the pale end: this share of the color's chroma, lightness this share of the way to white
PALE_CHROMA = 0.35
PALE_LIGHTNESS = 0.6
#a color moved more than this for the theme makes the palette "adjusted" for it
ADJUSTED_DIFFERENCE = 0.06


def theme_backgrounds(colors=None):
    """The backgrounds the colors have to stand out from, with a theme's colors (original -> replacement, the
    current theme's when None)"""
    if colors is None:
        import AppTheme
        colors = AppTheme.THEME.colors
    start, end = (colors.get(c, c) for c in WINDOW_GRADIENT)
    return tuple(mix(start, end, at) for at in BACKGROUND_AT)


def _stand_out(color, backgrounds):
    """color with its lightness moved (the least it takes) until it looks different enough from the backgrounds"""
    if min(difference(color, bg) for bg in backgrounds) >= COLOR_MIN_DIFFERENCE:
        return color
    L, C, h = to_oklch(color)
    for step in range(1, 101):
        for direction in (1, -1):
            moved = from_oklch(L + direction*step/100, C, h)
            if min(difference(moved, bg) for bg in backgrounds) >= COLOR_MIN_DIFFERENCE:
                return moved
    candidates = [from_oklch(L + step/100, C, h) for step in range(-100, 101)]
    return max(candidates, key=lambda c: min(difference(c, bg) for bg in backgrounds))


#how far toward its color a pale end is taken at a time, while fitting it
PALE_STEPS = 20


def _pale_at(color, t):
    """color washed out (t = 0: its palest) to color itself (t = 1)"""
    L1, C1, h = to_oklch(color)
    L0, C0 = L1 + (0.97 - L1)*PALE_LIGHTNESS, C1*PALE_CHROMA
    return from_oklch(L0 + (L1 - L0)*t, C0 + (C1 - C0)*t, h)


def _fade(pale, color, samples=10):
    """Colors along a zone's fade from ZONE_FADE_FROM to its color"""
    return [_blend(pale, color, k/samples) for k in range(samples + 1) if k/samples >= ZONE_FADE_FROM]


@functools.lru_cache(maxsize=256)
def fitted_palette(colors, backgrounds):
    """Each zone's (pale end, color, adjusted) for colors ("#rrggbb", one per zone) on backgrounds: each color
    moved to stand out from the backgrounds, each pale end taken back toward its color (never past it) until it
    stands out from them too and doesn't look like another zone's fade (washed out, colors near each other in
    hue, like orange and red, look alike: a fade starting on a color another zone has already shown)"""
    brights = [_stand_out(color, backgrounds) for color in colors]
    steps = [0]*len(colors) #how far toward its color each pale end has been taken (only ever further)
    maxPull = round(ZONE_MAX_PULL*PALE_STEPS)
    def fits(zone, step, fades):
        pale = _pale_at(brights[zone], step/PALE_STEPS)
        return (min(difference(pale, bg) for bg in backgrounds) >= PALE_MIN_DIFFERENCE and
                (step >= maxPull or all(difference(pale, c) >= ZONE_MIN_DIFFERENCE
                                        for other, fade in enumerate(fades) if other != zone for c in fade)))
    #(taking one pale end back changes the fade the others are compared with: again until none moves)
    for _ in range(len(colors)*PALE_STEPS):
        fades = [_fade(_pale_at(bright, step/PALE_STEPS), bright) for bright, step in zip(brights, steps)]
        moved = False
        for zone in range(len(brights)):
            while steps[zone] < PALE_STEPS and not fits(zone, steps[zone], fades):
                steps[zone] += 1
                moved = True
        if not moved:
            break
    return tuple((_pale_at(bright, step/PALE_STEPS), bright, difference(color, bright) > ADJUSTED_DIFFERENCE)
                 for color, bright, step in zip(colors, brights, steps))


def palette(colors=None, themeColors=None):
    """The zones' (pale end, color, adjusted), fitted to a theme's colors (the current palette and theme by default)"""
    if colors is None:
        colors = [_current("zones")[zone] for zone in ZONES]
    return fitted_palette(tuple(c.lower() for c in colors), theme_backgrounds(themeColors))


def adjusted_zones(colors=None, themeColors=None):
    """The zones ("0" - "4") whose colors were moved to stand out on the theme"""
    return [zone for zone, (_, _, adjusted) in zip(ZONES, palette(colors, themeColors)) if adjusted]


def _blend(pale, color, t):
    (L0, C0, h0), (L1, C1, h1) = to_oklch(pale), to_oklch(color)
    if C0 < 0.02: #(a grey has no hue to keep)
        h0 = h1
    dh = (h1 - h0 + math.pi) % (2*math.pi) - math.pi
    return from_oklch(L0 + (L1 - L0)*t, C0 + (C1 - C0)*t, h0 + dh*t)


def zone_position(value, ranges=None):
    """(zone 0 - 4, how far through it 0 - 1) for a power (or cadence, with cadence ranges)"""
    ranges = (AppState.ftpRanges if ranges is None else ranges) or power_ranges()
    zone = min(len(ZONES) - 1, bisect.bisect_right(ranges, value))
    start, end = zone_span(zone, ranges)
    return zone, min(1.0, max(0.0, (value - start)/(end - start))) if end > start else 1.0


#zone_hex's colors, worked out once per palette (the graph asks for hundreds of bars every second)
_colorCache = {}


def zone_hex(value, ranges=None, fitted=None):
    """The color ("#rrggbb") for a power (or a cadence, with cadence ranges): its zone's pale end where the zone
    starts, its zone's color where it ends"""
    ranges = (AppState.ftpRanges if ranges is None else ranges) or power_ranges()
    fitted = fitted or palette()
    key = (value, tuple(ranges), fitted)
    color = _colorCache.get(key)
    if color is None:
        if len(_colorCache) > 4000:
            _colorCache.clear()
        zone, t = zone_position(value, ranges)
        pale, bright, _ = fitted[zone]
        color = _colorCache[key] = _blend(pale, bright, t)
    return color


def zone_color(value, ranges=None):
    """zone_hex as a QColor"""
    return QColor(zone_hex(value, ranges))


def pale_hex(zone):
    """A zone's (0 - 4) pale end"""
    return palette()[zone][0]


def zone_stops(ranges, steps=6):
    """A gauge band's (value, QColor) stops: each zone fading from its pale end to its color, a hard edge where
    the next zone starts"""
    fitted = palette()
    stops = []
    for zone in range(len(ZONES)):
        start, end = zone_span(zone, ranges)
        pale, bright, _ = fitted[zone]
        edge = (end - start)*1e-4 if zone < len(ZONES) - 1 else 0
        for i in range(steps + 1):
            stops.append((start + (end - start)*i/steps - (edge if i == steps else 0),
                          QColor(_blend(pale, bright, i/steps))))
    return stops


def colors_key(ranges=None):
    """Changes whenever the colors zone_color gives for these ranges would (for drawings kept until then)"""
    ranges = (AppState.ftpRanges if ranges is None else ranges) or power_ranges()
    return tuple(ranges), palette()


###----------------------------------------------------------------------------------------------
### What each part draws with
###----------------------------------------------------------------------------------------------
def gauge_arc(power, ranges=None):
    """The target arc's two colors for a power, without the "#" (the gauge adds transparency): its zone's pale end
    to the power's color"""
    if power == 0:
        free = _current("free")[1:]
        return free, free
    zone, _ = zone_position(power, ranges)
    return pale_hex(zone)[1:], zone_hex(power, ranges)[1:]


def gauge_ring(power, ranges=None):
    """The ring color for the power being ridden, without the "#" """
    if power == 0:
        return _current("free")[1:]
    return zone_hex(power, ranges)[1:]


def all_colors():
    """Every color in use here, as "#rrggbb" """
    return {color.lower() for color in [*_current("zones").values(), _current("free")]}


#the block labels' text: always dark on their light zone colors, whatever a theme does with the app's black
#text (a theme's colors can still change it, it's its own color)
BLOCK_TEXT = "#111111"


def block_colors(power0, power1=0, ranges=None):
    """A block label's background colors (start, end) as "#rrggbb": a range from its low power's color to its high
    power's, a single power from its color to its zone's pale end"""
    start = zone_hex(power0, ranges)
    end = zone_hex(power1, ranges) if power1 else pale_hex(zone_position(power0, ranges)[0])
    return start, end


def block_css(power0, power1=0, ranges=None):
    """A block label's background (see block_colors)"""
    start, end = block_colors(power0, power1, ranges)
    return f"background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 {start} stop:1 {end}); color:{BLOCK_TEXT};"


def free_ride_colors():
    return _current("free"), "#ffffff"


def free_ride_css():
    start, end = free_ride_colors()
    return f"background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 {start} stop:1 {end}); color:{BLOCK_TEXT};"


#the graph's bars: a little see-through (the grid shows), the ones not ridden yet mixed with grey, more so paused
GRAPH_ALPHA = 0xcc
GRAPH_GREYS = {"paused": ("#9a9a9a", 0.70), "upcoming": ("#d3d3d3", 0.45), "past": None}


def graph_color(value, ranges=None, state="past"):
    """A bar's color for its target power (or cadence) in "paused", "upcoming" or "past" """
    color = zone_hex(value, ranges)
    grey = GRAPH_GREYS[state]
    if grey is not None:
        color = mix(color, *grey)
    color = QColor(color)
    color.setAlpha(GRAPH_ALPHA)
    return color


def update_graph_brushes():
    """AppState's bar colorers, (value, ranges) -> QColor (see ClassSolidColoredColumns.py)"""
    for name, state in (("graphWorkoutBrushLight", "paused"), ("graphWorkoutBrushMid", "upcoming"),
                        ("graphWorkoutBrushDark", "past")):
        setattr(AppState, name, functools.partial(graph_color, state=state))


###----------------------------------------------------------------------------------------------
### The graphs' sensor lines
###----------------------------------------------------------------------------------------------
#the sensor lines' colors (the HUD and synopsis graphs)
SENSOR_COLORS = {"power": "#0000ff", "hr": "#ff0000", "cadence": "#ffff00", "speed": "#ffffff"}
#each line this wide over a halo this wide, so it shows over any bar's or background's color: the halo light under a
#dark line (OKLCH lightness below HALO_LIGHT_BELOW, like the blue), dark under a light one
SENSOR_LINE_PX = 2
SENSOR_HALO_PX = 3.5
HALO_DARK = "#101018"
HALO_LIGHT = "#f2f2f6"
HALO_ALPHA = 0x80
HALO_LIGHT_BELOW = 0.55


def halo_color(color, alpha=HALO_ALPHA):
    """The halo (QColor) that sets color ("#rrggbb") apart from whatever is under it"""
    halo = QColor(HALO_LIGHT if to_oklch(color)[0] < HALO_LIGHT_BELOW else HALO_DARK)
    halo.setAlpha(alpha)
    return halo


def _cosmetic_pen(color, width):
    #(cosmetic: as wide on screen whatever the graph's scale, like pyqtgraph's own mkPen)
    pen = QPen(QColor(color), width)
    pen.setCosmetic(True)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    return pen


def sensor_pens(color, width=SENSOR_LINE_PX, haloWidth=SENSOR_HALO_PX):
    """(pen, halo pen) for a graph's sensor line in color ("#rrggbb"): the halo goes to the curve as its shadowPen"""
    return _cosmetic_pen(color, width), _cosmetic_pen(halo_color(color), haloWidth)


def marker_outline(color, width=1):
    """The pen outlining a sensor's current reading marker in color ("#rrggbb"), its line's halo color"""
    return _cosmetic_pen(halo_color(color, 0xff), width)


###----------------------------------------------------------------------------------------------
### Redrawing what shows them
###----------------------------------------------------------------------------------------------
_watchers = []
_lastApplied = None
_lastBackgrounds = None


def watch(obj, methodName):
    """obj.methodName() is called whenever the colors change (held weakly: a deleted window drops out)"""
    _watchers.append((weakref.ref(obj), methodName))


def unwatch(obj):
    """obj isn't redrawn when the colors change any more (it redraws itself, e.g. PreviewRide.WorkoutPreview's)"""
    _watchers[:] = [(ref, methodName) for ref, methodName in _watchers if ref() is not obj]


def notify():
    global _lastApplied, _lastBackgrounds
    _values.clear()
    _lastApplied = copy.deepcopy(_theme_changes())
    _lastBackgrounds = theme_backgrounds()
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
    """After a theme is loaded or cleared (AppTheme.apply): the colors, or the background they're fitted to, changed"""
    if _theme_changes() != _lastApplied or theme_backgrounds() != _lastBackgrounds:
        notify()

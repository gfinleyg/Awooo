"""
AppTheme.py

Font and color overrides for the whole app, made in LayoutViewer.py and saved as a theme .json:

  {
    "fonts": {                                   - per font role, see ROLE_TITLES
      "main": {"family": "Anybody Thin", "scale": 100, "weight": "Original"},
      ...
    },
    "colors": {"#9fc5d1": "#ffcc66", ...},       - original color -> replacement
    "ftpColors": {"zones": {"2": "#e0c200"}, ...} - power zone colors, see FtpColors.py
  }

The colors and the power/cadence zone gradients can also be saved and loaded on their own (see PARTS): a
colors file holds just "colors", a gradients file just "ftpColors". Loading one replaces just that part of
the theme in use, and it stays on top of it until a whole theme is loaded or the theme is cleared.

With no theme picked (or after Clear Theme) the app uses DEFAULT_THEME_FILE, else BUILT_IN_THEME.

install() (once, before any window is built) routes every stylesheet the app sets through the
current theme and records the original, so a theme can be loaded, changed or cleared at any time:
apply() re-sets every recorded original through the new theme. The few pieces that paint
themselves are covered the same way: the smooth progress bars, the spiral, the gauge numbers
and the synopsis max value labels.
"""

import copy
import json
import math
import os
import re

from PyQt6.QtCore import QTimer
from PyQt6.QtGui import QColor, QFont, QFontDatabase, QFontInfo, QPainter, QPen
from PyQt6.QtWidgets import QAbstractButton, QApplication, QGraphicsView, QLabel, QWidget

from AppState import AppState
import ClassAntLogger


FONT_DIRS = [os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts"),
             os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts", "variable")]

#font roles, keyed by the family the app's stylesheets name
ROLE_MAIN = "main"
ROLE_BUTTON = "button"
ROLE_GAUGE = "gauge"
ROLE_FAMILIES = {"orbitron": ROLE_MAIN, "segoe ui black": ROLE_BUTTON}
ROLE_TITLES = {ROLE_MAIN: "Text", ROLE_BUTTON: "Buttons + stats", ROLE_GAUGE: "Gauge numbers"}
#The app's own look, with no theme picked (and what Reset / Clear Theme go back to): the screens'
#code and stylesheets still name Orbitron and Segoe UI Black, which mark the font rows (ROLE_FAMILIES)
#that these fonts replace
BUILT_IN_THEME = {
    "fonts": {
        ROLE_MAIN: {"family": "Roboto Flex", "scale": 100, "weight": "Original"},
        ROLE_BUTTON: {"family": "Roboto Flex", "scale": 100, "weight": "Original"},
        ROLE_GAUGE: {"family": "Roboto Flex", "scale": 100, "weight": "Original"},
    },
    "colors": {},
}
ROLE_DEFAULT_FAMILY = {role: settings["family"] for role, settings in BUILT_IN_THEME["fonts"].items()}
WEIGHTS = ["Original", "100", "200", "300", "400", "500", "600", "700", "800", "900"]

#saved from LayoutViewer.py ("Save as default"); used whenever no theme is picked, next to the
#settings file (the exe's folder in a frozen build)
DEFAULT_THEME_FILE = "defaultTheme.json"

#the colors/ and gradients/ files to pick from, next to the settings file (the exe's folder in a frozen
#build, so they can be added to without a rebuild)
THEMES_DIR = os.path.abspath("themes")
#where each of PARTS' files are
PART_DIRS = {"colors": os.path.join(THEMES_DIR, "colors"), "ftpColors": os.path.join(THEMES_DIR, "gradients")}

#the synopsis max value labels paint this background themselves
PLOT_LABEL_BG = "#bdd6de"

HEX_RE = re.compile(r"#([0-9a-fA-F]{8}|[0-9a-fA-F]{6})\b")
NAMED_RE = re.compile(r"((?:background-)?color|border-color|background)(\s*:\s*)(black|white|grey|gray|red|green|blue)\b", re.I)
#font shorthand (font: [modifiers] <size><unit> '<family>'), or any other pixel length
FONT_OR_PX_RE = re.compile(r"(?P<prefix>font\s*:\s*)(?P<mods>[^;}\"']*?)(?P<size>\d+(?:\.\d+)?)(?P<unit>pt|px)\s*"
                           r"(?P<q>['\"])(?P<family>[^'\"]+)(?P=q)|(?<![\w.#])(?P<length>-?\d+(?:\.\d+)?)px\b", re.I)
WEIGHT_TOKEN_RE = re.compile(r"\b(\d00|bold|normal)\b", re.I)


class Theme:
    def __init__(self):
        self.reset()

    def reset(self):
        """The app's own look (BUILT_IN_THEME)"""
        self.fonts = copy.deepcopy(BUILT_IN_THEME["fonts"])
        self.colors = dict(BUILT_IN_THEME["colors"]) #original "#rrggbb" -> replacement "#rrggbb"
        self.ftp = {} #power zone colors changed from FtpColors.DEFAULTS

    def to_json(self, part=None):
        """The whole theme, or just one of PARTS"""
        if part == "colors":
            return {"colors": self.colors}
        if part == "ftpColors":
            return {"ftpColors": self.ftp}
        return {"fonts": self.fonts, "colors": self.colors, "ftpColors": self.ftp}

    def set_part(self, part, value):
        """Replaces one of PARTS (as read from a file)"""
        if part == "colors":
            self.colors = {k.lower(): v.lower() for k, v in value.items()}
        else:
            #(older files' entries, from before each zone had one color, are dropped: the defaults stand in)
            import FtpColors
            self.ftp = {key: copy.deepcopy(v) for key, v in value.items() if key in FtpColors.DEFAULTS}

    def from_json(self, data):
        self.reset()
        for role, settings in data.get("fonts", {}).items():
            if role in self.fonts:
                self.fonts[role].update(settings)
        #(older themes' "layouts", per screen size part sizes, are ignored: the app scales itself to fit)
        for part in PARTS:
            if isinstance(data.get(part), dict):
                self.set_part(part, data[part])

    def map_hex(self, text):
        #"#rrggbb" or Qt's "#aarrggbb"; the alpha is kept
        alpha, rgb = ("", text) if len(text) == 7 else (text[:3], "#" + text[3:])
        new = self.colors.get(rgb.lower())
        return alpha + new[1:] if new and alpha else (new or text)

    def map_color(self, color):
        #anything the progress bar/spiral color setters take: a hex string or an (r, g, b) tuple
        if isinstance(color, str) and HEX_RE.fullmatch(color):
            return self.map_hex(color)
        if isinstance(color, tuple) and len(color) == 3:
            return self.colors.get(QColor(*color).name(), color)
        return color

    def map_font(self, font, scale=1.0):
        role = ROLE_FAMILIES.get(font.family().lower())
        if role is None:
            return font
        settings = self.fonts[role]
        themed = QFont(font)
        themed.setFamily(settings["family"])
        if font.pointSizeF() > 0:
            themed.setPointSizeF(font.pointSizeF()*settings["scale"]/100*scale)
        if settings["weight"] != "Original":
            themed.setWeight(QFont.Weight(int(settings["weight"])))
        return themed

    def css(self, text, scale=1.0):
        """text with this theme's fonts and colors; scale also multiplies every font size and pixel
        length (padding, radii, ...) for a smaller or larger layout"""
        def fontOrLength(m):
            if m.group("length") is not None:
                length = float(m.group("length"))
                if not length:
                    return "0px"
                #never rounds a non-zero length down to nothing
                return f"{int(math.copysign(max(1, round(abs(length)*scale)), length))}px"
            prefix, mods, size, unit, family = (m.group(g) for g in ("prefix", "mods", "size", "unit", "family"))
            role = ROLE_FAMILIES.get(family.strip().lower())
            settings = self.fonts[role] if role else {"family": family, "scale": 100, "weight": "Original"}
            newSize = max(1, round(float(size) * settings["scale"] / 100 * scale))
            if settings["weight"] != "Original":
                if WEIGHT_TOKEN_RE.search(mods):
                    mods = WEIGHT_TOKEN_RE.sub(settings["weight"], mods)
                else:
                    mods = settings["weight"] + " " + mods
            return f"{prefix}{mods}{newSize}{unit} '{settings['family']}'"
        text = FONT_OR_PX_RE.sub(fontOrLength, text)
        text = HEX_RE.sub(lambda m: self.map_hex(m.group(0)), text)
        def named(m):
            new = self.colors.get(QColor(m.group(3)).name())
            return m.group(1) + m.group(2) + (new or m.group(3))
        return NAMED_RE.sub(named, text)


def colors_in_css(text):
    found = {("#" + m.group(1)[-6:]).lower() for m in HEX_RE.finditer(text)}
    found.update(QColor(m.group(3)).name() for m in NAMED_RE.finditer(text))
    return found


def colors_in_value(color):
    if isinstance(color, str) and HEX_RE.fullmatch(color):
        return {("#" + color[-6:]).lower()}
    if isinstance(color, tuple) and len(color) == 3:
        return {QColor(*color).name()}
    return set()


THEME = Theme()

#the parts of a theme that can be saved and loaded on their own: theme file key -> what to call it
PARTS = {"colors": "colors", "ftpColors": "power/cadence gradients"}
#the colors/gradients files loaded on top of the theme: part -> path (see load_file)
_partFiles = {}
#the whole theme file loaded (None: none picked, the default theme, see clear)
_themeFile = None


###----------------------------------------------------------------------------------------------
### Hooks: record each original, set it through the theme
###----------------------------------------------------------------------------------------------
original_set_stylesheet = QWidget.setStyleSheet
original_set_text = QAbstractButton.setText
_origAppSetStyleSheet = QApplication.setStyleSheet
_appSheet = None
_appliedAppFont = None #QFont.key() of the app font apply() last set
_installed = False
_registeredFamilies = None
#set by install(), the classes it hooks
_SmoothProgressBar = _QSpiralProgressBar = _AnalogGaugeWidget = _SynopsisWindow = _PlotLabel = None
_origSetBarColors = _origSpbLineColor = _origRescale = _origPlotLabelSetFont = None


#layout scale (see AppLayout.py): 1.0 is the size the screens were designed at
ui_scale = 1.0


def layout_shrink(widget):
    """How much smaller than the layout scale a popup window (and everything in it) is drawn so it fits the
    screen (AppLayout.fit_dialog sets it on the window; 1: not shrunk)"""
    shrink = 1.0
    while widget is not None:
        shrink *= getattr(widget, "_layoutShrink", 1.0)
        widget = widget.parentWidget()
    return shrink


def widget_scale(widget):
    """The layout scale, for widgets but those marked _layoutExempt (and everything in them, e.g.
    LayoutViewer's controls), which are never scaled; less in a popup window shrunk to fit the screen"""
    start = widget
    while widget is not None:
        if getattr(widget, "_layoutExempt", False):
            return 1.0
        widget = widget.parentWidget()
    return ui_scale*layout_shrink(start)


def sheet_source(widget):
    #a widget can also carry a stylesheet in front of its own (LayoutViewer puts the app wide one
    #on its display window, so it doesn't reach the viewer's control window)
    prefix = getattr(widget, "_themePrefixSheet", "")
    own = widget._themeOrigSheet
    if (prefix or getattr(widget, "_layoutFitPx", None)) and own.strip() and "{" not in own:
        #bare declarations (no selector) can't follow rules, so aim them at just this widget
        if not widget.objectName():
            widget.setObjectName(f"themed{id(widget)}")
        own = f"#{widget.objectName()}{{{own}}}"
    return prefix + "\n" + own


def themed_sheet(widget):
    """The stylesheet a widget gets: its own (and any prefix) through the theme at its scale, then the
    smaller font size AppLayout gave it if its text didn't fit (in px already, so after the scaling)"""
    sheet = THEME.css(sheet_source(widget), widget_scale(widget))
    fitPx = getattr(widget, "_layoutFitPx", None)
    if fitPx:
        if not widget.objectName():
            widget.setObjectName(f"themed{id(widget)}")
        sheet += f"\n#{widget.objectName()}{{font-size: {fitPx}px;}}"
    return sheet


def install():
    global _installed, _SmoothProgressBar, _QSpiralProgressBar, _AnalogGaugeWidget, _SynopsisWindow, _PlotLabel
    global _origSetBarColors, _origSpbLineColor, _origRescale, _origPlotLabelSetFont
    if _installed:
        return
    _installed = True
    #imported here, these modules import the title bar which imports this one
    from ClassSmoothProgressBar import SmoothProgressBar
    from QSpiralProgressBar import QSpiralProgressBar
    from ClassAnalogGauge import AnalogGaugeWidget
    import ClassSynopsisWindow
    _SmoothProgressBar, _QSpiralProgressBar, _AnalogGaugeWidget = SmoothProgressBar, QSpiralProgressBar, AnalogGaugeWidget
    _SynopsisWindow, _PlotLabel = ClassSynopsisWindow.SynopsisWindow, ClassSynopsisWindow.PlotLabel

    def setStyleSheet(self, sheet):
        self._themeOrigSheet = sheet
        original_set_stylesheet(self, themed_sheet(self))
    QWidget.setStyleSheet = setStyleSheet

    #new text on a label laid out by AppLayout gets checked it still fits (buttons: setText below). The same text
    #again (the workout screen re-sets some every tick) is only checked if the label's size changed since; window
    #resizes and monitor changes refit everything anyway (see AppLayout._TextFitter and layout_window)
    _origLabelSetText = QLabel.setText
    def labelSetText(self, text):
        unchanged = text == self.text() and self.size() == getattr(self, "_layoutFitSize", None)
        _origLabelSetText(self, text)
        if getattr(self, "_layoutFitWatch", False) and not unchanged:
            self._layoutFitSize = self.size()
            import AppLayout
            AppLayout.text_changed(self)
    QLabel.setText = labelSetText

    def appSetStyleSheet(self, sheet):
        global _appSheet
        _appSheet = sheet
        _origAppSetStyleSheet(self, THEME.css(sheet, ui_scale))
    QApplication.setStyleSheet = appSetStyleSheet

    #a button shown as a symbol (AppLayout.set_symbol) gets the symbol for each new text
    #(the same text again is skipped as for labels above; a symbol is redrawn on resize by its own event filter)
    def setText(self, text):
        sameText = text == getattr(self, "_themeOrigText", None)
        self._themeOrigText = text
        if getattr(self, "_layoutSymbol", False):
            if not sameText:
                import AppLayout
                AppLayout.show_symbol(self)
        else:
            unchanged = sameText and self.size() == getattr(self, "_layoutFitSize", None)
            original_set_text(self, text)
            if getattr(self, "_layoutFitWatch", False) and not unchanged:
                self._layoutFitSize = self.size()
                import AppLayout
                AppLayout.text_changed(self)
    QAbstractButton.setText = setText

    _origSetBarColors = SmoothProgressBar.setBarColors
    def setBarColors(self, chunkColor, barColor):
        self._themeOrigBar = (chunkColor, barColor)
        _origSetBarColors(self, THEME.map_color(chunkColor), THEME.map_color(barColor))
    SmoothProgressBar.setBarColors = setBarColors

    _origSpbLineColor = QSpiralProgressBar.spb_lineColor
    def spb_lineColor(self, color):
        self._themeOrigLine = color
        _origSpbLineColor(self, tuple(THEME.map_color(c) for c in color))
    QSpiralProgressBar.spb_lineColor = spb_lineColor

    #the gauges work out their font sizes here (from their size), so scale them here too
    _origRescale = AnalogGaugeWidget.rescale_method
    def rescale_method(self):
        _origRescale(self)
        settings = THEME.fonts[ROLE_GAUGE]
        self.scale_fontname = self.value_fontname = settings["family"]
        self.scale_fontsize *= settings["scale"]/100
        self.value_fontsize *= settings["scale"]/100
    AnalogGaugeWidget.rescale_method = rescale_method

    #the gauges paint their tick marks and scale numbers themselves (black, on the window background): in
    #the theme's colors for them. The value in the middle is on the gauge's own white face, so it stays as is
    def themed_colors(method, attrs):
        drawn = getattr(AnalogGaugeWidget, method)
        def draw(self):
            originals = {attr: getattr(self, attr) for attr in attrs}
            for attr, color in originals.items():
                themed = QColor(THEME.map_hex(color.name()))
                themed.setAlpha(color.alpha())
                setattr(self, attr, themed)
            try:
                drawn(self)
            finally:
                for attr, color in originals.items():
                    setattr(self, attr, color)
        setattr(AnalogGaugeWidget, method, draw)
    themed_colors("draw_big_scaled_marker", ("bigScaleMarker",))
    themed_colors("create_fine_scaled_marker", ("fineScaleColor",))
    themed_colors("create_scale_marker_values_text", ("ScaleValueColor",))

    _origPlotLabelSetFont = ClassSynopsisWindow.PlotLabel.setFont
    def plotLabelSetFont(self, font):
        self._themeOrigFont = QFont(font)
        _origPlotLabelSetFont(self, THEME.map_font(font, ui_scale))
    ClassSynopsisWindow.PlotLabel.setFont = plotLabelSetFont

    def plotLabelPaint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QPen(self.borderColor, self.borderWidth))
        painter.setBrush(QColor(THEME.map_hex(PLOT_LABEL_BG)))
        painter.drawRoundedRect(self.boundingRect().adjusted(0.6, 0.6, -0.6, -0.6), self.RADIUS, self.RADIUS)
        #the text in the theme's color for black (only set when it changes: setting it repaints)
        text = QColor(THEME.map_hex("#000000"))
        if self.brush().color() != text:
            self.setBrush(text)
        super(ClassSynopsisWindow.PlotLabel, self).paint(painter, option, widget)
    ClassSynopsisWindow.PlotLabel.paint = plotLabelPaint


def apply():
    """Re-sets everything already built through the current theme"""
    app = QApplication.instance()
    register_fonts() #the theme's fonts, the app's own included (once; needs the app to exist)
    if _appSheet is not None:
        _origAppSetStyleSheet(app, THEME.css(_appSheet, ui_scale))
    #app wide font, buttons keep the system default (AppState.defaultFont, see AntWorkout.py)
    main = THEME.fonts[ROLE_MAIN]
    appFont = QFont(main["family"])
    if main["scale"] != 100 or ui_scale != 1.0:
        appFont.setPointSizeF(appFont.pointSizeF()*main["scale"]/100*ui_scale)
    #only when it changed: setting the app font re-polishes every widget in the app
    global _appliedAppFont
    if appFont.key() != _appliedAppFont:
        _appliedAppFont = appFont.key()
        app.setFont(appFont)
        if AppState.defaultFont is not None:
            app.setFont(AppState.defaultFont, "QAbstractButton")
    for widget in app.allWidgets():
        if hasattr(widget, "_themeOrigSheet"):
            original_set_stylesheet(widget, themed_sheet(widget))
        if hasattr(widget, "_themeOrigBar"):
            _origSetBarColors(widget, *(THEME.map_color(c) for c in widget._themeOrigBar))
            widget.update()
        if hasattr(widget, "_themeOrigLine"):
            _origSpbLineColor(widget, tuple(THEME.map_color(c) for c in widget._themeOrigLine))
            widget.update()
        if hasattr(widget, "_themeColors"): #(draws its _themeColors through the theme itself, e.g. the HUD's spiral)
            widget.update()
        if _AnalogGaugeWidget is not None and isinstance(widget, _AnalogGaugeWidget):
            widget.rescale_method()
            widget.paintCacheDirty = True #(its tick marks and numbers may have changed color)
            widget.update()
        if isinstance(widget, QGraphicsView) and widget.scene() is not None:
            for item in widget.scene().items():
                if hasattr(item, "_themeOrigFont"):
                    _origPlotLabelSetFont(item, THEME.map_font(item._themeOrigFont, ui_scale))
                    item.update()
        if _SynopsisWindow is not None and isinstance(widget, _SynopsisWindow):
            QTimer.singleShot(0, widget.layout_max_labels)
        if type(widget).__name__ == "WorkoutGraph":
            theme_graph(widget)
        if getattr(widget, "_layoutSymbol", False):
            #button symbols are drawn in the theme's button text color
            import AppLayout
            AppLayout.show_symbol(widget)
    #power zone colors: redrawn by whatever shows them, when the theme changed them
    import FtpColors
    FtpColors.notify_if_changed()


def theme_graph(graph):
    """A graph's title and axes (the synopsis' "Workout Stats", "Power (Watts)", the time ticks, ...)
    in the Text font row's font, size and weight, and the theme's colors"""
    settings = THEME.fonts[ROLE_MAIN]
    scale = widget_scale(graph)
    factor = settings["scale"]/100*scale
    title = graph.plotScreen.titleLabel
    if not hasattr(graph, "_themeTitle"):
        graph._themeTitle = (title.text, title.opts.get("size", "20pt"), title.opts.get("color"))
    text, size, color = graph._themeTitle
    #a graph with a title has its axis labels ("Power (Watts)", ...) at the title's size too, unless it
    #sets its own (axisTitlePoints, the synopsis: its max labels' size); the tick numbers keep their own size
    titlePoints = None
    if text and title.isVisible():
        titlePoints = float(str(size).rstrip("pt"))*factor
        title.item.setFont(THEME.map_font(QFont("Orbitron"), scale))
        title.setText(text, color=THEME.map_color(color), size=f"{titlePoints:.1f}pt")
        #the title's row as tall as the title (pyqtgraph pins it at 30px, which cuts off a bigger one)
        height = math.ceil(title.itemRect().height())
        title.setMaximumHeight(height)
        graph.plotScreen.layout.setRowFixedHeight(0, height)
    labelPoints = graph.axisTitlePoints*factor if getattr(graph, "axisTitlePoints", None) else titlePoints
    #the tick numbers: pyqtgraph's own grey unless the theme changes black, then that
    black = THEME.colors.get("#000000")
    for name in ("left", "right", "bottom"):
        axis = graph.plotScreen.getAxis(name)
        if not hasattr(axis, "_themeOrig"):
            tickFont = axis.style.get("tickFont")
            axis._themeOrig = (QFont(tickFont) if tickFont is not None else QFont(graph.font()),
                               axis.labelText, axis.labelUnits, dict(axis.labelStyle), QPen(axis.textPen()))
        tickFont, labelText, labelUnits, labelStyle, textPen = axis._themeOrig
        axis.setTextPen(QPen(QColor(black)) if black else textPen)
        if "color" in labelStyle:
            labelStyle = {**labelStyle, "color": THEME.map_color(labelStyle["color"])}
        font = QFont(tickFont)
        font.setFamily(settings["family"])
        if tickFont.pointSizeF() > 0:
            font.setPointSizeF(tickFont.pointSizeF()*factor)
        if settings["weight"] != "Original":
            font.setWeight(QFont.Weight(int(settings["weight"])))
        axis.setTickFont(font)
        if labelText:
            style = {**labelStyle, "font-family": settings["family"]}
            match = re.match(r"\s*(\d+(?:\.\d+)?)px", str(labelStyle.get("font-size", "")))
            if labelPoints:
                style["font-size"] = f"{labelPoints:.1f}pt"
            elif match:
                style["font-size"] = f"{float(match.group(1))*factor:.1f}px"
            if settings["weight"] != "Original":
                style["font-weight"] = settings["weight"]
            labelFont = QFont(font)
            if labelPoints:
                labelFont.setPointSizeF(labelPoints)
            axis.label.setFont(labelFont)
            axis.setLabel(labelText, labelUnits, **style)


def register_fonts():
    """Registers every font in fonts/ and fonts/variable/ (once), returns their family names"""
    global _registeredFamilies
    if _registeredFamilies is None:
        families = []
        for folder in FONT_DIRS:
            if not os.path.isdir(folder):
                continue
            for name in sorted(os.listdir(folder)):
                if name.lower().endswith((".ttf", ".otf")):
                    fontId = QFontDatabase.addApplicationFont(os.path.join(folder, name))
                    for family in QFontDatabase.applicationFontFamilies(fontId) if fontId >= 0 else []:
                        if family not in families:
                            families.append(family)
        #some files report a family name the font engine can't actually find (it would silently
        #fall back to another font), so only keep the ones that resolve to themselves
        families = [f for f in families if QFontInfo(QFont(f)).family().lower() == f.lower()]
        _registeredFamilies = sorted(families, key=str.lower)
    return list(_registeredFamilies)


def _read(path):
    with open(path) as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError("not a theme file")
    return data


def file_kind(data):
    """"theme" for a whole theme (it has fonts), else the PARTS it holds ("colors", "ftpColors")"""
    if "fonts" in data:
        return "theme"
    parts = [part for part in PARTS if isinstance(data.get(part), dict)]
    if not parts:
        raise ValueError("not a theme, colors or gradients file")
    return parts[0] if len(parts) == 1 else "theme"


def load_file(path):
    """Loads a theme .json (picked by the user) and applies it right away: a whole theme replaces the theme
    (and drops any colors/gradients files loaded on top of the old one), a colors or gradients file replaces
    just that part. Returns the file's kind (see file_kind); raises OSError/ValueError for a bad file"""
    global _themeFile
    data = _read(path)
    kind = file_kind(data)
    register_fonts()
    if kind == "theme":
        THEME.from_json(data)
        _partFiles.clear()
        _themeFile = path
    else:
        THEME.set_part(kind, data[kind])
        _partFiles[kind] = path
    refresh()
    return kind


def _reload_part_files():
    """The colors/gradients files back on top of a theme that was just loaded underneath them"""
    for part, path in list(_partFiles.items()):
        try:
            THEME.set_part(part, _read(path)[part])
        except (OSError, ValueError, KeyError) as e:
            ClassAntLogger.log_event("ERROR", f"Failed to reload {PARTS[part]} {path}: {e}")
            _partFiles.pop(part)


def part_files():
    """{part: path} of the colors/gradients files loaded on top of the theme"""
    return dict(_partFiles)


def theme_file():
    """The whole theme file in use (None: the default theme)"""
    return _themeFile


def base_part(part):
    """The theme's own colors or gradients (one of PARTS, as a theme file holds it), without any colors/gradients
    file on top"""
    path = _themeFile or default_source()
    data = BUILT_IN_THEME
    if path:
        try:
            data = _read(path)
        except (OSError, ValueError) as e:
            ClassAntLogger.log_event("ERROR", f"Failed to reload {PARTS[part]} from {path}: {e}")
    return copy.deepcopy(data[part]) if isinstance(data.get(part), dict) else {}


def drop_part(part):
    """Back to the theme's own colors or gradients (one of PARTS): the colors/gradients file loaded on top of it is
    taken off, applied right away"""
    _partFiles.pop(part, None)
    THEME.set_part(part, base_part(part))
    refresh()


def snapshot():
    """The theme in use, to go back to with restore()"""
    return copy.deepcopy(THEME.to_json()), dict(_partFiles), _themeFile


def restore(saved):
    """Back to a snapshot(), applied right away"""
    global _themeFile
    data, parts, _themeFile = saved
    THEME.from_json(data)
    _partFiles.clear()
    _partFiles.update(parts)
    refresh()


def save_file(path, part=None):
    """Saves the whole theme, or just one of PARTS (a colors or gradients file)"""
    with open(path, "w") as f:
        json.dump(THEME.to_json(part), f, indent=2)


def reset():
    """Back to the app's own fonts, colors and layout"""
    THEME.reset()
    refresh()


def refresh():
    """Re-applies the theme, and rescales the app's window to its screen when there is one"""
    import AppLayout
    AppLayout.layout_window()


###----------------------------------------------------------------------------------------------
### No theme picked: DEFAULT_THEME_FILE, else the built-in theme
###----------------------------------------------------------------------------------------------
def default_source():
    """The file the app uses when no theme is picked (None: the built-in theme)"""
    return DEFAULT_THEME_FILE if os.path.exists(DEFAULT_THEME_FILE) else None


def _load_default():
    """Loads the default theme into THEME (not applied)"""
    path = default_source()
    if path:
        try:
            data = _read(path)
            register_fonts()
            THEME.from_json(data)
            _reload_part_files()
            return
        except (OSError, ValueError) as e:
            ClassAntLogger.log_event("ERROR", f"Failed to load default theme {path}: {e}")
    THEME.reset()
    _reload_part_files()


def clear(keepParts=False):
    """No theme picked: the default theme, applied right away; without the colors/gradients files loaded
    on top unless keepParts"""
    global _themeFile
    _themeFile = None
    if not keepParts:
        _partFiles.clear()
    _load_default()
    refresh()

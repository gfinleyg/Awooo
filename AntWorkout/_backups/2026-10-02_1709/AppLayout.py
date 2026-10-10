"""
AppLayout.py

Fits the screens to the screen they're on: every fixed size, gap, font and stylesheet length the screens
set (laid out for 1920x1200, see DESIGN_AVAILABLE) is scaled to the screen's available area (its effective
size after Windows display scaling, less the taskbar), keeping the design's proportions.

Whatever the fonts and screen, three rules hold (see the constants below): every screen fits the screen
(the scale steps down if one doesn't), text always fits the widget showing it (that widget's font shrinks
if it doesn't), and gaps/margins sit on one grid so equal gaps stay equal at every scale.

The workout's START/PAUSE/END buttons show play/pause/stop symbols.

layout_window() applies it to MainWindow and whichever screens it has built; MainWindow calls it
as each screen is built, Load/Clear Theme call it, and so does moving to another monitor.
"""

import os

from PyQt6.QtCore import QEvent, QObject, QSize, Qt, QTimer
from PyQt6.QtGui import QColor, QFontInfo, QIcon, QPainter, QPixmap
from PyQt6.QtSvg import QSvgRenderer
from PyQt6.QtWidgets import QGridLayout, QLabel, QLayout, QPushButton, QStyle, QStyleOptionButton, QWidget

import AppTheme
import ClassAntLogger


TASKBAR_HEIGHT = 48 #Windows 11; maximized windows stop above it
#the available area the screens were laid out for: 1920x1200 less the taskbar
DESIGN_AVAILABLE = (1920, 1152)
MIN_SCALE, MAX_SCALE = 0.3, 2.5
QWIDGETSIZE_MAX = 16777215
#the workout screens' buttons that show symbols in place of their text
SYMBOL_BUTTONS = ("pauseButton", "startEndButton")

#Rules every layout keeps, whatever the fonts, font sizes and screen size (see "Layout rules" below):
# 1. each screen fits the screen: one that needs more room than there is steps the layout scale down until it fits
# 2. text fits the widget showing it: a label/button too small for its text (a wide font, a big font size, a fixed
#    size scaled down) gets a smaller font, just that widget, down to TEXT_FIT_MIN of its size
# 3. gaps and margins sit on one grid: GAP_GRID design px steps (Qt's unset ones included), all scaled alike, so equal
#    gaps stay equal at every scale
TEXT_FIT_MIN = 0.6
TEXT_FIT_SLACK = 1 #px of rounding a widget's text may overrun its room by
GAP_GRID = 2
DEFAULT_GAP = 6 #Qt's own spacing, for layouts that don't set one (it didn't scale before)
SCREEN_FIT_TRIES = 4


###----------------------------------------------------------------------------------------------
### The layout scale for a screen
###----------------------------------------------------------------------------------------------
def scale_for(available):
    """The scale that fits the design to the available area (before rule 1 checks every screen fits)"""
    scale = min(available[0]/DESIGN_AVAILABLE[0], available[1]/DESIGN_AVAILABLE[1])
    return round(max(MIN_SCALE, min(scale, MAX_SCALE)), 3)

###----------------------------------------------------------------------------------------------
### Button symbols: play / pause / stop in place of START / PAUSE / END
###----------------------------------------------------------------------------------------------
#Feather Icon set (fe:play, fe:pause, fe:stop on Iconify), MIT license
SYMBOL_FILES = {"START": "Play.svg", "PAUSE": "Pause.svg", "END": "Stop.svg"}
SYMBOL_SIZE = 0.6 #of the button's smaller side


class _SymbolSizer(QObject):
    """Redraws a button's symbol at its new size whenever the button is resized (or first shown: the
    layout sizes it just before it counts as visible)"""
    def eventFilter(self, obj, event):
        if event.type() in (QEvent.Type.Resize, QEvent.Type.Show) and getattr(obj, "_layoutSymbol", False):
            show_symbol(obj)
        return False


def symbol_icon(fileName, size, dpr=1.0):
    """The SVG at size px, in the theme's button text color (black unless the theme changes it)"""
    pixmap = QPixmap(max(1, round(size*dpr)), max(1, round(size*dpr)))
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    QSvgRenderer(os.path.join(os.path.dirname(os.path.abspath(__file__)), fileName)).render(painter)
    painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceIn)
    painter.fillRect(pixmap.rect(), QColor(AppTheme.THEME.colors.get("#000000", "#000000")))
    painter.end()
    pixmap.setDevicePixelRatio(dpr)
    return QIcon(pixmap) #its disabled look (greyed) comes free


def show_symbol(button):
    """Shows the symbol for the button's current text (set via setText, see AppTheme.install);
    text without a symbol shows as text"""
    text = getattr(button, "_themeOrigText", button.text())
    fileName = SYMBOL_FILES.get(text.strip().upper())
    if fileName is None:
        button.setIcon(QIcon())
        AppTheme.original_set_text(button, text)
        return
    AppTheme.original_set_text(button, "")
    button.setToolTip(text.title())
    #a button that isn't on screen still has Qt's default size (640x480), and an icon drawn to that would
    #set its minimum size; it gets redrawn at the real size when it's shown (the resize from the layout)
    size = max(16, round(min(button.width(), button.height())*SYMBOL_SIZE)) if button.isVisible() else 16
    button.setIconSize(QSize(size, size))
    button.setIcon(symbol_icon(fileName, size, button.devicePixelRatioF()))


def set_symbol(button):
    """Shows the button's symbol in place of its text, from now on (each new text gets its symbol)"""
    if not hasattr(button, "_themeOrigText"):
        button._themeOrigText = button.text()
    button._layoutSymbol = True
    if not hasattr(button, "_layoutSymbolSizer"):
        button._layoutSymbolSizer = _SymbolSizer(button)
        button.installEventFilter(button._layoutSymbolSizer)
    show_symbol(button)



###----------------------------------------------------------------------------------------------
### Applying the scale
###----------------------------------------------------------------------------------------------
def _apply_sizes(root):
    """Scales every fixed/min/max size the screens set, and their layouts' gaps and margins"""
    for widget in [root] + root.findChildren(QWidget):
        if not hasattr(widget, "_layoutOrigSize"):
            widget._layoutOrigSize = (widget.minimumWidth(), widget.minimumHeight(),
                                      widget.maximumWidth(), widget.maximumHeight())
        minW, minH, maxW, maxH = widget._layoutOrigSize
        if (minW, minH, maxW, maxH) == (0, 0, QWIDGETSIZE_MAX, QWIDGETSIZE_MAX):
            continue
        scale = AppTheme.widget_scale(widget)
        def scaled(value):
            return value if value in (0, QWIDGETSIZE_MAX) else max(1, round(value*scale))
        if getattr(widget, "_layoutSkipHeight", False):
            widget.setMinimumWidth(scaled(minW))
            widget.setMaximumWidth(scaled(maxW))
            continue
        widget.setMinimumSize(scaled(minW), scaled(minH))
        widget.setMaximumSize(scaled(maxW), scaled(maxH))
    for layout in ([root.layout()] if root.layout() else []) + root.findChildren(QLayout):
        if not hasattr(layout, "_layoutOrigSpacing"):
            margins = layout.contentsMargins()
            spacing = ((layout.horizontalSpacing(), layout.verticalSpacing()) if isinstance(layout, QGridLayout)
                       else (layout.spacing(),))
            layout._layoutOrigSpacing = (spacing, (margins.left(), margins.top(), margins.right(), margins.bottom()))
        spacing, margins = layout._layoutOrigSpacing
        layout.setContentsMargins(*(gap(m) for m in margins))
        if isinstance(layout, QGridLayout):
            layout.setHorizontalSpacing(gap(spacing[0]))
            layout.setVerticalSpacing(gap(spacing[1]))
        else:
            layout.setSpacing(gap(spacing[0]))


def gap(design):
    """A gap or margin of this many design px at the layout scale, on the GAP_GRID (rule 3): equal design gaps
    come out equal, and keep their proportions, at every scale. Hairlines (under a grid step) just scale;
    unset (-1) is Qt's default"""
    if design < 0:
        design = DEFAULT_GAP
    if design == 0:
        return 0
    if design >= GAP_GRID:
        design = int(design/GAP_GRID + 0.5)*GAP_GRID
    return max(1, round(design*AppTheme.ui_scale))


def _settle_layouts(root):
    """Runs the layout updates size changes have queued, so geometries are current right away"""
    from PyQt6.QtCore import QCoreApplication
    for _ in range(4): #nested layouts pass the change up one level at a time
        QCoreApplication.sendPostedEvents(None, QEvent.Type.LayoutRequest)
    if root.layout() is not None:
        root.layout().activate()


###----------------------------------------------------------------------------------------------
### Layout rules 1 and 2: screens fit the screen, text fits its widget
###----------------------------------------------------------------------------------------------
def required_size(screen):
    """Smallest area the screen fits in without anything squeezed below its minimum, counting what's
    stacked with it in its window (the title bar)"""
    #a height pinned to another widget's (the setup screen's device column follows its settings column, in the
    #same row) still has the height from before the scale changed: measured without it
    pinned = [(w, w.minimumHeight(), w.maximumHeight()) for w in screen.findChildren(QWidget)
              if getattr(w, "_layoutSkipHeight", False)]
    for widget, _, _ in pinned:
        widget.setMinimumHeight(0)
        widget.setMaximumHeight(QWIDGETSIZE_MAX)
    if pinned or not screen.isVisible():
        #Qt doesn't refresh a hidden screen's cached layout sizes when its contents change
        for layout in [screen.layout()] + screen.findChildren(QLayout):
            if layout is not None:
                layout.invalidate()
    need = screen.minimumSizeHint().expandedTo(screen.minimumSize())
    for widget, minH, maxH in pinned:
        widget.setMinimumHeight(minH)
        widget.setMaximumHeight(maxH)
    width, height = need.width(), need.height()
    parent = screen.parentWidget()
    layout = parent.layout() if parent is not None else None
    if layout is not None and layout.indexOf(screen) >= 0:
        margins = layout.contentsMargins()
        width += margins.left() + margins.right()
        height += margins.top() + margins.bottom()
        for i in range(layout.count()):
            other = layout.itemAt(i).widget()
            #the other screens take turns with this one; anything else (the title bar) is on screen with it
            if other is None or other is screen or other.isHidden() or getattr(other, "_layoutScreen", False):
                continue
            otherNeed = other.minimumSizeHint().expandedTo(other.minimumSize())
            width = max(width, otherNeed.width() + margins.left() + margins.right())
            height += max(other.minimumHeight(), otherNeed.height()) + max(layout.spacing(), 0)
    return width, height


def screen_fit(screens, available):
    """How much of what the most demanding screen needs the available area has (1 or more: they all fit),
    and that screen's name and size needed"""
    fit, worst = 1.0, None
    for name, screen in screens.items():
        width, height = required_size(screen)
        screenFit = min(available[0]/max(width, 1), available[1]/max(height, 1))
        if screenFit < fit:
            fit, worst = screenFit, (name, width, height)
    return fit, worst


def _exempt(widget):
    while widget is not None:
        if getattr(widget, "_layoutExempt", False):
            return True
        widget = widget.parentWidget()
    return False


def _shows_text(widget):
    """Labels and push buttons showing text the text rule looks after"""
    if getattr(widget, "_layoutOwnFit", False): #(fits its text itself, e.g. the HUD's Ride At)
        return False
    if isinstance(widget, QLabel):
        pixmap = widget.pixmap()
        return bool(widget.text().strip()) and (pixmap is None or pixmap.isNull()) and widget.movie() is None
    if isinstance(widget, QPushButton):
        return bool(widget.text().strip()) and widget.icon().isNull()
    return False


def text_room(widget):
    """Room the widget has for its text over what the text needs (under 1: it doesn't fit)"""
    slack = TEXT_FIT_SLACK
    if isinstance(widget, QLabel):
        if widget.wordWrap():
            need = widget.heightForWidth(widget.width())
            #(a smaller font wraps to fewer, shorter lines: the height needed goes about with its square)
            return ((widget.height() + slack)/need)**0.5 if need > 0 else 1.0
        need = widget.minimumSizeHint()
        return min((widget.width() + slack)/max(need.width(), 1), (widget.height() + slack)/max(need.height(), 1))
    option = QStyleOptionButton()
    widget.initStyleOption(option)
    #inside the button's border and padding
    room = widget.style().subElementRect(QStyle.SubElement.SE_PushButtonContents, option, widget)
    metrics = widget.fontMetrics()
    lines = widget.text().replace("&&", "\0").replace("&", "").replace("\0", "&").split("\n")
    textWidth = max(metrics.horizontalAdvance(line) for line in lines)
    textHeight = metrics.height() + metrics.lineSpacing()*(len(lines) - 1)
    return min((room.width() + slack)/max(textWidth, 1), (room.height() + slack)/max(textHeight, 1))


def _set_fit_px(widget, px):
    widget._layoutFitPx = px
    if not hasattr(widget, "_themeOrigSheet"):
        widget._themeOrigSheet = ""
    AppTheme.original_set_stylesheet(widget, AppTheme.themed_sheet(widget))


def _shrink_to_fit(widget):
    """One step of rule 2 for a widget: a font small enough for the room it has now. True when it changed"""
    room = text_room(widget)
    if room >= 1:
        return False
    current = QFontInfo(widget.font()).pixelSize()
    if getattr(widget, "_layoutFitPx", None) is None:
        widget._layoutFitBase = current
    smallest = max(6, int(widget._layoutFitBase*TEXT_FIT_MIN))
    px = max(smallest, int(current*room))
    if px >= current:
        return False
    _set_fit_px(widget, px)
    return True


def fit_text(root):
    """Rule 2 for every visible label and button in root, from their own font sizes (hidden ones get it when
    shown, see watch_screen). Not while root is squeezed below its minimum size: it's still being resized
    (e.g. on its way to maximized), and text fitted to that would stay too small"""
    _settle_layouts(root.window())
    need = root.minimumSizeHint()
    if root.width() < need.width() or root.height() < need.height():
        return
    widgets = [w for w in root.findChildren(QWidget) if w.isVisible() and _shows_text(w) and not _exempt(w)]
    fitted = [w for w in widgets if getattr(w, "_layoutFitPx", None)]
    for widget in fitted:
        _set_fit_px(widget, None)
    for widget in widgets:
        widget._layoutFitWatch = True #text set later gets checked too, see text_changed
    for _ in range(4): #shrinking one can move the others
        _settle_layouts(root.window())
        if not [w for w in widgets if _shrink_to_fit(w)]:
            break


def text_changed(widget):
    """New text on a laid out label/button: rule 2 again once the layout has made room for it (a font made
    smaller for longer text grows back for shorter)"""
    if getattr(widget, "_layoutFitPending", False):
        return
    widget._layoutFitPending = True
    def refit():
        try:
            widget._layoutFitPending = False
            if not widget.isVisible() or not _shows_text(widget):
                return
            _settle_layouts(widget.window())
            #(room for the text at its own size)
            if (getattr(widget, "_layoutFitPx", None)
                    and text_room(widget) >= widget._layoutFitBase/QFontInfo(widget.font()).pixelSize()):
                _set_fit_px(widget, None)
                _settle_layouts(widget.window())
            for _ in range(3):
                if not _shrink_to_fit(widget):
                    break
                _settle_layouts(widget.window())
        except RuntimeError: #deleted meanwhile
            pass
    QTimer.singleShot(0, refit)


def _clear_text_fit(root):
    """Back to each widget's own font size, to be measured afresh (AppTheme.apply re-sets the sheets)"""
    for widget in [root] + root.findChildren(QWidget):
        if getattr(widget, "_layoutFitPx", None):
            widget._layoutFitPx = None


TEXT_FIT_DELAY = 100 #ms after the last show/resize


class _TextFitter(QObject):
    """Rule 2 for a window, screen or dialog whenever it's shown or resized (once that settles): hidden widgets
    have no real size to fit text to, and a new size can need different fonts"""
    def eventFilter(self, obj, event):
        if event.type() in (QEvent.Type.Show, QEvent.Type.Resize):
            timer = getattr(obj, "_layoutFitTimer", None)
            if timer is None:
                timer = obj._layoutFitTimer = QTimer(obj)
                timer.setSingleShot(True)
                timer.setInterval(TEXT_FIT_DELAY)
                timer.timeout.connect(lambda: obj.isVisible() and fit_text(obj))
            timer.start()
        return False


_textFitter = None


def watch_screen(widget):
    global _textFitter
    if _textFitter is None:
        _textFitter = _TextFitter()
    if not getattr(widget, "_layoutTextFitter", False):
        widget._layoutTextFitter = True
        widget.installEventFilter(_textFitter)


def fit_dialog(dialog):
    """The layout scale and rules for a dialog (the pickers): its fixed sizes and gaps scaled, its size (as set
    for the design size) scaled too and kept on the screen, and its text fitted once it's shown"""
    _apply_sizes(dialog)
    screen = (dialog.parentWidget() or dialog).screen()
    if screen is not None:
        available = screen.availableGeometry()
        width = min(round(dialog.width()*AppTheme.ui_scale), available.width())
        height = min(round(dialog.height()*AppTheme.ui_scale), available.height())
        dialog.resize(width, height)
    watch_screen(dialog)


def apply_layout(root, screens, scale, available=None):
    """root: the main window (with its title_bar); screens: {view name: screen widget}; available: the area
    the screens must fit in (rule 1; None: not checked)"""
    for view, screen in screens.items():
        screen._layoutScreen = True
        watch_screen(screen)
        for name in SYMBOL_BUTTONS:
            if getattr(screen, name, None) is not None:
                set_symbol(getattr(screen, name))
    if "Init" in screens:
        #its own event filter keeps this column as tall as the settings column
        screens["Init"].initializationWidget._layoutSkipHeight = True
    _clear_text_fit(root)
    wanted, tooBig = scale, []
    for _ in range(SCREEN_FIT_TRIES):
        AppTheme.ui_scale = scale
        _apply_sizes(root)
        AppTheme.apply()
        fit, worst = screen_fit(screens, available) if available else (1.0, None)
        if fit >= 1 or scale <= MIN_SCALE:
            break
        tooBig.append(f"{worst[0]} needs {worst[1]}x{worst[2]} at {scale:g}")
        #(a little under: not everything shrinks in proportion, e.g. borders and hairlines)
        scale = max(MIN_SCALE, int(scale*fit*0.99*1000)/1000)
    if scale != wanted:
        ClassAntLogger.log_event("DISPLAY", f"Layout scale {wanted:g} -> {scale:g} so every screen fits "
                                 f"{available[0]}x{available[1]} ({'; '.join(tooBig)})")
    #(the window's own resizes, e.g. to maximized, fit it again)
    watch_screen(root)
    fit_text(root)
    for screen in screens.values():
        #the workout message fits itself between the gauges, which may have moved
        if hasattr(screen, "fit_workout_message"):
            QTimer.singleShot(0, screen.fit_workout_message)


###----------------------------------------------------------------------------------------------
### The app's main window
###----------------------------------------------------------------------------------------------
_window = None


def screens_of(window):
    from ClassWorkoutSetupWindow import WorkoutSetupWindow
    from ClassHUDWindow import HUDWindow
    from ClassSynopsisWindow import SynopsisWindow
    screens = {}
    for widget in window.findChildren(QWidget):
        if isinstance(widget, WorkoutSetupWindow):
            screens["Init"] = widget
        elif isinstance(widget, HUDWindow):
            screens["Workout" if hasattr(widget, "upNextLabelHeader") else "Free Ride"] = widget
        elif isinstance(widget, SynopsisWindow):
            screens["Synopsis"] = widget
    return screens


_loggedResolution = None


def layout_window(window=None):
    """Scales the window and the screens it has built to its screen"""
    global _window, _loggedResolution
    if window is not None:
        _window = window
    window = _window
    if window is None:
        AppTheme.apply()
        return
    #moving to another monitor scales to that monitor (the native window only exists once shown)
    handle = window.windowHandle()
    if handle is not None and not getattr(window, "_layoutScreenHooked", False):
        window._layoutScreenHooked = True
        handle.screenChanged.connect(lambda screen: layout_window())
    screen = window.screen()
    full, available = screen.geometry(), screen.availableGeometry()
    #log the resolution once per monitor/size (this runs again as each screen is built)
    dpr = screen.devicePixelRatio()
    resolution = (screen.name(), full.width(), full.height(), available.width(), available.height(), dpr)
    if resolution != _loggedResolution:
        _loggedResolution = resolution
        ClassAntLogger.log_event("DISPLAY", f"Screen {screen.name()}: {full.width()}x{full.height()} effective "
                                 f"({round(full.width()*dpr)}x{round(full.height()*dpr)} physical, {dpr:g}x scaling), "
                                 f"available {available.width()}x{available.height()}")
    availableSize = (available.width(), available.height())
    apply_layout(window, screens_of(window), scale_for(availableSize), availableSize)

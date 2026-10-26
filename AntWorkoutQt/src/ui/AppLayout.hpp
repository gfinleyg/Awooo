#pragma once

// Fits the screens to the screen they're on (AppLayout.py): every fixed size, gap, font and stylesheet length the
// screens set (laid out for 1920x1200, see kDesignAvailable) is scaled to the screen's available area (its effective
// size after Windows display scaling, less the taskbar), keeping the design's proportions.
//
// Whatever the fonts and screen, three rules hold: every screen fits the screen (the scale steps down if one
// doesn't), text always fits the widget showing it (that widget's font shrinks if it doesn't), and gaps and margins
// sit on one grid so equal gaps stay equal at every scale. The workout's START/PAUSE/END buttons show play/pause/stop
// symbols.
//
// layoutWindow() applies it to the main window and the screens it has built (each registered with markScreen); the
// main window calls it as each screen is built, loading a theme calls it, and so does moving to another monitor.
// Widget text goes through setText() so a label or button laid out here gets its text fitted again.

#include <QMap>
#include <QSize>
#include <QString>

class QAbstractButton;
class QIcon;
class QLabel;
class QWidget;

namespace awo::layout {

constexpr int kTaskbarHeight = 48;
// the available area the screens were laid out for: 1920x1200 less the taskbar
constexpr QSize kDesignAvailable{1920, 1152};
constexpr double kMinScale = 0.3;
constexpr double kMaxScale = 2.5;
constexpr double kTextFitMin = 0.6;
constexpr int kTextFitSlack = 1;
constexpr int kGapGrid = 2;
constexpr int kDefaultGap = 6;
constexpr int kScreenFitTries = 4;
constexpr int kTextFitDelayMs = 100;
constexpr double kSymbolSize = 0.6;

// The scale that fits the design to the available area (before rule 1 checks every screen fits)
double scaleFor(const QSize& available);
// A gap or margin of this many design px at the layout scale (or the scale given), on the grid
int gap(int design, double scale = -1);

// The SVG at size px, in the theme's button text color
QIcon symbolIcon(const QString& fileName, int size, double dpr = 1.0);
// Shows the symbol for the button's text (play/pause/stop for START/PAUSE/END); other text shows as text
void showSymbol(QAbstractButton* button);
// The button shows its symbol from now on (each new text gets its symbol)
void setSymbol(QAbstractButton* button);

// Text on a label or button: the replacement for setText, so laid out text is fitted again and a symbol button
// shows its symbol
void setText(QLabel* label, const QString& text);
void setText(QAbstractButton* button, const QString& text);
// The button's text as set (a symbol button shows none)
QString text(const QAbstractButton* button);

// A screen of the main window, by its view name ("Init", "Workout", "Free Ride", "Synopsis")
void markScreen(QWidget* screen, const QString& name);
// A widget whose height follows another's (the setup screen's device column): measured without it
void setSkipHeight(QWidget* widget, bool skip = true);
// A widget that fits its own text (the workout screen's Ride At)
void setOwnFit(QWidget* widget, bool own = true);

// Smallest area the screen fits in, counting what's stacked with it in its window (the title bar)
QSize requiredSize(QWidget* screen);
// Rule 2 for every visible label and button in root
void fitText(QWidget* root);
// New text on a laid out label or button: rule 2 again once the layout has made room for it
void textChanged(QWidget* widget);
// Rule 2 whenever the widget (a window, screen or dialog) is shown or resized
void watchScreen(QWidget* widget);
// The layout scale and rules for a dialog or popup window: sizes and gaps scaled, its size kept on the screen, its
// text fitted once shown, and drawn smaller if it needs more room than the screen has
void fitDialog(QWidget* dialog);
void applyLayout(QWidget* root, const QMap<QString, QWidget*>& screens, double scale, const QSize& available = {});
// Scales the window (the main window, remembered) and the screens it has built to its screen
void layoutWindow(QWidget* window = nullptr);
QWidget* mainWindow();

}  // namespace awo::layout

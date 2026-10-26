#pragma once

// Font and color overrides for the whole app (AppTheme.py), made in LayoutViewer and saved as a theme .json:
//
//   {
//     "fonts": {"main": {"family": "Anybody Thin", "scale": 100, "weight": "Original"}, ...},   per font role
//     "colors": {"#9fc5d1": "#ffcc66", ...},        original color -> replacement
//     "ftpColors": {"zones": {"2": "#e0c200"}, ...}  the zone colors changed from the defaults (see ZoneColors)
//   }
//
// The colors and the zone gradients can also be saved and loaded on their own (kParts): a colors file holds just
// "colors", a gradients file just "ftpColors". Loading one replaces just that part of the theme, and it stays on top
// until a whole theme is loaded or the theme is cleared. With no theme picked the app uses kDefaultThemeFile, else the
// built-in theme.
//
// The Python app hooked Qt's own setters so every stylesheet passed through the theme; here every stylesheet goes
// through setSheet() instead (and widget text through AppLayout's setText), which keeps the original in the widget,
// so apply() can re-theme everything built when a theme is loaded, changed or cleared. Widgets that paint
// themselves read the theme as they paint, and repaint on notifier().changed().

#include <QColor>
#include <QFont>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPen>
#include <QSet>
#include <QString>
#include <QStringList>

#include <stdexcept>

class QWidget;

namespace awo::theme {

// the font roles, keyed by the family the app's stylesheets name (they mark which of the theme's fonts to use)
extern const QString kRoleMain;    // "Orbitron": text
extern const QString kRoleButton;  // "Segoe UI Black": buttons and stats
extern const QString kRoleGauge;   // the gauges' numbers
extern const QHash<QString, QString> kRoleFamilies;  // lowercase family -> role
extern const QMap<QString, QString> kRoleTitles;
extern const QStringList kWeights;

// colors that look like black/white with no theme but are their own entries, so a theme can set them apart from the
// text: a button's text, the pressed button's gradient end, and light text on the window gradient
extern const QString kButtonText;
extern const QString kPressed;
extern const QString kLightText;
// text no theme recolors: on the zone and grade colors, which the gradients files set (see textOn)
extern const QString kFixedDarkText;
extern const QString kFixedLightText;
// every color a colors file maps
extern const QStringList kThemeKeys;
// the synopsis' max value labels paint this background themselves
extern const QString kPlotLabelBg;
extern const QString kDefaultThemeFile;
// the parts of a theme saved and loaded on their own: file key -> what to call it
extern const QMap<QString, QString> kParts;

QString themesDir();
QString partDir(const QString& part);

// a theme file that couldn't be read or isn't one
class ThemeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FontSettings {
    QString family;
    double scale = 100;
    QString weight = QStringLiteral("Original");

    bool operator==(const FontSettings&) const = default;
};

class Theme {
public:
    Theme() { reset(); }

    QMap<QString, FontSettings> fonts;  // by role
    QHash<QString, QString> colors;     // original "#rrggbb" -> replacement, lowercase
    QJsonObject ftp;                    // the zone colors changed from the defaults

    // the app's own look
    void reset();
    // the whole theme, or just one of kParts
    QJsonObject toJson(const QString& part = {}) const;
    // replaces one of kParts (as read from a file)
    void setPart(const QString& part, const QJsonObject& value);
    void fromJson(const QJsonObject& data);

    // "#rrggbb" or Qt's "#aarrggbb" (the alpha kept)
    QString mapHex(const QString& text) const;
    QColor mapColor(const QColor& color) const;
    QFont mapFont(const QFont& font, double scale = 1.0) const;
    // text with this theme's fonts and colors; scale also multiplies every font size and pixel length
    QString css(const QString& text, double scale = 1.0) const;

    bool operator==(const Theme&) const = default;
};

// every color in a stylesheet, as "#rrggbb"
QSet<QString> colorsInCss(const QString& text);
// WCAG relative luminance (0 black to 1 white) and contrast ratio (1 to 21)
double luminance(const QColor& color);
double contrast(const QColor& a, const QColor& b);
// the text color for a background the colors theme doesn't set (a zone or grade color): the theme's text color if it
// does set it, else whichever of dark or light reads on it
QString textOn(const QString& background);
// a copy of the pen in the theme's color for its color (its alpha kept)
QPen themedPen(const QPen& pen);

Theme& current();

// the layout scale (see AppLayout): 1.0 is the size the screens were designed at
double uiScale();
void setUiScale(double scale);

// ---- Widgets ----

// The stylesheet a widget is to have, through the theme at its scale (the replacement for setStyleSheet)
void setSheet(QWidget* widget, const QString& sheet);
QString originalSheet(const QWidget* widget);
bool hasSheet(const QWidget* widget);
// A stylesheet a widget carries in front of its own (LayoutViewer puts the app wide one on its display window)
void setPrefixSheet(QWidget* widget, const QString& sheet);
// What the widget's stylesheet is through the theme: its own (and any prefix) at its scale, then the smaller font
// size AppLayout gave it if its text didn't fit
QString themedSheet(QWidget* widget);
// Re-sets one widget's stylesheet through the theme
void restyle(QWidget* widget);
// How much smaller than the layout scale a popup window (and what's in it) is drawn so it fits the screen
double layoutShrink(const QWidget* widget);
// The layout scale for a widget: 1 for widgets marked exempt (LayoutViewer's controls), less in a shrunk popup
double widgetScale(const QWidget* widget);
void setExempt(QWidget* widget, bool exempt = true);

void setAppStyleSheet(const QString& sheet);
QString appStyleSheet();
// the system default font from before the theme's: buttons keep it
void setDefaultFont(const QFont& font);
QFont defaultFont();

// changed(): the theme was applied again, for whatever paints itself
class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
Notifier& notifier();

// Re-sets everything built through the current theme
void apply();
// Registers every font in fonts/ and fonts/variable/ (once); their family names
QStringList registerFonts();

// ---- Files ----

QJsonObject readFile(const QString& path);
// "theme" for a whole theme (it has fonts), else the part it holds ("colors", "ftpColors")
QString fileKind(const QJsonObject& data);
// Loads a theme .json and applies it: a whole theme replaces the theme (and drops the part files on top of the old
// one), a colors or gradients file just that part. Its kind; throws ThemeError for a bad file
QString loadFile(const QString& path);
// A colors file and a gradients file picked at random from themes/, each loaded on top; never the one in use or the
// one picked last time (kept as "randomThemeLast" in the settings), unless there's nothing else
void loadRandomParts();
QMap<QString, QString> partFiles();
QString themeFile();
// the theme's own colors or gradients, without the part file on top
QJsonObject basePart(const QString& part);
// back to the theme's own colors or gradients, applied
void dropPart(const QString& part);

struct Snapshot {
    QJsonObject data;
    QMap<QString, QString> parts;
    QString file;
};
Snapshot snapshot();
void restore(const Snapshot& saved);
void saveFile(const QString& path, const QString& part = {});
// back to the app's own fonts and colors
void reset();
// applies the theme, rescaling the app's window to its screen when there is one
void refresh();
// the file used when no theme is picked; empty: the built-in theme
QString defaultSource();
// no theme picked: the default theme, applied; without the part files on top unless keepParts
void clear(bool keepParts = false);

// (tests: forget the files picked, without applying)
void forgetFiles();

}  // namespace awo::theme

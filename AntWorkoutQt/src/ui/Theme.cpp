#include "Theme.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <random>

#include "AppLayout.hpp"
#include "Logger.hpp"
#include "PyCompat.hpp"
#include "SavedSettings.hpp"
#include "ZoneColors.hpp"

namespace awo::theme {

const QString kRoleMain = QStringLiteral("main");
const QString kRoleButton = QStringLiteral("button");
const QString kRoleGauge = QStringLiteral("gauge");
const QHash<QString, QString> kRoleFamilies = {{QStringLiteral("orbitron"), kRoleMain},
                                               {QStringLiteral("segoe ui black"), kRoleButton}};
const QMap<QString, QString> kRoleTitles = {{kRoleMain, QStringLiteral("Text")},
                                            {kRoleButton, QStringLiteral("Buttons + stats")},
                                            {kRoleGauge, QStringLiteral("Gauge numbers")}};
const QStringList kWeights = {QStringLiteral("Original"), QStringLiteral("100"), QStringLiteral("200"),
                              QStringLiteral("300"),      QStringLiteral("400"), QStringLiteral("500"),
                              QStringLiteral("600"),      QStringLiteral("700"), QStringLiteral("800"),
                              QStringLiteral("900")};
const QString kButtonText = QStringLiteral("#000001");
const QString kPressed = QStringLiteral("#1f15e2");
const QString kLightText = QStringLiteral("#fffffe");
const QString kFixedDarkText = QStringLiteral("#010101");
const QString kFixedLightText = QStringLiteral("#fefefe");
const QStringList kThemeKeys = {QStringLiteral("#28275e"), QStringLiteral("#9fc5d1"), QStringLiteral("#bdd6de"),
                                QStringLiteral("#1cb5e0"), QStringLiteral("#1f15e1"), QStringLiteral("#dadbde"),
                                QStringLiteral("#029cde"), QStringLiteral("#808080"), QStringLiteral("#000000"),
                                QStringLiteral("#28285e"), kButtonText, kPressed, kLightText};
const QString kPlotLabelBg = QStringLiteral("#bdd6de");
const QString kDefaultThemeFile = QStringLiteral("defaultTheme.json");
const QMap<QString, QString> kParts = {{QStringLiteral("colors"), QStringLiteral("colors")},
                                       {QStringLiteral("ftpColors"), QStringLiteral("power/cadence gradients")}};

namespace {

// the built-in look: the screens' stylesheets still name Orbitron and Segoe UI Black, which mark the font roles
// these fonts replace
const QString kBuiltInFamily = QStringLiteral("Roboto Flex");

const QRegularExpression& hexRe() {
    static const QRegularExpression re(QStringLiteral(R"(#([0-9a-fA-F]{8}|[0-9a-fA-F]{6})\b)"));
    return re;
}
const QRegularExpression& namedRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(((?:background-)?color|border-color|background)(\s*:\s*)(black|white|grey|gray|red|green|blue)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}
// font shorthand (font: [modifiers] <size><unit> '<family>'), or any other pixel length
const QRegularExpression& fontOrPxRe() {
    static const QRegularExpression re(
        QStringLiteral(R"((?P<prefix>font\s*:\s*)(?P<mods>[^;}"']*?)(?P<size>\d+(?:\.\d+)?)(?P<unit>pt|px)\s*)"
                       R"((?P<q>['"])(?P<family>[^'"]+)(?P=q)|(?<![\w.#])(?P<length>-?\d+(?:\.\d+)?)px\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression& weightTokenRe() {
    static const QRegularExpression re(QStringLiteral(R"(\b(\d00|bold|normal)\b)"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

// re.sub with a function
QString substitute(const QString& text, const QRegularExpression& re,
                   const std::function<QString(const QRegularExpressionMatch&)>& replace) {
    QString out;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += text.mid(last, match.capturedStart() - last);
        out += replace(match);
        last = match.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

double scaleValue = 1.0;
QString appSheet;
bool appSheetSet = false;
QFont savedDefaultFont;
bool defaultFontSet = false;
QString appliedAppFont;
QMap<QString, QString> partFilesLoaded;
QString themeFileLoaded;

const char* const kSheetProperty = "awoSheet";
const char* const kPrefixProperty = "awoPrefixSheet";
const char* const kExemptProperty = "awoExempt";
const char* const kShrinkProperty = "awoShrink";

void ensureObjectName(QWidget* widget) {
    if (widget->objectName().isEmpty()) {
        widget->setObjectName(QStringLiteral("themed%1").arg(reinterpret_cast<quintptr>(widget)));
    }
}

QString sheetSource(QWidget* widget) {
    const QString prefix = widget->property(kPrefixProperty).toString();
    QString own = originalSheet(widget);
    const bool fitted = widget->property("awoFitPx").toInt() > 0;
    // bare declarations (no selector) can't follow rules: aimed at just this widget
    if ((!prefix.isEmpty() || fitted) && !own.trimmed().isEmpty() && !own.contains(u'{')) {
        ensureObjectName(widget);
        own = QStringLiteral("#%1{%2}").arg(widget->objectName(), own);
    }
    return prefix + u'\n' + own;
}

// the colors and gradients files back on top of a theme just loaded underneath them
void reloadPartFiles() {
    for (const QString& part : partFilesLoaded.keys()) {
        const QString path = partFilesLoaded.value(part);
        try {
            const QJsonObject data = readFile(path);
            if (!data.value(part).isObject()) {
                throw ThemeError("no " + part.toStdString());
            }
            current().setPart(part, data.value(part).toObject());
        } catch (const ThemeError& e) {
            log::event(QStringLiteral("ERROR"),
                       QStringLiteral("Failed to reload %1 %2: %3").arg(kParts.value(part), path, QString::fromUtf8(e.what())));
            partFilesLoaded.remove(part);
        }
    }
}

// the default theme into the theme in use (not applied)
void loadDefault() {
    const QString path = defaultSource();
    if (!path.isEmpty()) {
        try {
            const QJsonObject data = readFile(path);
            registerFonts();
            current().fromJson(data);
            reloadPartFiles();
            return;
        } catch (const ThemeError& e) {
            log::event(QStringLiteral("ERROR"),
                       QStringLiteral("Failed to load default theme %1: %2").arg(path, QString::fromUtf8(e.what())));
        }
    }
    current().reset();
    reloadPartFiles();
}

QString normalized(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

}  // namespace

QString themesDir() {
    return QDir::current().absoluteFilePath(QStringLiteral("themes"));
}

QString partDir(const QString& part) {
    return QDir(themesDir()).filePath(part == u"colors" ? QStringLiteral("colors") : QStringLiteral("gradients"));
}

// ---- Theme ----

void Theme::reset() {
    fonts = {{kRoleMain, {kBuiltInFamily}}, {kRoleButton, {kBuiltInFamily}}, {kRoleGauge, {kBuiltInFamily}}};
    colors.clear();
    ftp = QJsonObject();
}

QJsonObject Theme::toJson(const QString& part) const {
    QJsonObject colorObject;
    for (auto it = colors.cbegin(); it != colors.cend(); ++it) {
        colorObject.insert(it.key(), it.value());
    }
    if (part == u"colors") {
        return {{QStringLiteral("colors"), colorObject}};
    }
    if (part == u"ftpColors") {
        return {{QStringLiteral("ftpColors"), ftp}};
    }
    QJsonObject fontObject;
    for (auto it = fonts.cbegin(); it != fonts.cend(); ++it) {
        fontObject.insert(it.key(), QJsonObject{{QStringLiteral("family"), it->family},
                                                {QStringLiteral("scale"), it->scale},
                                                {QStringLiteral("weight"), it->weight}});
    }
    return {{QStringLiteral("fonts"), fontObject}, {QStringLiteral("colors"), colorObject},
            {QStringLiteral("ftpColors"), ftp}};
}

void Theme::setPart(const QString& part, const QJsonObject& value) {
    if (part == u"colors") {
        colors.clear();
        for (auto it = value.begin(); it != value.end(); ++it) {
            colors.insert(it.key().toLower(), it.value().toString().toLower());
        }
        return;
    }
    // (older files' entries, from before each zone had one color, are dropped: the defaults stand in)
    ftp = QJsonObject();
    for (const QString& key : {QStringLiteral("zones"), QStringLiteral("free")}) {
        if (value.contains(key)) {
            ftp.insert(key, value.value(key));
        }
    }
}

void Theme::fromJson(const QJsonObject& data) {
    reset();
    const QJsonObject fontObject = data.value(QStringLiteral("fonts")).toObject();
    for (auto it = fontObject.begin(); it != fontObject.end(); ++it) {
        if (!fonts.contains(it.key())) {
            continue;
        }
        const QJsonObject settings = it.value().toObject();
        FontSettings& font = fonts[it.key()];
        if (settings.contains(QStringLiteral("family"))) {
            font.family = settings.value(QStringLiteral("family")).toString();
        }
        if (settings.contains(QStringLiteral("scale"))) {
            font.scale = settings.value(QStringLiteral("scale")).toDouble(100);
        }
        if (settings.contains(QStringLiteral("weight"))) {
            const QJsonValue weight = settings.value(QStringLiteral("weight"));
            font.weight = weight.isString() ? weight.toString() : QString::number(weight.toInt());
        }
    }
    // (older themes' "layouts", per screen size part sizes, are ignored: the app scales itself to fit)
    for (const QString& part : kParts.keys()) {
        if (data.value(part).isObject()) {
            setPart(part, data.value(part).toObject());
        }
    }
}

QString Theme::mapHex(const QString& text) const {
    const bool hasAlpha = text.size() != 7;
    const QString alpha = hasAlpha ? text.left(3) : QString();
    const QString rgb = hasAlpha ? u'#' + text.mid(3) : text;
    const auto found = colors.constFind(rgb.toLower());
    if (found == colors.constEnd()) {
        return text;
    }
    return hasAlpha ? alpha + found->mid(1) : *found;
}

QColor Theme::mapColor(const QColor& color) const {
    const auto found = colors.constFind(color.name());
    if (found == colors.constEnd()) {
        return color;
    }
    QColor mapped(*found);
    mapped.setAlpha(color.alpha());
    return mapped;
}

QFont Theme::mapFont(const QFont& font, double scale) const {
    const auto role = kRoleFamilies.constFind(font.family().toLower());
    if (role == kRoleFamilies.constEnd()) {
        return font;
    }
    const FontSettings& settings = fonts[*role];
    QFont themed(font);
    themed.setFamily(settings.family);
    if (font.pointSizeF() > 0) {
        themed.setPointSizeF(font.pointSizeF() * settings.scale / 100 * scale);
    }
    if (settings.weight != u"Original") {
        themed.setWeight(static_cast<QFont::Weight>(settings.weight.toInt()));
    }
    return themed;
}

QString Theme::css(const QString& source, double scale) const {
    QString text = substitute(source, fontOrPxRe(), [&](const QRegularExpressionMatch& m) {
        if (m.capturedStart(QStringLiteral("length")) >= 0) {
            const double length = m.captured(QStringLiteral("length")).toDouble();
            if (length == 0) {
                return QStringLiteral("0px");
            }
            // never rounds a non-zero length down to nothing
            const double sized = std::max(1.0, py::round(std::abs(length) * scale));
            return QStringLiteral("%1px").arg(static_cast<long long>(std::copysign(sized, length)));
        }
        const QString family = m.captured(QStringLiteral("family"));
        const auto role = kRoleFamilies.constFind(family.trimmed().toLower());
        const FontSettings settings = role != kRoleFamilies.constEnd() ? fonts[*role] : FontSettings{family};
        const long long size = static_cast<long long>(
            std::max(1.0, py::round(m.captured(QStringLiteral("size")).toDouble() * settings.scale / 100 * scale)));
        QString mods = m.captured(QStringLiteral("mods"));
        if (settings.weight != u"Original") {
            if (weightTokenRe().match(mods).hasMatch()) {
                mods = substitute(mods, weightTokenRe(), [&](const QRegularExpressionMatch&) { return settings.weight; });
            } else {
                mods = settings.weight + u' ' + mods;
            }
        }
        return QStringLiteral("%1%2%3%4 '%5'")
            .arg(m.captured(QStringLiteral("prefix")), mods)
            .arg(size)
            .arg(m.captured(QStringLiteral("unit")), settings.family);
    });
    text = substitute(text, hexRe(), [&](const QRegularExpressionMatch& m) { return mapHex(m.captured(0)); });
    return substitute(text, namedRe(), [&](const QRegularExpressionMatch& m) {
        const auto found = colors.constFind(QColor(m.captured(3)).name());
        return m.captured(1) + m.captured(2) + (found != colors.constEnd() ? *found : m.captured(3));
    });
}

// ---- Colors ----

QSet<QString> colorsInCss(const QString& text) {
    QSet<QString> found;
    QRegularExpressionMatchIterator hex = hexRe().globalMatch(text);
    while (hex.hasNext()) {
        found.insert((u'#' + hex.next().captured(1).right(6)).toLower());
    }
    QRegularExpressionMatchIterator named = namedRe().globalMatch(text);
    while (named.hasNext()) {
        found.insert(QColor(named.next().captured(3)).name());
    }
    return found;
}

double luminance(const QColor& color) {
    const auto linear = [](int channel) {
        const double c = channel / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.red()) + 0.7152 * linear(color.green()) + 0.0722 * linear(color.blue());
}

double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QString textOn(const QString& background) {
    if (kThemeKeys.contains(background.toLower())) {
        return QStringLiteral("#000000");
    }
    return contrast(QColor(background), Qt::black) >= contrast(QColor(background), Qt::white) ? kFixedDarkText
                                                                                              : kFixedLightText;
}

QPen themedPen(const QPen& pen) {
    QPen themed(pen);
    themed.setColor(current().mapColor(pen.color()));
    return themed;
}

Theme& current() {
    static Theme theme;
    return theme;
}

double uiScale() {
    return scaleValue;
}

void setUiScale(double scale) {
    scaleValue = scale;
}

// ---- Widgets ----

void setSheet(QWidget* widget, const QString& sheet) {
    widget->setProperty(kSheetProperty, sheet);
    widget->setStyleSheet(themedSheet(widget));
}

QString originalSheet(const QWidget* widget) {
    return widget->property(kSheetProperty).toString();
}

bool hasSheet(const QWidget* widget) {
    return widget->property(kSheetProperty).isValid();
}

void setPrefixSheet(QWidget* widget, const QString& sheet) {
    widget->setProperty(kPrefixProperty, sheet);
    if (!hasSheet(widget)) {
        widget->setProperty(kSheetProperty, QString());
    }
    restyle(widget);
}

QString themedSheet(QWidget* widget) {
    QString sheet = current().css(sheetSource(widget), widgetScale(widget));
    const int fitPx = widget->property("awoFitPx").toInt();
    if (fitPx > 0) {
        ensureObjectName(widget);
        sheet += QStringLiteral("\n#%1{font-size: %2px;}").arg(widget->objectName()).arg(fitPx);
    }
    return sheet;
}

void restyle(QWidget* widget) {
    widget->setStyleSheet(themedSheet(widget));
}

double layoutShrink(const QWidget* widget) {
    double shrink = 1.0;
    for (; widget; widget = widget->parentWidget()) {
        const QVariant value = widget->property(kShrinkProperty);
        if (value.isValid()) {
            shrink *= value.toDouble();
        }
    }
    return shrink;
}

double widgetScale(const QWidget* widget) {
    for (const QWidget* w = widget; w; w = w->parentWidget()) {
        if (w->property(kExemptProperty).toBool()) {
            return 1.0;
        }
    }
    return scaleValue * layoutShrink(widget);
}

void setExempt(QWidget* widget, bool exempt) {
    widget->setProperty(kExemptProperty, exempt);
}

void setAppStyleSheet(const QString& sheet) {
    appSheet = sheet;
    appSheetSet = true;
    qApp->setStyleSheet(current().css(sheet, scaleValue));
}

QString appStyleSheet() {
    return appSheet;
}

void setDefaultFont(const QFont& font) {
    savedDefaultFont = font;
    defaultFontSet = true;
}

QFont defaultFont() {
    return defaultFontSet ? savedDefaultFont : QApplication::font();
}

Notifier& notifier() {
    static Notifier instance;
    return instance;
}

void apply() {
    registerFonts();
    if (appSheetSet) {
        qApp->setStyleSheet(current().css(appSheet, scaleValue));
    }
    // the app wide font; buttons keep the system default
    const FontSettings& main = current().fonts[kRoleMain];
    QFont appFont(main.family);
    if (main.scale != 100 || scaleValue != 1.0) {
        appFont.setPointSizeF(appFont.pointSizeF() * main.scale / 100 * scaleValue);
    }
    // (only when it changed: setting the app font re-polishes every widget)
    if (appFont.key() != appliedAppFont) {
        appliedAppFont = appFont.key();
        QApplication::setFont(appFont);
        if (defaultFontSet) {
            QApplication::setFont(savedDefaultFont, "QAbstractButton");
        }
    }
    for (QWidget* widget : QApplication::allWidgets()) {
        if (hasSheet(widget)) {
            restyle(widget);
        }
        if (widget->property("awoSymbol").toBool()) {
            // button symbols are drawn in the theme's button text color
            layout::showSymbol(qobject_cast<QAbstractButton*>(widget));
        }
    }
    emit notifier().changed();
    // the zone colors: redrawn by whatever shows them, when the theme changed them
    zones::notifyIfChanged();
}

QStringList registerFonts() {
    static std::optional<QStringList> registered;
    if (!registered) {
        QStringList families;
        const QString base = QCoreApplication::applicationDirPath();
        for (const QString& folder : {base + QStringLiteral("/fonts"), base + QStringLiteral("/fonts/variable")}) {
            QStringList names = QDir(folder).entryList({QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files);
            std::sort(names.begin(), names.end());
            for (const QString& name : names) {
                const int id = QFontDatabase::addApplicationFont(QDir(folder).filePath(name));
                if (id < 0) {
                    continue;
                }
                for (const QString& family : QFontDatabase::applicationFontFamilies(id)) {
                    if (!families.contains(family)) {
                        families << family;
                    }
                }
            }
        }
        // some files report a family the font engine can't find (it would quietly use another): only the ones that
        // resolve to themselves
        QStringList usable;
        for (const QString& family : families) {
            if (QFontInfo(QFont(family)).family().toLower() == family.toLower()) {
                usable << family;
            }
        }
        std::sort(usable.begin(), usable.end(), [](const QString& a, const QString& b) {
            return a.toLower() < b.toLower();
        });
        registered = usable;
    }
    return *registered;
}

// ---- Files ----

QJsonObject readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw ThemeError(file.errorString().toStdString());
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        throw ThemeError(error.errorString().toStdString());
    }
    if (!document.isObject()) {
        throw ThemeError("not a theme file");
    }
    return document.object();
}

QString fileKind(const QJsonObject& data) {
    if (data.contains(QStringLiteral("fonts"))) {
        return QStringLiteral("theme");
    }
    QStringList parts;
    for (const QString& part : kParts.keys()) {
        if (data.value(part).isObject()) {
            parts << part;
        }
    }
    if (parts.isEmpty()) {
        throw ThemeError("not a theme, colors or gradients file");
    }
    return parts.size() == 1 ? parts.first() : QStringLiteral("theme");
}

QString loadFile(const QString& path) {
    const QJsonObject data = readFile(path);
    const QString kind = fileKind(data);
    registerFonts();
    if (kind == u"theme") {
        current().fromJson(data);
        partFilesLoaded.clear();
        themeFileLoaded = path;
    } else {
        current().setPart(kind, data.value(kind).toObject());
        partFilesLoaded.insert(kind, path);
    }
    refresh();
    return kind;
}

void loadRandomParts() {
    QJsonObject last = settings::load().value(QStringLiteral("randomThemeLast")).toObject();
    QJsonObject picked;
    static std::mt19937 generator{std::random_device{}()};
    for (const QString& part : kParts.keys()) {
        QDir folder(partDir(part));
        QStringList paths;
        for (const QString& name : folder.entryList({QStringLiteral("*.json")}, QDir::Files)) {
            paths << folder.filePath(name);
        }
        QSet<QString> avoid;
        for (const QString& path : {partFilesLoaded.value(part), last.value(part).toString()}) {
            if (!path.isEmpty()) {
                avoid.insert(normalized(path));
            }
        }
        std::shuffle(paths.begin(), paths.end(), generator);
        // the ones to avoid go last, then the next one if a file can't be used
        std::stable_sort(paths.begin(), paths.end(), [&](const QString& a, const QString& b) {
            return !avoid.contains(normalized(a)) && avoid.contains(normalized(b));
        });
        for (const QString& path : paths) {
            try {
                if (loadFile(path) == part) {
                    picked.insert(part, path);
                    break;
                }
            } catch (const ThemeError& e) {
                log::event(QStringLiteral("ERROR"), QStringLiteral("Failed to load random %1 %2: %3")
                                                        .arg(kParts.value(part), path, QString::fromUtf8(e.what())));
            }
        }
    }
    for (auto it = picked.begin(); it != picked.end(); ++it) {
        last.insert(it.key(), it.value());
    }
    settings::update({{QStringLiteral("randomThemeLast"), last}});
}

QMap<QString, QString> partFiles() {
    return partFilesLoaded;
}

QString themeFile() {
    return themeFileLoaded;
}

QJsonObject basePart(const QString& part) {
    const QString path = themeFileLoaded.isEmpty() ? defaultSource() : themeFileLoaded;
    if (!path.isEmpty()) {
        try {
            return readFile(path).value(part).toObject();
        } catch (const ThemeError& e) {
            log::event(QStringLiteral("ERROR"), QStringLiteral("Failed to reload %1 from %2: %3")
                                                    .arg(kParts.value(part), path, QString::fromUtf8(e.what())));
        }
    }
    return {};
}

void dropPart(const QString& part) {
    partFilesLoaded.remove(part);
    current().setPart(part, basePart(part));
    refresh();
}

Snapshot snapshot() {
    return {current().toJson(), partFilesLoaded, themeFileLoaded};
}

void restore(const Snapshot& saved) {
    themeFileLoaded = saved.file;
    current().fromJson(saved.data);
    partFilesLoaded = saved.parts;
    refresh();
}

void saveFile(const QString& path, const QString& part) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw ThemeError(file.errorString().toStdString());
    }
    file.write(QJsonDocument(current().toJson(part)).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        throw ThemeError(file.errorString().toStdString());
    }
}

void reset() {
    current().reset();
    refresh();
}

void refresh() {
    layout::layoutWindow();
}

QString defaultSource() {
    return QFile::exists(kDefaultThemeFile) ? kDefaultThemeFile : QString();
}

void clear(bool keepParts) {
    themeFileLoaded.clear();
    if (!keepParts) {
        partFilesLoaded.clear();
    }
    loadDefault();
    refresh();
}

void forgetFiles() {
    partFilesLoaded.clear();
    themeFileLoaded.clear();
}

}  // namespace awo::theme

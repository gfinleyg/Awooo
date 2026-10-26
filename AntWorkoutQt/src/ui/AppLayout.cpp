#include "AppLayout.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFontInfo>
#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QMovie>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QStyleOptionButton>
#include <QSvgRenderer>
#include <QTimer>
#include <QWindow>

#include <cmath>

#include "Logger.hpp"
#include "PyCompat.hpp"
#include "Theme.hpp"

namespace awo::layout {

namespace {

constexpr int kWidgetSizeMax = QWIDGETSIZE_MAX;
const QMap<QString, QString> kSymbolFiles = {{QStringLiteral("START"), QStringLiteral("Play.svg")},
                                             {QStringLiteral("PAUSE"), QStringLiteral("Pause.svg")},
                                             {QStringLiteral("END"), QStringLiteral("Stop.svg")}};

QPointer<QWidget> window_;
QString loggedResolution;

QString imagePath(const QString& fileName) {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/images/") + fileName;
}

// Redraws a button's symbol at its new size whenever it's resized or first shown (the layout sizes it just before it
// counts as visible)
class SymbolSizer : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* object, QEvent* event) override {
        if ((event->type() == QEvent::Resize || event->type() == QEvent::Show) && object->property("awoSymbol").toBool()) {
            showSymbol(static_cast<QAbstractButton*>(object));
        }
        return false;
    }
};

// Rule 2 for a window, screen or dialog when it's shown or resized, once that settles
class TextFitter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize) {
            auto* widget = static_cast<QWidget*>(object);
            auto* timer = widget->findChild<QTimer*>(QStringLiteral("awoFitTimer"), Qt::FindDirectChildrenOnly);
            if (!timer) {
                timer = new QTimer(widget);
                timer->setObjectName(QStringLiteral("awoFitTimer"));
                timer->setSingleShot(true);
                timer->setInterval(kTextFitDelayMs);
                QObject::connect(timer, &QTimer::timeout, widget, [widget] {
                    if (widget->isVisible()) {
                        fitText(widget);
                    }
                });
            }
            timer->start();
        }
        return false;
    }
};

TextFitter* textFitter() {
    static TextFitter* fitter = new TextFitter(qApp);
    return fitter;
}

bool exempt(const QWidget* widget) {
    for (; widget; widget = widget->parentWidget()) {
        if (widget->property("awoExempt").toBool()) {
            return true;
        }
    }
    return false;
}

// labels and push buttons showing text the text rule looks after
bool showsText(QWidget* widget) {
    if (widget->property("awoOwnFit").toBool()) {
        return false;
    }
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        return !label->text().trimmed().isEmpty() && label->pixmap().isNull() && !label->movie();
    }
    if (auto* button = qobject_cast<QPushButton*>(widget)) {
        return !button->text().trimmed().isEmpty() && button->icon().isNull();
    }
    return false;
}

// The size a one line label needs for its text: as Qt's size hint works it out (margins, indent, its stylesheet's
// border and padding), but not stretched to the label's minimum size as the hint is, which makes a fixed size label
// always look exactly full (the Python app's shrunk text never grew back for shorter text in one)
QSize labelTextSize(QLabel* label) {
    if (label->textFormat() == Qt::RichText ||
        (label->textFormat() == Qt::AutoText && Qt::mightBeRichText(label->text()))) {
        return label->minimumSizeHint();
    }
    const QSize text = QFontMetrics(label->font()).size(0, label->text());
    const QMargins margins = label->contentsMargins();
    const int margin = 2 * label->margin();
    const int indent = std::max(0, label->indent());
    const Qt::Alignment align = label->alignment();
    const int hextra = margin + ((align & (Qt::AlignLeft | Qt::AlignRight)) ? indent : 0);
    const int vextra = margin + ((align & (Qt::AlignTop | Qt::AlignBottom)) ? indent : 0);
    return {text.width() + hextra + margins.left() + margins.right(),
            text.height() + vextra + margins.top() + margins.bottom()};
}

// the font's size in pixels (a font database not ready yet can't say: from its point size)
int fontPixels(const QWidget* widget) {
    const int px = QFontInfo(widget->font()).pixelSize();
    if (px > 0) {
        return px;
    }
    const QFont font = widget->font();
    return font.pixelSize() > 0 ? font.pixelSize()
                                : static_cast<int>(std::lround(font.pointSizeF() * widget->logicalDpiY() / 72.0));
}

// room the widget has for its text over what the text needs (under 1: it doesn't fit)
double textRoom(QWidget* widget) {
    const double slack = kTextFitSlack;
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        if (label->wordWrap()) {
            const int need = label->heightForWidth(label->width());
            // (a smaller font wraps to fewer, shorter lines: the height needed goes about with its square)
            return need > 0 ? std::sqrt((label->height() + slack) / need) : 1.0;
        }
        const QSize need = labelTextSize(label);
        return std::min((label->width() + slack) / std::max(need.width(), 1),
                        (label->height() + slack) / std::max(need.height(), 1));
    }
    auto* button = static_cast<QPushButton*>(widget);
    QStyleOptionButton option;
    option.initFrom(button);
    option.text = button->text();
    option.features = button->isFlat() ? QStyleOptionButton::Flat : QStyleOptionButton::None;
    // inside the button's border and padding
    const QRect room = button->style()->subElementRect(QStyle::SE_PushButtonContents, &option, button);
    const QFontMetrics metrics = button->fontMetrics();
    QString shown = button->text();
    shown.replace(QStringLiteral("&&"), QStringLiteral("\x01")).remove(u'&').replace(u'\x01', u'&');
    const QStringList lines = shown.split(u'\n');
    int textWidth = 0;
    for (const QString& line : lines) {
        textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
    }
    const int textHeight = metrics.height() + metrics.lineSpacing() * static_cast<int>(lines.size() - 1);
    return std::min((room.width() + slack) / std::max(textWidth, 1), (room.height() + slack) / std::max(textHeight, 1));
}

void setFitPx(QWidget* widget, int px) {
    widget->setProperty("awoFitPx", px);
    if (!theme::hasSheet(widget)) {
        widget->setProperty("awoSheet", QString());
    }
    theme::restyle(widget);
}

// one step of rule 2: a font small enough for the room the widget has now; true when it changed
bool shrinkToFit(QWidget* widget) {
    const double room = textRoom(widget);
    if (room >= 1) {
        return false;
    }
    const int current = fontPixels(widget);
    if (widget->property("awoFitPx").toInt() <= 0) {
        widget->setProperty("awoFitBase", current);
    }
    const int smallest = std::max(6, static_cast<int>(widget->property("awoFitBase").toInt() * kTextFitMin));
    const int px = std::max(smallest, static_cast<int>(current * room));
    if (px >= current) {
        return false;
    }
    setFitPx(widget, px);
    return true;
}

// runs the layout updates size changes have queued, so geometries are current
void settleLayouts(QWidget* root) {
    for (int i = 0; i < 4; ++i) {  // nested layouts pass the change up one level at a time
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
    if (root && root->layout()) {
        root->layout()->activate();
    }
}

void clearTextFit(QWidget* root) {
    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    widgets.prepend(root);
    for (QWidget* widget : widgets) {
        if (widget->property("awoFitPx").toInt() > 0) {
            widget->setProperty("awoFitPx", 0);
        }
    }
}

// scales every fixed/min/max size the screens set, and their layouts' gaps and margins
void applySizes(QWidget* root) {
    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    widgets.prepend(root);
    for (QWidget* widget : widgets) {
        if (!widget->property("awoOrigSize").isValid()) {
            widget->setProperty("awoOrigSize", QVariantList{widget->minimumWidth(), widget->minimumHeight(),
                                                            widget->maximumWidth(), widget->maximumHeight()});
        }
        const QVariantList original = widget->property("awoOrigSize").toList();
        const int minW = original[0].toInt();
        const int minH = original[1].toInt();
        const int maxW = original[2].toInt();
        const int maxH = original[3].toInt();
        if (minW == 0 && minH == 0 && maxW == kWidgetSizeMax && maxH == kWidgetSizeMax) {
            continue;
        }
        const double scale = theme::widgetScale(widget);
        const auto scaled = [scale](int value) {
            return value == 0 || value == kWidgetSizeMax ? value
                                                          : std::max(1, static_cast<int>(py::round(value * scale)));
        };
        if (widget->property("awoSkipHeight").toBool()) {
            widget->setMinimumWidth(scaled(minW));
            widget->setMaximumWidth(scaled(maxW));
            continue;
        }
        widget->setMinimumSize(scaled(minW), scaled(minH));
        widget->setMaximumSize(scaled(maxW), scaled(maxH));
    }
    QList<QLayout*> layouts = root->findChildren<QLayout*>();
    if (root->layout() && !layouts.contains(root->layout())) {
        layouts.prepend(root->layout());
    }
    for (QLayout* layout : layouts) {
        auto* grid = qobject_cast<QGridLayout*>(layout);
        if (!layout->property("awoOrigSpacing").isValid()) {
            const QMargins m = layout->contentsMargins();
            QVariantList spacing = grid ? QVariantList{grid->horizontalSpacing(), grid->verticalSpacing()}
                                        : QVariantList{layout->spacing()};
            layout->setProperty("awoOrigSpacing", spacing);
            layout->setProperty("awoOrigMargins", QVariantList{m.left(), m.top(), m.right(), m.bottom()});
        }
        const QVariantList spacing = layout->property("awoOrigSpacing").toList();
        const QVariantList margins = layout->property("awoOrigMargins").toList();
        // (a popup window shrunk to fit the screen has its gaps shrunk with it, see fitDialog)
        const double scale = theme::uiScale() * theme::layoutShrink(layout->parentWidget());
        layout->setContentsMargins(gap(margins[0].toInt(), scale), gap(margins[1].toInt(), scale),
                                   gap(margins[2].toInt(), scale), gap(margins[3].toInt(), scale));
        if (grid) {
            grid->setHorizontalSpacing(gap(spacing[0].toInt(), scale));
            grid->setVerticalSpacing(gap(spacing[1].toInt(), scale));
        } else {
            layout->setSpacing(gap(spacing[0].toInt(), scale));
        }
    }
}

// rule 1 for a popup window: its own scale steps down until it fits the available area. Its buttons and labels take
// their fonts from the app wide stylesheet, which stays at the app's scale, so it carries its own copy to shrink with
void shrinkToScreen(QWidget* dialog, const QRect& available) {
    double shrink = dialog->property("awoShrink").isValid() ? dialog->property("awoShrink").toDouble() : 1.0;
    QStringList tooBig;
    for (int i = 0; i < kScreenFitTries; ++i) {
        const QSize need = requiredSize(dialog);
        const double fit = std::min(static_cast<double>(available.width()) / std::max(need.width(), 1),
                                    static_cast<double>(available.height()) / std::max(need.height(), 1));
        if (fit >= 1 || theme::uiScale() * shrink <= kMinScale) {
            break;
        }
        tooBig << QStringLiteral("needs %1x%2 at %3").arg(need.width()).arg(need.height())
                      .arg(py::formatG(theme::uiScale() * shrink));
        // (a little under: not everything shrinks in proportion, borders and hairlines)
        shrink = std::max(kMinScale / theme::uiScale(), static_cast<int>(shrink * fit * 0.99 * 1000) / 1000.0);
        dialog->setProperty("awoShrink", shrink);
        dialog->setProperty("awoPrefixSheet", theme::appStyleSheet());
        if (!theme::hasSheet(dialog)) {
            dialog->setProperty("awoSheet", QString());
        }
        QList<QWidget*> widgets = dialog->findChildren<QWidget*>();
        widgets.prepend(dialog);
        for (QWidget* widget : widgets) {
            if (theme::hasSheet(widget)) {
                theme::restyle(widget);
            }
        }
        applySizes(dialog);
    }
    if (!tooBig.isEmpty()) {
        log::event(QStringLiteral("DISPLAY"), QStringLiteral("%1 drawn at %2 to fit %3x%4 (%5)")
                                                  .arg(QString::fromLatin1(dialog->metaObject()->className()))
                                                  .arg(py::formatG(theme::uiScale() * shrink))
                                                  .arg(available.width())
                                                  .arg(available.height())
                                                  .arg(tooBig.join(QStringLiteral("; "))));
    }
}

QMap<QString, QWidget*> screensOf(QWidget* window) {
    QMap<QString, QWidget*> screens;
    for (QWidget* widget : window->findChildren<QWidget*>()) {
        const QString name = widget->property("awoScreen").toString();
        if (!name.isEmpty()) {
            screens.insert(name, widget);
        }
    }
    return screens;
}

}  // namespace

double scaleFor(const QSize& available) {
    const double scale = std::min(static_cast<double>(available.width()) / kDesignAvailable.width(),
                                  static_cast<double>(available.height()) / kDesignAvailable.height());
    return py::round(std::max(kMinScale, std::min(scale, kMaxScale)), 3);
}

int gap(int design, double scale) {
    if (design < 0) {
        design = kDefaultGap;
    }
    if (design == 0) {
        return 0;
    }
    if (design >= kGapGrid) {
        design = static_cast<int>(static_cast<double>(design) / kGapGrid + 0.5) * kGapGrid;
    }
    return std::max(1, static_cast<int>(py::round(design * (scale < 0 ? theme::uiScale() : scale))));
}

QIcon symbolIcon(const QString& fileName, int size, double dpr) {
    const int pixels = std::max(1, static_cast<int>(py::round(size * dpr)));
    QPixmap pixmap(pixels, pixels);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    QSvgRenderer(imagePath(fileName)).render(&painter);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), QColor(theme::current().mapHex(theme::kButtonText)));
    painter.end();
    pixmap.setDevicePixelRatio(dpr);
    return QIcon(pixmap);  // (its disabled look, greyed, comes free)
}

QString text(const QAbstractButton* button) {
    const QVariant original = button->property("awoOrigText");
    return original.isValid() ? original.toString() : button->text();
}

void showSymbol(QAbstractButton* button) {
    if (!button) {
        return;
    }
    const QString shown = text(button);
    const QString fileName = kSymbolFiles.value(shown.trimmed().toUpper());
    if (fileName.isEmpty()) {
        button->setIcon(QIcon());
        button->setText(shown);
        return;
    }
    button->setText(QString());
    button->setToolTip(shown.left(1).toUpper() + shown.mid(1).toLower());
    // a button not on screen still has Qt's default size, and an icon drawn to that would set its minimum size: it's
    // drawn again at its real size when shown
    const int size = button->isVisible()
                         ? std::max(16, static_cast<int>(py::round(std::min(button->width(), button->height()) * kSymbolSize)))
                         : 16;
    button->setIconSize(QSize(size, size));
    button->setIcon(symbolIcon(fileName, size, button->devicePixelRatioF()));
}

void setSymbol(QAbstractButton* button) {
    if (!button->property("awoOrigText").isValid()) {
        button->setProperty("awoOrigText", button->text());
    }
    button->setProperty("awoSymbol", true);
    if (!button->findChild<SymbolSizer*>(QString(), Qt::FindDirectChildrenOnly)) {
        button->installEventFilter(new SymbolSizer(button));
    }
    showSymbol(button);
}

void setText(QLabel* label, const QString& newText) {
    // (the same text again, as the workout screen sets some every tick, is only checked if the size changed)
    const bool unchanged = newText == label->text() && label->size() == label->property("awoFitSize").toSize();
    label->setText(newText);
    if (label->property("awoFitWatch").toBool() && !unchanged) {
        label->setProperty("awoFitSize", label->size());
        textChanged(label);
    }
}

void setText(QAbstractButton* button, const QString& newText) {
    const bool sameText = newText == button->property("awoOrigText").toString() && button->property("awoOrigText").isValid();
    button->setProperty("awoOrigText", newText);
    if (button->property("awoSymbol").toBool()) {
        if (!sameText) {
            showSymbol(button);
        }
        return;
    }
    const bool unchanged = sameText && button->size() == button->property("awoFitSize").toSize();
    button->setText(newText);
    if (button->property("awoFitWatch").toBool() && !unchanged) {
        button->setProperty("awoFitSize", button->size());
        textChanged(button);
    }
}

void markScreen(QWidget* screen, const QString& name) {
    screen->setProperty("awoScreen", name);
}

void setSkipHeight(QWidget* widget, bool skip) {
    widget->setProperty("awoSkipHeight", skip);
}

void setOwnFit(QWidget* widget, bool own) {
    widget->setProperty("awoOwnFit", own);
}

QSize requiredSize(QWidget* screen) {
    // a height pinned to another widget's still has the height from before the scale changed: measured without it
    struct Pinned {
        QWidget* widget;
        int minH;
        int maxH;
    };
    QList<Pinned> pinned;
    for (QWidget* widget : screen->findChildren<QWidget*>()) {
        if (widget->property("awoSkipHeight").toBool()) {
            pinned.append({widget, widget->minimumHeight(), widget->maximumHeight()});
            widget->setMinimumHeight(0);
            widget->setMaximumHeight(kWidgetSizeMax);
        }
    }
    if (!pinned.isEmpty() || !screen->isVisible()) {
        // (Qt doesn't refresh a hidden screen's cached layout sizes when its contents change)
        QList<QLayout*> layouts = screen->findChildren<QLayout*>();
        if (screen->layout()) {
            layouts.prepend(screen->layout());
        }
        for (QLayout* layout : layouts) {
            layout->invalidate();
        }
    }
    const QSize need = screen->minimumSizeHint().expandedTo(screen->minimumSize());
    for (const Pinned& pin : pinned) {
        pin.widget->setMinimumHeight(pin.minH);
        pin.widget->setMaximumHeight(pin.maxH);
    }
    int width = need.width();
    int height = need.height();
    QWidget* parent = screen->parentWidget();
    QLayout* layout = parent ? parent->layout() : nullptr;
    if (layout && layout->indexOf(screen) >= 0) {
        const QMargins margins = layout->contentsMargins();
        width += margins.left() + margins.right();
        height += margins.top() + margins.bottom();
        for (int i = 0; i < layout->count(); ++i) {
            QWidget* other = layout->itemAt(i)->widget();
            // the other screens take turns with this one; anything else (the title bar) is on screen with it
            if (!other || other == screen || other->isHidden() || other->property("awoLayoutScreen").toBool()) {
                continue;
            }
            const QSize otherNeed = other->minimumSizeHint().expandedTo(other->minimumSize());
            width = std::max(width, otherNeed.width() + margins.left() + margins.right());
            height += std::max(other->minimumHeight(), otherNeed.height()) + std::max(layout->spacing(), 0);
        }
    }
    return {width, height};
}

void fitText(QWidget* root) {
    settleLayouts(root->window());
    // (not while root is squeezed below its minimum: it's still being resized, and text fitted to that would stay
    // too small)
    const QSize need = root->minimumSizeHint();
    if (root->width() < need.width() || root->height() < need.height()) {
        return;
    }
    QList<QWidget*> widgets;
    for (QWidget* widget : root->findChildren<QWidget*>()) {
        if (widget->isVisible() && showsText(widget) && !exempt(widget)) {
            widgets << widget;
        }
    }
    for (QWidget* widget : widgets) {
        if (widget->property("awoFitPx").toInt() > 0) {
            setFitPx(widget, 0);
        }
        widget->setProperty("awoFitWatch", true);  // text set later gets checked too
    }
    for (int i = 0; i < 4; ++i) {  // shrinking one can move the others
        settleLayouts(root->window());
        bool any = false;
        for (QWidget* widget : widgets) {
            any = shrinkToFit(widget) || any;
        }
        if (!any) {
            break;
        }
    }
}

void textChanged(QWidget* widget) {
    if (widget->property("awoFitPending").toBool()) {
        return;
    }
    widget->setProperty("awoFitPending", true);
    QPointer<QWidget> guarded(widget);
    QTimer::singleShot(0, widget, [guarded] {
        if (!guarded) {
            return;
        }
        QWidget* w = guarded;
        w->setProperty("awoFitPending", false);
        if (!w->isVisible() || !showsText(w)) {
            return;
        }
        settleLayouts(w->window());
        // room for the text at its own size: back to it (a font made smaller for longer text grows back)
        if (w->property("awoFitPx").toInt() > 0 &&
            textRoom(w) >= w->property("awoFitBase").toDouble() / std::max(1, fontPixels(w))) {
            setFitPx(w, 0);
            settleLayouts(w->window());
        }
        for (int i = 0; i < 3; ++i) {
            if (!shrinkToFit(w)) {
                break;
            }
            settleLayouts(w->window());
        }
    });
}

void watchScreen(QWidget* widget) {
    if (!widget->property("awoTextFitter").toBool()) {
        widget->setProperty("awoTextFitter", true);
        widget->installEventFilter(textFitter());
    }
}

void fitDialog(QWidget* dialog) {
    applySizes(dialog);
    QScreen* screen = (dialog->parentWidget() ? dialog->parentWidget() : dialog)->screen();
    if (screen) {
        shrinkToScreen(dialog, screen->availableGeometry());
    }
    if (screen && dialog->testAttribute(Qt::WA_Resized)) {
        const QRect available = screen->availableGeometry();
        const int width = std::min(static_cast<int>(py::round(dialog->width() * theme::uiScale())), available.width());
        const int height = std::min(static_cast<int>(py::round(dialog->height() * theme::uiScale())), available.height());
        dialog->resize(width, height);
        if (dialog->testAttribute(Qt::WA_Moved)) {
            // (placed for the design size: moved back onto a smaller screen)
            dialog->move(std::max(available.left(), std::min(dialog->x(), available.right() + 1 - width)),
                         std::max(available.top(), std::min(dialog->y(), available.bottom() + 1 - height)));
        }
    }
    watchScreen(dialog);
}

void applyLayout(QWidget* root, const QMap<QString, QWidget*>& screens, double scale, const QSize& available) {
    for (QWidget* screen : screens) {
        screen->setProperty("awoLayoutScreen", true);
        watchScreen(screen);
    }
    clearTextFit(root);
    const double wanted = scale;
    QStringList tooBig;
    for (int i = 0; i < kScreenFitTries; ++i) {
        theme::setUiScale(scale);
        applySizes(root);
        theme::apply();
        if (!available.isValid()) {
            break;
        }
        double fit = 1.0;
        QString worst;
        for (auto it = screens.cbegin(); it != screens.cend(); ++it) {
            const QSize need = requiredSize(it.value());
            const double screenFit = std::min(static_cast<double>(available.width()) / std::max(need.width(), 1),
                                              static_cast<double>(available.height()) / std::max(need.height(), 1));
            if (screenFit < fit) {
                fit = screenFit;
                worst = QStringLiteral("%1 needs %2x%3").arg(it.key()).arg(need.width()).arg(need.height());
            }
        }
        if (fit >= 1 || scale <= kMinScale) {
            break;
        }
        tooBig << QStringLiteral("%1 at %2").arg(worst, py::formatG(scale));
        // (a little under: not everything shrinks in proportion, borders and hairlines)
        scale = std::max(kMinScale, static_cast<int>(scale * fit * 0.99 * 1000) / 1000.0);
    }
    if (scale != wanted) {
        log::event(QStringLiteral("DISPLAY"), QStringLiteral("Layout scale %1 -> %2 so every screen fits %3x%4 (%5)")
                                                  .arg(py::formatG(wanted), py::formatG(scale))
                                                  .arg(available.width())
                                                  .arg(available.height())
                                                  .arg(tooBig.join(QStringLiteral("; "))));
    }
    // (the window's own resizes, to maximized say, fit it again)
    watchScreen(root);
    fitText(root);
    for (QWidget* screen : screens) {
        // the workout message fits itself between the gauges, which may have moved
        if (screen->metaObject()->indexOfMethod("fitWorkoutMessage()") >= 0) {
            QMetaObject::invokeMethod(screen, "fitWorkoutMessage", Qt::QueuedConnection);
        }
    }
}

void layoutWindow(QWidget* window) {
    if (window) {
        window_ = window;
    }
    if (!window_) {
        theme::apply();
        return;
    }
    QWidget* main = window_;
    // moving to another monitor scales to that monitor (the native window only exists once shown)
    if (QWindow* handle = main->windowHandle(); handle && !main->property("awoScreenHooked").toBool()) {
        main->setProperty("awoScreenHooked", true);
        QObject::connect(handle, &QWindow::screenChanged, main, [] { layoutWindow(); });
    }
    QScreen* screen = main->screen();
    const QRect full = screen->geometry();
    const QRect available = screen->availableGeometry();
    // the resolution logged once per monitor and size (this runs again as each screen is built)
    const double dpr = screen->devicePixelRatio();
    const QString resolution = QStringLiteral("%1 %2x%3 %4x%5 %6").arg(screen->name()).arg(full.width()).arg(full.height())
                                   .arg(available.width()).arg(available.height()).arg(dpr);
    if (resolution != loggedResolution) {
        loggedResolution = resolution;
        log::event(QStringLiteral("DISPLAY"),
                   QStringLiteral("Screen %1: %2x%3 effective (%4x%5 physical, %6x scaling), available %7x%8")
                       .arg(screen->name())
                       .arg(full.width())
                       .arg(full.height())
                       .arg(static_cast<int>(py::round(full.width() * dpr)))
                       .arg(static_cast<int>(py::round(full.height() * dpr)))
                       .arg(py::formatG(dpr))
                       .arg(available.width())
                       .arg(available.height()));
    }
    applyLayout(main, screensOf(main), scaleFor(available.size()), available.size());
}

QWidget* mainWindow() {
    return window_;
}

}  // namespace awo::layout

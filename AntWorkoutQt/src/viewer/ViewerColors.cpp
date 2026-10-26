#include "ViewerColors.hpp"

#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <memory>

#include "SmoothProgressBar.hpp"
#include "SynopsisScreen.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"

namespace awo::viewer {

namespace {

const QRegularExpression& gradientRe() {
    static const QRegularExpression re(QStringLiteral(R"(q(?:linear|radial|conical)gradient\([^)]*\))"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

// (as the theme reads colors: "#rrggbb", or Qt's "#aarrggbb")
const QRegularExpression& hexRe() {
    static const QRegularExpression re(QStringLiteral(R"(#([0-9a-fA-F]{8}|[0-9a-fA-F]{6})\b)"));
    return re;
}

}  // namespace

std::pair<QString, QSet<Gradient>> splitGradients(const QString& cssIn) {
    QString css = cssIn;
    QSet<Gradient> gradients;
    QRegularExpressionMatchIterator found = gradientRe().globalMatch(cssIn);
    while (found.hasNext()) {
        const QString gradient = found.next().captured(0);
        Gradient colors;
        QRegularExpressionMatchIterator hex = hexRe().globalMatch(gradient);
        while (hex.hasNext()) {
            colors.append((u'#' + hex.next().captured(1).right(6)).toLower());
        }
        if (QSet<QString>(colors.begin(), colors.end()).size() > 1) {
            gradients.insert(colors);
        } else if (!colors.isEmpty()) {
            css += u' ' + colors.first();
        }
    }
    css.remove(gradientRe());
    return {css, gradients};
}

WidgetColors widgetColors(const QWidget* widget) {
    auto [rest, gradients] = splitGradients(theme::originalSheet(widget));
    QSet<QString> singles = theme::colorsInCss(rest);
    if (const auto* bar = qobject_cast<const SmoothProgressBar*>(widget)) {
        if (const auto colors = bar->originalBarColors()) {
            singles.insert(colors->first.name());
            singles.insert(colors->second.name());
        }
    }
    for (const QString& color : theme::paintedColors(widget)) {
        singles.insert(color.toLower());
    }
    if (widget->property("awoFtpColored").toBool()) {
        // the power zone colors are on the FTP Colors tab instead
        const QSet<QString> zoneColors = zones::allColors();
        singles -= zoneColors;
        QSet<Gradient> kept;
        for (const Gradient& gradient : gradients) {
            if (!QSet<QString>(gradient.begin(), gradient.end()).intersects(zoneColors)) {
                kept.insert(gradient);
            }
        }
        gradients = kept;
    }
    if (qobject_cast<const SynopsisScreen*>(widget) || widget->objectName() == u"synopsisGraphFrame") {
        singles.insert(theme::kPlotLabelBg);
    }
    return {singles, gradients};
}

// ---- GradientPreview ----

GradientPreview::GradientPreview(const Gradient& colors, std::function<void()> onClick)
    : colors_(colors), onClick_(std::move(onClick)) {
    setMinimumSize(110, 24);
    setCursor(Qt::PointingHandCursor);
}

void GradientPreview::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient gradient(rect.topLeft(), rect.topRight());
    for (int i = 0; i < colors_.size(); ++i) {
        gradient.setColorAt(static_cast<double>(i) / std::max<qsizetype>(colors_.size() - 1, 1),
                            QColor(theme::current().mapHex(colors_[i])));
    }
    painter.setBrush(gradient);
    painter.setPen(highlight_ ? QPen(QColor(QStringLiteral("#ffeb3b")), 4) : QPen(QColor(QStringLiteral("#444444"))));
    painter.drawRoundedRect(highlight_ ? rect.adjusted(1.5, 1.5, -1.5, -1.5) : rect, 4, 4);
}

void GradientPreview::setHighlight(bool on) {
    highlight_ = on;
    update();
}

void GradientPreview::mousePressEvent(QMouseEvent*) {
    onClick_();
}

// ---- ClickableLabel ----

ClickableLabel::ClickableLabel(const QString& text, std::function<void()> onClick)
    : QLabel(text), onClick_(std::move(onClick)) {
    setCursor(Qt::PointingHandCursor);
}

void ClickableLabel::mousePressEvent(QMouseEvent*) {
    onClick_();
}

// ---- OutlineFlash ----

OutlineFlash* OutlineFlash::flash(QWidget* display, QWidget* widget, const QString& color) {
    if (widget != display && !display->isAncestorOf(widget)) {
        return nullptr;
    }
    return new OutlineFlash(display, widget, color);
}

OutlineFlash::OutlineFlash(QWidget* display, QWidget* widget, const QString& color)
    : QWidget(display), color_(color), timer_(new QTimer(this)) {
    // (the layout scale never applies to it)
    theme::setExempt(this);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    const QPoint topLeft = widget->mapTo(display, QPoint(0, 0));
    setGeometry(QRect(topLeft, widget->size()).adjusted(-3, -3, 3, 3));
    connect(timer_, &QTimer::timeout, this, &OutlineFlash::blink);
    timer_->start(kBlinkMs);
    raise();
    show();
}

void OutlineFlash::blink() {
    if (++blinks_ >= kBlinks) {
        timer_->stop();
        deleteLater();
        return;
    }
    setVisible(!isVisible());
}

void OutlineFlash::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setPen(QPen(color_, 4));
    painter.drawRect(QRectF(rect()).adjusted(2, 2, -2, -2));
}

// ---- blinkWidgets ----

void blinkWidgets(const QList<QWidget*>& widgetsIn, int times, int interval) {
    if (widgetsIn.isEmpty()) {
        return;
    }
    QList<QPointer<QWidget>> widgets(widgetsIn.begin(), widgetsIn.end());
    auto* timer = new QTimer(widgetsIn.first());
    auto count = std::make_shared<int>(0);
    const auto step = [widgets, timer, count, times] {
        ++*count;
        const bool on = *count % 2 == 1 && *count < times;
        for (QWidget* widget : widgets) {
            if (!widget) {
                continue;
            }
            if (auto* preview = qobject_cast<GradientPreview*>(widget)) {
                preview->setHighlight(on);
                continue;
            }
            // through the palette: a stylesheet would also swap in the app wide (theme's) font
            if (!widget->property("awoBlinkPalette").isValid()) {
                widget->setProperty("awoBlinkPalette", QVariant::fromValue(QPalette(widget->palette())));
                widget->setProperty("awoBlinkAutoFill", widget->autoFillBackground());
            }
            const QPalette palette = widget->property("awoBlinkPalette").value<QPalette>();
            if (on) {
                QPalette highlighted(palette);
                highlighted.setColor(QPalette::Window, QColor(QStringLiteral("#ffeb3b")));
                widget->setPalette(highlighted);
                widget->setAutoFillBackground(true);
            } else {
                widget->setPalette(palette);
                widget->setAutoFillBackground(widget->property("awoBlinkAutoFill").toBool());
            }
        }
        if (*count >= times) {
            timer->stop();
            timer->deleteLater();
        }
    };
    QObject::connect(timer, &QTimer::timeout, timer, step);
    timer->start(interval);
    step();
}

}  // namespace awo::viewer

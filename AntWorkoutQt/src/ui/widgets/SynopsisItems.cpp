#include "SynopsisItems.hpp"

#include <QFontMetricsF>
#include <QGraphicsSceneHoverEvent>
#include <QPainter>

#include <cmath>

#include "PyCompat.hpp"
#include "Theme.hpp"
#include "WorkoutGraph.hpp"
#include "WorkoutHelpers.hpp"

namespace awo {

// ---- PlotLabel ----

const QColor PlotLabel::kBorderColor(0, 0, 0, 215);

PlotLabel::PlotLabel(const QString& text) : QGraphicsSimpleTextItem(text) {}

void PlotLabel::setBorderColor(const QColor& color, double width) {
    borderColor_ = color;
    borderWidth_ = width;
    update();
}

void PlotLabel::setLabelFont(const QFont& font) {
    font_ = font;
    setFont(theme::current().mapFont(font_, theme::uiScale()));
}

void PlotLabel::retheme() {
    setFont(theme::current().mapFont(font_, theme::uiScale()));
    update();
}

QRectF PlotLabel::boundingRect() const {
    return QGraphicsSimpleTextItem::boundingRect().adjusted(-kPadX, -kPadY, kPadX, kPadY);
}

QPainterPath PlotLabel::shape() const {
    QPainterPath path;
    path.addRect(boundingRect());
    return path;
}

void PlotLabel::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(theme::themedPen(QPen(borderColor_, borderWidth_)));
    painter->setBrush(QColor(theme::current().mapHex(theme::kPlotLabelBg)));
    painter->drawRoundedRect(boundingRect().adjusted(0.6, 0.6, -0.6, -0.6), kRadius, kRadius);
    // the text in the theme's color for black (only set when it changes: setting it repaints)
    const QColor text(theme::current().mapHex(QStringLiteral("#000000")));
    if (brush().color() != text) {
        setBrush(text);
    }
    QGraphicsSimpleTextItem::paint(painter, option, widget);
}

bool PlotLabel::click(const QPointF&, Qt::MouseButton button) {
    if (!onClick || button != Qt::LeftButton) {
        return false;
    }
    onClick();
    return true;
}

// ---- GradeRegion ----

GradeRegion::GradeRegion(const QRectF& rect, const GradeRegionKey& key, plot::ViewBox* vbox,
                         std::function<void(GradeRegion*)> onClick)
    : QGraphicsRectItem(rect), key_(key), vbox_(vbox), onClick_(std::move(onClick)) {
    setPen(Qt::NoPen);
    setCursor(Qt::PointingHandCursor);
    setAcceptedMouseButtons(Qt::NoButton);
}

bool GradeRegion::click(const QPointF&, Qt::MouseButton button) {
    if (button != Qt::LeftButton) {
        return false;
    }
    onClick_(this);
    return true;
}

// ---- BandStrip ----

const QColor BandStrip::kSongColors[2] = {QColor(40, 39, 94, 215), QColor(72, 98, 140, 215)};
const QColor BandStrip::kRideColors[2] = {QColor(18, 18, 48, 225), QColor(52, 70, 110, 225)};

BandStrip::BandStrip(const QList<Band>& bands, const QColor (&colors)[2], plot::ViewBox* vbox,
                     std::function<void(int)> onClick)
    : bands_(bands), colors_{colors[0], colors[1]}, vbox_(vbox), onClick_(std::move(onClick)) {
    setFont();
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::NoButton);
    setCursor(Qt::PointingHandCursor);
    // over the curves, under the max labels and their leader lines
    setZValue(998);
}

void BandStrip::setFont() {
    const double points =
        theme::current().mapFont(QFont(QStringLiteral("Orbitron"), static_cast<int>(PlotLabel::kFontPoints)), theme::uiScale())
            .pointSizeF();
    font_ = QFont(QStringLiteral("Segoe UI"));
    font_.setPointSizeF(points);
    font_.setBold(true);
    height_ = py::round(QFontMetricsF(font_).height() + 8);
}

void BandStrip::layout() {
    const QRectF area = vbox_->sceneBoundingRect();
    prepareGeometryChange();
    setFont();
    width_ = area.width();
    setPos(area.left(), area.top() + top);
    update();
}

QRectF BandStrip::boundingRect() const {
    return QRectF(0, 0, width_, height_);
}

QList<std::tuple<int, double, double>> BandStrip::spans() const {
    const plot::Range range = vbox_->xRange();
    QList<std::tuple<int, double, double>> spans;
    if (range.max <= range.min || width_ <= 0) {
        return spans;
    }
    const double scale = width_ / (range.max - range.min);
    for (int index = 0; index < bands_.size(); ++index) {
        const double left = std::max((bands_[index].start - range.min) * scale, 0.0);
        const double right = std::min((bands_[index].end - range.min) * scale, width_);
        if (right > left) {
            spans.append({index, left, right});
        }
    }
    return spans;
}

std::optional<int> BandStrip::bandAt(double x) const {
    for (const auto& [index, left, right] : spans()) {
        if (left <= x && x < right) {
            return index;
        }
    }
    return std::nullopt;
}

void BandStrip::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setFont(font_);
    const QFontMetricsF metrics(font_);
    // (too narrow for a few letters: no title, hovering still shows it)
    const double minText = metrics.horizontalAdvance(QStringLiteral("Abc…"));
    for (const auto& [index, left, right] : spans()) {
        const QRectF rect(left, 0, right - left, height_);
        painter->fillRect(rect, colors_[index % 2]);
        const double textWidth = rect.width() - 2 * kPadX;
        if (textWidth < minText) {
            continue;
        }
        const QString text = metrics.elidedText(bands_[index].title, Qt::ElideRight, textWidth);
        painter->setPen(QColor(QStringLiteral("white")));
        painter->drawText(rect.adjusted(kPadX, 0, -kPadX, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

void BandStrip::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    const std::optional<int> index = bandAt(event->pos().x());
    if (!index) {
        setToolTip(QString());
        return;
    }
    const Band& band = bands_[*index];
    setToolTip(QStringLiteral("%1\n%2 to %3  (%4)")
                   .arg(band.title, hourMinuteSecondFromSecond(band.start), hourMinuteSecondFromSecond(band.end),
                        minimizedDuration(py::round(band.end - band.start))));
}

bool BandStrip::click(const QPointF& scenePos, Qt::MouseButton button) {
    if (button != Qt::LeftButton) {
        return false;
    }
    const std::optional<int> index = bandAt(mapFromScene(scenePos).x());
    if (!index) {
        return false;
    }
    onClick_(*index);
    return true;
}

}  // namespace awo

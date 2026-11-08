#include "BlockHighlight.hpp"

#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "HatchedLabel.hpp"
#include "Theme.hpp"

namespace awo {

namespace {

// the glow round the swollen box: its widest line (design px) and its color, at the swell's height
constexpr double kGlowWidth = 9;
const QColor kGlow(255, 250, 215);
constexpr int kGlowAlpha[] = {14, 26, 48};

double easeInOut(double p) {
    return p < 0.5 ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3) / 2;
}

QRectF lerp(const QRectF& a, const QRectF& b, double f) {
    return {a.left() + (b.left() - a.left()) * f, a.top() + (b.top() - a.top()) * f,
            a.width() + (b.width() - a.width()) * f, a.height() + (b.height() - a.height()) * f};
}

void scaleAbout(QPainter& painter, const QPointF& center, const QSizeF& scale) {
    painter.translate(center);
    painter.scale(scale.width(), scale.height());
    painter.translate(-center);
}

// how far the swell and its glow reach past the labels
double reach(double width, double scale) {
    return width * BlockHighlight::kSwellScale / 2 + kGlowWidth * scale + 2;
}

}  // namespace

BlockHighlight::BlockHighlight(QWidget* area, QWidget* parent) : QWidget(parent), area_(area) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    area->installEventFilter(this);
    place();
    raise();
}

void BlockHighlight::setRows(const QList<QLabel*>& rows, int index) {
    const bool wasDrawing = drawing();
    for (const QPointer<QLabel>& row : rows_) {
        if (row) {
            row->removeEventFilter(this);
        }
    }
    rows_.clear();
    for (QLabel* row : rows) {
        rows_.append(row);
        row->installEventFilter(this);
    }
    index_ = rows.isEmpty() ? 0 : std::clamp(index, 0, static_cast<int>(rows.size()) - 1);
    position_ = index_;
    sliding_ = false;
    swell_ = 0;
    lit_.clear();
    dim_.clear();
    stale_ = true;
    updateDrawn(wasDrawing);
}

void BlockHighlight::setActive(int index) {
    if (rows_.isEmpty() || index < 0 || index >= rows_.size() || index == index_) {
        return;
    }
    // (from wherever it is: part way through a slide, from there)
    slideSeconds_ = kSlideSecondsPerRow * std::max(1.0, std::abs(index - position_));
    fromPosition_ = position_;
    index_ = index;
    sliding_ = true;
    // (the labels' lit and dimmed look have just changed over)
    stale_ = true;
}

void BlockHighlight::setBlockTime(double elapsed, double remaining, bool swell) {
    // (a hidden screen's moves and resizes reach it only once it's shown: wherever the labels are now)
    place();
    const bool wasDrawing = drawing();
    if (sliding_) {
        const double p = slideSeconds_ > 0 ? elapsed / slideSeconds_ : 1;
        if (p >= 1) {
            sliding_ = false;
            position_ = index_;
        } else {
            position_ = fromPosition_ + (index_ - fromPosition_) * easeInOut(std::max(0.0, p));
        }
    }
    swell_ = 0;
    if (swell && !rows_.isEmpty() && remaining > 0 && remaining < kSwellWindowSeconds) {
        const double x = (kSwellWindowSeconds - remaining) / kSwellPeriodSeconds;
        swell_ = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * x);
        swellRoom_ = roomToSwell();
    }
    if (drawing() && stale_) {
        grabRows();
    }
    updateDrawn(wasDrawing);
}

// how much wider and taller (fractions of its size, kSwellScale at most) the lit box can grow about its middle and
// stay kSwellClearance px clear of every widget shown around it: the lit row's siblings (the interval's other rows, its
// count, the block bar), its parents' siblings out to the window, and the window's edges. Each way is held back on its
// own, so an interval's row, flush with the rows above and below it, only widens. A widget the box already overlaps
// (one it lies within, or a row it's sliding across) can't be touched by its growing, so doesn't count.
QSizeF BlockHighlight::roomToSwell() const {
    const QLabel* lit = rows_.value(index_);
    const QWidget* top = window();
    const QRectF box = litRect();
    if (!lit || !top || box.width() <= 0 || box.height() <= 0) {
        return {kSwellScale, kSwellScale};
    }
    const double clearance = std::max(1.0, kSwellClearance * theme::widgetScale(this));
    // (growing by g, each side moves out by half of g times the box's size)
    const double perX = 2 / box.width();
    const double perY = 2 / box.height();
    const QRectF bounds(mapFrom(top, QPoint()), QSizeF(top->size()));
    double roomX = std::min({kSwellScale, (box.left() - bounds.left() - clearance) * perX,
                             (bounds.right() - box.right() - clearance) * perX});
    double roomY = std::min({kSwellScale, (box.top() - bounds.top() - clearance) * perY,
                             (bounds.bottom() - box.bottom() - clearance) * perY});
    // (how far each way the box can grow before it reaches each widget, -1 where it's alongside it already)
    QList<QPointF> corners;
    for (const QWidget* level = lit; level && level != top; level = level->parentWidget()) {
        const QWidget* parent = level->parentWidget();
        if (!parent) {
            break;
        }
        for (const QObject* child : parent->children()) {
            const auto* other = qobject_cast<const QWidget*>(child);
            if (!other || other == level || other == this || other->isWindow() || !other->isVisible()) {
                continue;
            }
            const QRectF rect(QPointF(mapFrom(top, other->mapTo(top, QPoint()))), QSizeF(other->size()));
            if (rect.isEmpty() || rect.intersects(box)) {
                continue;
            }
            const double apartX = std::max(rect.left() - box.right(), box.left() - rect.right());
            const double apartY = std::max(rect.top() - box.bottom(), box.top() - rect.bottom());
            if (apartX <= 0) {
                // above or below it: it's reached growing taller
                roomY = std::min(roomY, (apartY - clearance) * perY);
            } else if (apartY <= 0) {
                // beside it: growing wider
                roomX = std::min(roomX, (apartX - clearance) * perX);
            } else {
                corners.append({(apartX - clearance) * perX, (apartY - clearance) * perY});
            }
        }
    }
    // one off a corner is reached only by growing both ways that far: kept short of it the way that gives up less
    for (const QPointF& corner : corners) {
        if (roomX > corner.x() && roomY > corner.y()) {
            if (corner.x() / roomX >= corner.y() / roomY) {
                roomX = corner.x();
            } else {
                roomY = corner.y();
            }
        }
    }
    return {std::max(0.0, roomX), std::max(0.0, roomY)};
}

// each label's look lit and dimmed, as it draws itself (its colors, text, lines and blink)
void BlockHighlight::grabRows() {
    grabbing_ = true;
    lit_.clear();
    dim_.clear();
    for (const QPointer<QLabel>& row : rows_) {
        if (!row) {
            lit_.append(QPixmap());
            dim_.append(QPixmap());
            continue;
        }
        const bool enabled = row->isEnabled();
        row->setEnabled(true);
        lit_.append(row->grab());
        row->setEnabled(false);
        dim_.append(row->grab());
        row->setEnabled(enabled);
    }
    grabbing_ = false;
    stale_ = false;
}

void BlockHighlight::place() {
    if (!area_ || area_->parentWidget() != parentWidget()) {
        return;
    }
    const int margin = static_cast<int>(std::ceil(reach(area_->width(), theme::widgetScale(this))));
    setGeometry(area_->geometry().adjusted(-margin, -margin, margin, margin));
    setVisible(area_->isVisibleTo(parentWidget()));
    raise();
}

QRectF BlockHighlight::rowRect(int row) const {
    const QLabel* label = rows_.value(row);
    if (!label) {
        return {};
    }
    return {QPointF(mapFrom(window(), label->mapTo(window(), QPoint()))), QSizeF(label->size())};
}

// the lit box: its row's, or part way from one row's to the next's
QRectF BlockHighlight::litRect() const {
    const int first = std::clamp(static_cast<int>(std::floor(position_)), 0, static_cast<int>(rows_.size()) - 1);
    const int second = std::min(first + 1, static_cast<int>(rows_.size()) - 1);
    return lerp(rowRect(first), rowRect(second), std::clamp(position_ - first, 0.0, 1.0));
}

double BlockHighlight::radius(const QRectF& box) const {
    return std::min(std::max(1.0, std::round(HatchedLabel::kBoxRadius * theme::widgetScale(this))), box.height() / 2);
}

// the labels' box and as far past it as the swell reaches, again: now, and to take away what was drawn before
void BlockHighlight::updateDrawn(bool wasDrawing) {
    if (!wasDrawing && !drawing()) {
        return;
    }
    QRectF box;
    for (int i = 0; i < rows_.size(); ++i) {
        box |= rowRect(i);
    }
    if (box.isEmpty()) {
        update();
        return;
    }
    const double margin = reach(box.width(), theme::widgetScale(this));
    update(box.adjusted(-margin, -margin, margin, margin).toAlignedRect());
}

void BlockHighlight::paintEvent(QPaintEvent*) {
    if (!drawing() || rows_.isEmpty() || lit_.size() != rows_.size() || !rows_.value(index_) ||
        !rows_.value(index_)->isVisible()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const QSizeF scale = swellScale();
    const QRectF box = litRect();
    // (each row within its own rounded box: a label's grab fills its corners, which its sheet leaves showing what's
    // behind it)
    const auto drawRow = [this](QPainter& p, int row, const QList<QPixmap>& pixmaps) {
        const QPixmap& pixmap = pixmaps.value(row);
        if (!pixmap.isNull()) {
            const QRectF rect = rowRect(row);
            QPainterPath rounded;
            rounded.addRoundedRect(rect, radius(rect), radius(rect));
            // (filled with the grab, for smooth corners: a clip's are jagged)
            QBrush brush(pixmap);
            QTransform transform;
            transform.translate(rect.left(), rect.top());
            transform.scale(rect.width() / pixmap.width(), rect.height() / pixmap.height());
            brush.setTransform(transform);
            p.fillPath(rounded, brush);
        }
    };
    if (swell_ > 0) {
        // a faint glow round the box, its own size times the swell
        painter.save();
        scaleAbout(painter, box.center(), scale);
        QPainterPath outline;
        outline.addRoundedRect(box, radius(box), radius(box));
        const double widgetScale = theme::widgetScale(this);
        for (int i = 0; i < 3; ++i) {
            QColor color = kGlow;
            color.setAlpha(static_cast<int>(kGlowAlpha[i] * swell_));
            painter.strokePath(outline, QPen(color, kGlowWidth * widgetScale * (3 - i) / 3));
        }
        painter.restore();
    }
    if (!sliding_) {
        // the lit box, swollen
        scaleAbout(painter, box.center(), scale);
        drawRow(painter, index_, lit_);
        return;
    }
    // every row dimmed (the one being slid to too, until the box gets there), and through the moving box each row lit
    for (int i = 0; i < rows_.size(); ++i) {
        drawRow(painter, i, dim_);
    }
    const double ratio = devicePixelRatioF();
    QImage layer(size() * ratio, QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(ratio);
    layer.fill(Qt::transparent);
    QPainter litPainter(&layer);
    litPainter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    scaleAbout(litPainter, box.center(), scale);
    // the box's shape (smooth edged), then the lit rows only where it is
    QPainterPath shape;
    shape.addRoundedRect(box, radius(box), radius(box));
    litPainter.fillPath(shape, Qt::black);
    litPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    for (int i = 0; i < rows_.size(); ++i) {
        drawRow(litPainter, i, lit_);
    }
    litPainter.end();
    painter.drawImage(QPointF(0, 0), layer);
}

bool BlockHighlight::eventFilter(QObject* watched, QEvent* event) {
    if (watched == area_) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
            place();
            break;
        default:
            break;
        }
    } else if (!grabbing_) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::StyleChange:
        case QEvent::FontChange:
        case QEvent::PaletteChange:
        case QEvent::Show:
        case QEvent::Hide:
            stale_ = true;
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace awo

#include "ScrollingLabel.hpp"

#include <QPainter>
#include <QStyle>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "AppLayout.hpp"
#include "Theme.hpp"

namespace awo {

ScrollingLabel::ScrollingLabel(const QString& text, QWidget* parent) : QLabel(text, parent), timer_(new QTimer(this)) {
    timer_->setInterval(kStepMs);
    connect(timer_, &QTimer::timeout, this, &ScrollingLabel::step);
    since_.start();
}

void ScrollingLabel::setScrolling(bool on) {
    scrolling_ = on;
    layout::setOwnFit(this, on);
    updateTimer();
    update();
}

QRect ScrollingLabel::textArea() const {
    return contentsRect().adjusted(margin(), margin(), -margin(), -margin());
}

bool ScrollingLabel::overflows() const {
    const int room = textArea().width();
    const QFontMetrics metrics = fontMetrics();
    const QStringList lines = text().split(u'\n');
    return std::any_of(lines.begin(), lines.end(),
                       [&](const QString& line) { return metrics.horizontalAdvance(line) > room; });
}

double ScrollingLabel::offsetAt(double seconds, double overflow, double speed) {
    if (overflow <= 0 || speed <= 0) {
        return 0;
    }
    const double travel = overflow / speed;
    const double at = std::fmod(seconds, 2 * (kPauseSeconds + travel));
    if (at < kPauseSeconds) {
        return 0;
    }
    if (at < kPauseSeconds + travel) {
        return (at - kPauseSeconds) * speed;
    }
    if (at < 2 * kPauseSeconds + travel) {
        return overflow;
    }
    return overflow - (at - 2 * kPauseSeconds - travel) * speed;
}

double ScrollingLabel::lineOffset(int line) const {
    const QStringList lines = text().split(u'\n');
    if (!scrolling_ || line < 0 || line >= lines.size()) {
        return 0;
    }
    const double overflow = fontMetrics().horizontalAdvance(lines[line]) - textArea().width();
    const double seconds = text() == scrolledText_ ? since_.elapsed() / 1000.0 : 0.0;
    return offsetAt(seconds, overflow, kSpeed * theme::widgetScale(this));
}

void ScrollingLabel::step() {
    if (text() != scrolledText_) {
        scrolledText_ = text();
        since_.restart();
    }
    if (overflows()) {
        update();
    }
}

void ScrollingLabel::updateTimer() {
    if (scrolling_ && isVisible()) {
        timer_->start();
    } else {
        timer_->stop();
    }
}

void ScrollingLabel::showEvent(QShowEvent* event) {
    QLabel::showEvent(event);
    updateTimer();
}

void ScrollingLabel::hideEvent(QHideEvent* event) {
    QLabel::hideEvent(event);
    updateTimer();
}

void ScrollingLabel::paintEvent(QPaintEvent* event) {
    if (!scrolling_ || !overflows()) {
        QLabel::paintEvent(event);
        return;
    }
    // the box (its stylesheet's background and border), then each line as QLabel would place it, the long ones
    // moved along by their offset and cut off at the box's edges
    QFrame::paintEvent(event);
    QPainter painter(this);
    const QRect area = textArea();
    painter.setClipRect(area);
    const QFontMetrics metrics = fontMetrics();
    const QStringList lines = text().split(u'\n');
    const int blockHeight = metrics.height() + metrics.lineSpacing() * static_cast<int>(lines.size() - 1);
    const Qt::Alignment align = QStyle::visualAlignment(layoutDirection(), alignment());
    int top = area.top() + (area.height() - blockHeight) / 2;
    if (align & Qt::AlignTop) {
        top = area.top();
    } else if (align & Qt::AlignBottom) {
        top = area.bottom() + 1 - blockHeight;
    }
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const int y = top + static_cast<int>(i) * metrics.lineSpacing();
        const int width = metrics.horizontalAdvance(lines[i]);
        QRect lineRect(area.left(), y, area.width(), metrics.height());
        Qt::Alignment lineAlign = (align & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter;
        if (width > area.width()) {
            lineRect = QRect(area.left() - static_cast<int>(std::lround(lineOffset(static_cast<int>(i)))), y, width,
                             metrics.height());
            lineAlign = Qt::AlignLeft | Qt::AlignVCenter;
        }
        style()->drawItemText(&painter, lineRect, static_cast<int>(lineAlign), palette(), isEnabled(), lines[i],
                              foregroundRole());
    }
}

}  // namespace awo

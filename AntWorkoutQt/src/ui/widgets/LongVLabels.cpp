#include "LongVLabels.hpp"

#include <QLabel>
#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "AppLayout.hpp"
#include "AppState.hpp"
#include "Theme.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

const QString kMoreText = QStringLiteral("more...");
const QString kAlmostDone = QStringLiteral("Almost done!");

}  // namespace

LongVLabels::LongVLabels(const ftpcolors::ZoneRanges& ranges, QWidget* parent)
    : QWidget(parent), ftpRanges(ranges), labelLayout_(new QVBoxLayout), renderTimer_(new QTimer(this)) {
    // two layouts: one the widget's size, one holding the labels
    auto* main = new QVBoxLayout(this);
    // (the labels flush with the Up Next box)
    main->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < kWidgetCount; ++i) {
        auto* label = new QLabel;
        label->setFixedSize(500, 42);
        // (LayoutViewer lists them with the zone colored widgets)
        label->setProperty("awoFtpColored", true);
        labelLayout_->addWidget(label);
        label->hide();
        labels_.append(label);
    }
    main->addLayout(labelLayout_);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(0);
    connect(renderTimer_, &QTimer::timeout, this, &LongVLabels::renderLines);
    connect(this, &LongVLabels::textChanged, renderTimer_, qOverload<>(&QTimer::start));
    // redrawn when the zone colors change
    connect(&zones::notifier(), &zones::Notifier::changed, this, &LongVLabels::renderLines);
}

// a label's height and the layout's spacing, at the layout scale
double LongVLabels::rowHeight() const {
    return labels_.front()->minimumHeight() + std::max(labelLayout_->spacing(), 0);
}

// as many rows as fit, so it needs room for just one: the rows it shows mustn't keep it (and the screen) from getting
// smaller, e.g. on a smaller screen
QSize LongVLabels::minimumSizeHint() const {
    return {QWidget::minimumSizeHint().width(), static_cast<int>(std::ceil(rowHeight()))};
}

// the window of lines from scrollOffset, with a "more..." block in the top or bottom slot whenever lines are scrolled
// out of view above or below
void LongVLabels::renderLines() {
    for (QLabel* label : labels_) {
        label->hide();
    }
    const int maxLines = std::min(static_cast<int>(geometry().height() / rowHeight()), kWidgetCount);
    QStringList lines;
    for (const QString& line : fullText_.split(u'\n')) {
        if (!line.trimmed().isEmpty()) {
            lines.append(line);
        }
    }
    QStringList shown;
    if (fullText_.isEmpty()) {
        if (showAlmostDone) {
            shown = {kAlmostDone};
        }
    } else if (fullText_ == QLatin1String("DONE")) {
        const QRect geo = geometry();
        setFixedSize(geo.width(), geo.height());
        return;
    } else if (lines.size() <= maxLines || maxLines < 3) {
        // all the text fits (or there's no room for "more..." blocks): no scrolling
        scrollOffset_ = 0;
        shown = lines.mid(0, maxLines);
    } else {
        // the furthest scroll leaves the last line in the bottom slot, beneath a top "more..."
        scrollOffset_ = std::max(0, std::min(scrollOffset_, static_cast<int>(lines.size()) - maxLines + 1));
        if (scrollOffset_ > 0) {
            shown.append(kMoreText);
        }
        const qsizetype available = maxLines - shown.size();
        const QStringList remaining = lines.mid(scrollOffset_);
        if (remaining.size() > available) {
            shown.append(remaining.mid(0, available - 1));
            shown.append(kMoreText);
        } else {
            shown.append(remaining);
        }
    }
    for (qsizetype i = 0; i < shown.size(); ++i) {
        theme::setSheet(labels_[i], labelColor(shown[i]));
        layout::setText(labels_[i], shown[i]);
        labels_[i]->show();
    }
}

void LongVLabels::scrollTo(int offset) {
    if (offset != scrollOffset_) {
        // (renderLines holds the bottom end)
        scrollOffset_ = std::max(0, offset);
        renderLines();
    }
}

// a line a wheel notch (120); touchpads send smaller steps that add up
void LongVLabels::wheelEvent(QWheelEvent* event) {
    wheelRemainder_ += event->angleDelta().y();
    const int steps = wheelRemainder_ / 120;
    if (steps) {
        wheelRemainder_ -= steps * 120;
        scrollTo(scrollOffset_ - steps);
    }
    event->accept();
}

// drag (mouse or touch) to scroll, a line a row height dragged
void LongVLabels::mousePressEvent(QMouseEvent* event) {
    dragStartY_ = event->position().y();
    dragStartOffset_ = scrollOffset_;
    event->accept();
}

void LongVLabels::mouseMoveEvent(QMouseEvent* event) {
    if (dragStartY_) {
        scrollTo(dragStartOffset_ + static_cast<int>((*dragStartY_ - event->position().y()) / rowHeight()));
    }
    event->accept();
}

void LongVLabels::mouseReleaseEvent(QMouseEvent* event) {
    dragStartY_.reset();
    event->accept();
}

QString LongVLabels::labelColor(const QString& text) const {
    if (text == kAlmostDone || text == kMoreText) {
        return QStringLiteral("background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #9fc5d1 stop:1 #9fc5d1);");
    }
    if (isSongHeader(text, appState().cadenceMode)) {
        return kSongHeaderCss;
    }
    const auto targets = targetsFromText(text);
    if (!targets) {
        // free ride
        return zones::freeRideCss();
    }
    return zones::blockCss(targets->first, targets->second, ftpRanges);
}

void LongVLabels::setFullText(const QString& text) {
    fullText_ = text;
    // (a new list starts back at the top)
    scrollOffset_ = 0;
    emit textChanged(text);
}

void LongVLabels::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    renderTimer_->start();
}

}  // namespace awo

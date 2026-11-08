#include "LongVLabels.hpp"

#include <QLabel>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

#include "AppLayout.hpp"
#include "AppState.hpp"
#include "ScrollingLabel.hpp"
#include "Theme.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

const QString kMoreText = QStringLiteral("more...");
// an interval's other blocks are indented this much under its first (see workoutText)
const QString kIndent = QStringLiteral("      ");

}  // namespace

LongVLabels::LongVLabels(const ftpcolors::ZoneRanges& ranges, QWidget* parent)
    : QWidget(parent),
      ftpRanges(ranges),
      labelLayout_(new QVBoxLayout),
      renderTimer_(new QTimer(this)),
      slide_(new QVariantAnimation(this)) {
    // two layouts: one the widget's size, one holding the labels
    auto* main = new QVBoxLayout(this);
    // (the labels flush with the Up Next box)
    main->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < kWidgetCount; ++i) {
        auto* label = new ScrollingLabel;
        label->setFixedSize(kLabelWidth, 42);
        // (LayoutViewer lists them with the zone colored widgets)
        label->setProperty("awoFtpColored", true);
        labelLayout_->addWidget(label);
        label->hide();
        // (its cap follows it as the layout moves it)
        label->installEventFilter(this);
        labels_.append(label);
    }
    // an interval has two blocks at least: a cap for every two rows (over the rows, not in the layout)
    for (int i = 0; i < kWidgetCount / 2; ++i) {
        auto* cap = new QLabel(this);
        cap->setAlignment(Qt::AlignCenter);
        cap->setProperty("awoFtpColored", true);
        cap->setAttribute(Qt::WA_TransparentForMouseEvents);
        cap->hide();
        caps_.append(cap);
    }
    main->addLayout(labelLayout_);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(0);
    connect(renderTimer_, &QTimer::timeout, this, &LongVLabels::renderLines);
    connect(this, &LongVLabels::textChanged, renderTimer_, qOverload<>(&QTimer::start));
    // redrawn when the zone colors change
    connect(&zones::notifier(), &zones::Notifier::changed, this, &LongVLabels::renderLines);
    // a slide: its clock (ms), the rows placed by it (their caps follow them)
    slide_->setStartValue(0);
    connect(slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { placeSlide(value.toInt()); });
}

bool LongVLabels::slideRunning() const {
    return slide_->state() != QAbstractAnimation::Stopped;
}

void LongVLabels::holdSlide(std::optional<int> time) {
    slideHold_ = time;
    if (slide_->state() == QAbstractAnimation::Stopped) {
        return;
    }
    if (time) {
        slide_->pause();
        slide_->setCurrentTime(std::min(*time, slide_->duration()));
    } else {
        slide_->resume();
    }
}

void LongVLabels::finishSlide() {
    slide_->stop();
    for (qsizetype i = 0; i < slideHomes_.size() && i < labels_.size(); ++i) {
        labels_[i]->move(slideHomes_[i]);
    }
    slideHomes_.clear();
}

// the shift (in rows) that lines the rows up with the ones before: every pair that overlaps the same (the "more..."
// rows aside, which stay at the ends), and one at least; the smallest that does
int LongVLabels::rowShift(const QStringList& before, const QStringList& after, int direction) {
    if (direction == 0) {
        return 0;
    }
    const QStringList& from = direction > 0 ? before : after;
    const QStringList& to = direction > 0 ? after : before;
    for (qsizetype k = 1; k < from.size(); ++k) {
        bool matched = false;
        bool clash = false;
        for (qsizetype i = 0; i + k < from.size() && i < to.size(); ++i) {
            if (from[i + k] == kMoreText || to[i] == kMoreText) {
                continue;
            }
            if (from[i + k] != to[i]) {
                clash = true;
                break;
            }
            matched = true;
        }
        if (matched && !clash) {
            return static_cast<int>(direction > 0 ? k : -k);
        }
    }
    return 0;
}

void LongVLabels::startSlide(int rows, double step, const spiralslide::Motion& motion, int time) {
    slideHomes_.clear();
    for (QLabel* label : labels_) {
        if (label->isHidden()) {
            break;
        }
        slideHomes_.append(label->pos());
    }
    if (slideHomes_.isEmpty()) {
        return;
    }
    slideRows_ = rows;
    slideStep_ = step;
    slideMotion_ = motion;
    // (going up, the rows wait their turn after the ones that were above them, the one gone from the top first)
    const int duration = motion.duration(static_cast<int>(slideHomes_.size()) + std::max(rows, 0));
    slide_->setDuration(duration);
    slide_->setEndValue(duration);
    slide_->start();
    if (slideHold_) {
        slide_->pause();
        time = *slideHold_;
    }
    slide_->setCurrentTime(std::min(time, duration));
    placeSlide(slide_->currentValue().toInt());
}

void LongVLabels::placeSlide(int time) {
    const double scale = theme::widgetScale(this);
    for (qsizetype i = 0; i < slideHomes_.size() && i < labels_.size(); ++i) {
        const int wait = (static_cast<int>(i) + std::max(slideRows_, 0)) * slideMotion_.staggerMs;
        const spiralslide::Step step = spiralslide::at(slideMotion_, time - wait, scale);
        const double y = slideHomes_[i].y() + slideRows_ * slideStep_ * (1 - step.progress) + step.offset.y();
        labels_[i]->move(static_cast<int>(std::lround(slideHomes_[i].x() + step.offset.x())),
                         static_cast<int>(std::lround(y)));
    }
}

void LongVLabels::setPinnedStyle(int labelWidth) {
    setMinimumWidth(labelWidth);
    for (QLabel* label : labels_) {
        label->setFixedWidth(labelWidth);
        label->setAlignment(Qt::AlignCenter);
        static_cast<ScrollingLabel*>(label)->setScrolling(true);
    }
    // (the rows below the ones shown are empty: a block finishing takes the top one away, the rest each move up one)
    static_cast<QVBoxLayout*>(layout())->addStretch();
}

// a label's height and the layout's spacing, at the layout scale
double LongVLabels::rowHeight() const {
    return labels_.front()->minimumHeight() + std::max(labelLayout_->spacing(), 0);
}

int LongVLabels::rowGap() const {
    return std::max(labelLayout_->spacing(), 0);
}

// as many rows as fit, so it needs room for just one: the rows it shows mustn't keep it (and the screen) from getting
// smaller, e.g. on a smaller screen
QSize LongVLabels::minimumSizeHint() const {
    return {QWidget::minimumSizeHint().width(), static_cast<int>(std::ceil(rowHeight()))};
}

QString LongVLabels::capSheet(const QList<std::pair<QString, QString>>& blockColors) {
    // (rounded on the left, as the blocks are; square where it meets them)
    return zones::intervalCapCss(blockColors) +
           QStringLiteral("border-top-left-radius:%1px; border-bottom-left-radius:%1px; border-top-right-radius:0px; "
                          "border-bottom-right-radius:0px;")
               .arg(kCapRadius);
}

QString LongVLabels::capRoomCss() {
    // (square on the left, facing the cap's square side)
    return QStringLiteral("margin-left:%1px; border-top-left-radius:0px; border-bottom-left-radius:0px;")
        .arg(kCapWidth + kCapGap);
}

// the blocks and intervals in the text, in order
QList<LongVLabels::Item> LongVLabels::items() const {
    static const QRegularExpression repeats(QStringLiteral("^(\\d+)x (.+)$"));
    QList<Item> found;
    for (const QString& line : fullText_.split(u'\n')) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        if (line.startsWith(kIndent) && !found.isEmpty() && found.back().repeats > 0) {
            found.back().lines.append(line.trimmed());
            continue;
        }
        const QRegularExpressionMatch match = repeats.match(line);
        if (match.hasMatch()) {
            found.append({{match.captured(2)}, match.captured(1).toInt()});
        } else {
            found.append({{line}, 0});
        }
    }
    return found;
}

// the window of blocks and intervals from scrollOffset, with a "more..." block in the top or bottom slot whenever any
// are scrolled out of view above or below; the furthest scroll leaves the last one at the bottom, beneath a top
// "more..."
void LongVLabels::renderLines() {
    // (a slide under way: the rows back in their places before they're laid out again; it goes on from where it was
    // if they're the same rows, and ends where it was going if not)
    const bool wasSliding = slideRunning();
    const int slideTime = slide_->currentTime();
    finishSlide();
    const int direction = std::exchange(slideDirection_, 0);
    const spiralslide::Motion motion = slideMotion_;
    for (QLabel* label : labels_) {
        label->hide();
    }
    for (QLabel* cap : caps_) {
        cap->hide();
    }
    shownCaps_.clear();
    if (fullText_ == QLatin1String("DONE")) {
        shownRows_.clear();
        const QRect geo = geometry();
        setFixedSize(geo.width(), geo.height());
        return;
    }
    const int maxLines = std::min(static_cast<int>(geometry().height() / rowHeight()), kWidgetCount);
    const QList<Item> all = items();
    const auto count = static_cast<int>(all.size());
    const auto size = [&](int item) { return static_cast<int>(all[item].lines.size()); };
    int total = 0;
    for (int i = 0; i < count; ++i) {
        total += size(i);
    }
    // a row shown: its text, and the interval it's one of (-1: none)
    struct Row {
        QString text;
        int item = -1;
    };
    QList<Row> rows;
    // the whole blocks and intervals from first that fit in room rows (the first cut short if even it doesn't)
    const auto take = [&](int first, int room) {
        int used = 0;
        for (int i = first; i < count && used + size(i) <= room; ++i) {
            for (const QString& line : all[i].lines) {
                rows.append({line, all[i].repeats > 0 ? i : -1});
            }
            used += size(i);
        }
        if (used == 0 && first < count) {
            for (const QString& line : all[first].lines.mid(0, std::max(room, 0))) {
                rows.append({line, all[first].repeats > 0 ? first : -1});
            }
        }
    };
    if (total <= maxLines || maxLines < 3) {
        // all the text fits (or there's no room for "more..." blocks): no scrolling
        scrollOffset_ = 0;
        take(0, maxLines);
    } else {
        // the furthest scroll: as many from the end as fit under a top "more..."
        int furthest = count - 1;
        for (int i = count - 1, used = 0; i >= 1; --i) {
            used += size(i);
            if (used > maxLines - 1) {
                break;
            }
            furthest = i;
        }
        scrollOffset_ = std::clamp(scrollOffset_, 0, furthest);
        if (scrollOffset_ > 0) {
            rows.append({kMoreText});
        }
        const int room = maxLines - static_cast<int>(rows.size());
        int remaining = 0;
        for (int i = scrollOffset_; i < count; ++i) {
            remaining += size(i);
        }
        if (remaining > room) {
            take(scrollOffset_, room - 1);
            rows.append({kMoreText});
        } else {
            take(scrollOffset_, room);
        }
    }
    // an interval's rows make room on their left for its cap
    const QString capRoom = capRoomCss();
    for (qsizetype i = 0; i < rows.size() && i < labels_.size(); ++i) {
        theme::setSheet(labels_[i], labelColor(rows[i].text) + (rows[i].item >= 0 ? capRoom : QString()));
        layout::setText(labels_[i], rows[i].text);
        labels_[i]->show();
        if (rows[i].item >= 0) {
            if (!shownCaps_.isEmpty() && shownCaps_.back().item == rows[i].item && shownCaps_.back().last == i - 1) {
                shownCaps_.back().last = static_cast<int>(i);
            } else {
                shownCaps_.append({static_cast<int>(i), static_cast<int>(i), rows[i].item});
            }
        }
    }
    // each cap: the interval's repeat count, colored through its blocks' colors (the ones shown)
    for (qsizetype c = 0; c < shownCaps_.size() && c < caps_.size(); ++c) {
        const Cap& shown = shownCaps_[c];
        QList<std::pair<QString, QString>> colors;
        for (int row = shown.first; row <= shown.last; ++row) {
            colors.append(zones::lineColors(rows[row].text, ftpRanges));
        }
        QLabel* cap = caps_[c];
        theme::setSheet(cap, capSheet(colors));
        layout::setText(cap, QStringLiteral("%1x").arg(all[shown.item].repeats));
    }
    // (the layout puts the rows where they go now, so the caps can go over them)
    if (layout()) {
        layout()->activate();
    }
    placeCaps();
    // the rows glide from where they were shown before (when they're the same rows, moved)
    QStringList texts;
    for (const Row& row : rows) {
        texts.append(row.text);
    }
    const bool same = texts == shownRows_;
    const int shift = sliding_ && isVisible() && !same ? rowShift(shownRows_, texts, direction) : 0;
    shownRows_ = texts;
    if (shift != 0) {
        // (a row's height apart, as the layout has them)
        const double step = rows.size() > 1 ? labels_[1]->y() - labels_[0]->y() : rowHeight();
        startSlide(shift, step, motion);
    } else if (wasSliding && same && isVisible()) {
        startSlide(slideRows_, slideStep_, motion, slideTime);
    }
}

void LongVLabels::placeCaps() {
    const double scale = theme::widgetScale(this);
    const int width = std::max(1, static_cast<int>(std::lround(kCapWidth * scale)));
    for (qsizetype c = 0; c < caps_.size(); ++c) {
        QLabel* cap = caps_[c];
        if (c >= shownCaps_.size()) {
            cap->hide();
            continue;
        }
        const QRect first = labels_[shownCaps_[c].first]->geometry();
        const QRect last = labels_[shownCaps_[c].last]->geometry();
        cap->setGeometry(first.left(), first.top(), width, last.bottom() - first.top() + 1);
        cap->raise();
        cap->show();
    }
}

bool LongVLabels::eventFilter(QObject* watched, QEvent* event) {
    if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) && !shownCaps_.isEmpty()) {
        placeCaps();
    }
    return QWidget::eventFilter(watched, event);
}

void LongVLabels::scrollTo(int offset) {
    if (offset != scrollOffset_) {
        // (renderLines holds the bottom end)
        slideDirection_ = offset > scrollOffset_ ? 1 : -1;
        slideMotion_ = spiralslide::kScroll;
        scrollOffset_ = std::max(0, offset);
        renderLines();
    }
}

// a block or interval a wheel notch (120); touchpads send smaller steps that add up
void LongVLabels::wheelEvent(QWheelEvent* event) {
    wheelRemainder_ += event->angleDelta().y();
    const int steps = wheelRemainder_ / 120;
    if (steps) {
        wheelRemainder_ -= steps * 120;
        scrollTo(scrollOffset_ - steps);
    }
    event->accept();
}

// drag (mouse or touch) to scroll, a block or interval a row height dragged
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
    if (text == kMoreText) {
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
    // (the same list again stays still; a new one's rows move up, if they're the ones there were, a block done)
    if (text != fullText_) {
        slideDirection_ = 1;
        slideMotion_ = spiralslide::kBlockDone;
    }
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

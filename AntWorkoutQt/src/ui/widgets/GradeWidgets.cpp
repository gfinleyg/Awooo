#include "GradeWidgets.hpp"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "AppState.hpp"
#include "PowerSpiral.hpp"
#include "PyCompat.hpp"
#include "Theme.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

const QString kMoreText = QStringLiteral("more...");
const QString kPanelColor = QStringLiteral("#9fc5d1");
const QString kAveragePanelColor = QStringLiteral("#bdd6de");
// a song's label: the song header's colors in the workout lists (kSongHeaderCss)
const std::pair<QString, QString> kSongColors{QStringLiteral("#28275e"), QStringLiteral("#9fc5d1")};
// a ride's heading (the synopsis' session): darker, the same way round
const std::pair<QString, QString> kRideColors{QStringLiteral("#121230"), QStringLiteral("#4f7fa0")};

QColor themeColor(const QString& hex) {
    return QColor(theme::current().mapHex(hex));
}

// a font in the theme's family for Orbitron/Segoe UI Black, at the layout scale
QFont themeFont(const QString& family, double points, QFont::Weight weight = QFont::Black) {
    QFont font(family);
    font.setPointSizeF(points);
    font.setWeight(weight);
    return theme::current().mapFont(font, theme::uiScale());
}

// a block label's background colors (start, end), the same as its label everywhere else (see zones::blockCss): by
// the watts (or RPM) in its text in these zones (the app's by default), or the free ride colors
std::pair<QColor, QColor> labelGradientColors(const QString& text, const std::optional<ftpcolors::ZoneRanges>& ranges) {
    const auto targets = targetsFromText(text);
    const auto [start, end] = targets ? zones::blockColors(targets->first, targets->second, ranges)
                                      : zones::freeRideColors();
    return {themeColor(start), themeColor(end)};
}

// a grade's rounded, colored badge with its letter
void drawBadge(QPainter& painter, const QRectF& rect, const QString& grade, double points) {
    const double scale = theme::uiScale();
    const double radius = 12 * scale;
    painter.setPen(QPen(QColor(0, 0, 0, 140), std::max(1.0, 2 * scale)));
    painter.setBrush(themeColor(grading::gradeColor(grade)));
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), radius, radius);
    painter.setPen(themeColor(theme::textOn(grading::gradeColor(grade))));
    painter.setFont(fittedFont(themeFont(QStringLiteral("Orbitron"), points), grade, rect.width() * 0.86,
                               rect.height() * 0.9));
    painter.drawText(rect, Qt::AlignCenter, grade);
}

GradeList::Row toRow(const grading::GradeRow& row) {
    using Kind = GradeList::Row::Kind;
    GradeList::Row shown;
    switch (row.kind) {
        case grading::GradeRow::Kind::Block:
            shown.kind = Kind::Block;
            break;
        case grading::GradeRow::Kind::Interval:
            shown.kind = Kind::Interval;
            break;
        case grading::GradeRow::Kind::Sub:
            shown.kind = Kind::Sub;
            break;
        case grading::GradeRow::Kind::Song:
            shown.kind = Kind::Song;
            break;
    }
    shown.label = row.label;
    shown.pct = row.pct;
    shown.grade = row.grade;
    shown.note = row.note;
    shown.indent = row.indent;
    return shown;
}

}  // namespace

QFont fittedFont(const QFont& font, const QString& text, double width, double height) {
    // (what's drawn every frame fits its text once; a resize or new theme makes new ones, dropped now and then)
    static QHash<QString, QFont> fitted;
    const QString key = font.key() + u'\x1f' + text + u'\x1f' + QString::number(width, 'g', 17) + u'\x1f' +
                        QString::number(height, 'g', 17);
    const auto found = fitted.constFind(key);
    if (found != fitted.constEnd()) {
        return *found;
    }
    QFont shrunk(font);
    for (int i = 0; i < 40; ++i) {
        const QFontMetricsF metrics(shrunk);
        if ((metrics.horizontalAdvance(text) <= width && metrics.height() <= height) || shrunk.pointSizeF() <= 5) {
            break;
        }
        shrunk.setPointSizeF(shrunk.pointSizeF() * 0.92);
    }
    if (fitted.size() >= 256) {
        fitted.clear();
    }
    fitted.insert(key, shrunk);
    return shrunk;
}

// ---- GradeOverlay ----

GradeOverlay::GradeOverlay(PowerSpiral* spiral) : QWidget(spiral), spiral_(spiral), timer_(new QTimer(this)) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    timer_->setInterval(30);
    connect(timer_, &QTimer::timeout, this, &GradeOverlay::step);
    spiral->installEventFilter(this);
    setGeometry(spiral->rect());
    hide();
}

bool GradeOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == spiral_ && event->type() == QEvent::Resize) {
        setGeometry(spiral_->rect());
    }
    return false;
}

void GradeOverlay::showGrade(const QString& grade, std::optional<double> nextBlockSeconds) {
    showSeconds_ = nextBlockSeconds ? std::min(kMaxSeconds, *nextBlockSeconds) : kMaxSeconds;
    if (showSeconds_ <= 0) {
        return;
    }
    grade_ = grade;
    title_ = QStringLiteral("Grade");
    valueColor_.reset();
    shownAt_.start();
    opacity_ = 1.0;
    setGeometry(spiral_->rect());
    raise();
    show();
    timer_->start();
    update();
}

void GradeOverlay::showValue(const QString& title, const QString& value) {
    if (isVisible() && valueColor_ && title_ == title && grade_ == value) {
        return;
    }
    timer_->stop();
    title_ = title;
    grade_ = value;
    valueColor_ = QColor(Qt::white);
    opacity_ = 1.0;
    setGeometry(spiral_->rect());
    raise();
    show();
    update();
}

void GradeOverlay::clear() {
    timer_->stop();
    grade_.clear();
    hide();
}

void GradeOverlay::step() {
    stepAt(shownAt_.nsecsElapsed() / 1e9);
}

void GradeOverlay::stepAt(double elapsed) {
    const double fade = std::min(kMaxFadeSeconds, showSeconds_ / 2);
    if (elapsed >= showSeconds_) {
        clear();
        return;
    }
    opacity_ = elapsed < showSeconds_ - fade ? 1.0 : (showSeconds_ - elapsed) / fade;
    update();
}

std::pair<QPointF, double> GradeOverlay::innerCircle() const {
    const auto [center, radius] = spiral_->innerCircle();
    if (radius > 10) {
        return {center, radius};
    }
    const QRectF rect(this->rect());
    return {rect.center(), std::min(rect.width(), rect.height()) * 0.3};
}

void GradeOverlay::paintEvent(QPaintEvent*) {
    if (grade_.isEmpty()) {
        return;
    }
    const auto [center, radius] = innerCircle();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(opacity_);
    // "Grade" across the top of the circle, the letter as big as fits below it
    const QRectF wordRect(center.x() - radius * 0.8, center.y() - radius * 0.78, radius * 1.6, radius * 0.42);
    const QRectF letterRect(center.x() - radius * 0.85, center.y() - radius * 0.36, radius * 1.7, radius * 1.05);
    drawOutlined(painter, title_, wordRect, QColor(Qt::white), 26);
    const QColor fill = valueColor_.value_or(themeColor(grading::gradeColor(grade_)));
    drawOutlined(painter, grade_, letterRect, fill, 110);
}

// (it repaints with the spiral under it, every frame: the text's shape is only worked out again when the text, its
// room or the theme's font changes)
void GradeOverlay::drawOutlined(QPainter& painter, const QString& text, const QRectF& rect, const QColor& fill,
                                double points) {
    const QString family = theme::current().fonts.value(theme::kRoleMain).family;
    const auto key = std::make_tuple(text, rect.x(), rect.y(), rect.width(), rect.height(), points, family);
    auto found = shapes_.find(key);
    if (found == shapes_.end()) {
        if (shapes_.size() >= 16) {
            shapes_.clear();
        }
        found = shapes_.emplace(key, textShape(text, rect, points, family)).first;
    }
    const auto& [path, outline] = found->second;
    painter.setPen(QPen(QColor(0, 0, 0, 220), outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawPath(path);
}

// (path, outline width) of text centered in rect, as big as fits up to points
std::pair<QPainterPath, double> GradeOverlay::textShape(const QString& text, const QRectF& rect, double points,
                                                        const QString& family) {
    QFont font(family);
    font.setWeight(QFont::Black);
    font.setPointSizeF(points);
    font = fittedFont(font, text, rect.width(), rect.height());
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(text);
    // the baseline so the text's capitals sit centered in rect
    const double baseline = rect.center().y() + metrics.capHeight() / 2;
    QPainterPath path;
    path.addText(QPointF(rect.center().x() - width / 2, baseline), font, text);
    // (a "+" is two overlapping strokes: no hole where they cross)
    path.setFillRule(Qt::WindingFill);
    return {path, std::max(1.5, font.pointSizeF() * 0.06)};
}

// ---- GradeList ----

GradeList::GradeList(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(200);
    connect(&zones::notifier(), &zones::Notifier::changed, this, qOverload<>(&QWidget::update));
    connect(&theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void GradeList::refresh() {
    refresh(appState().blockGrades);
}

void GradeList::refresh(const grading::Grades& grades) {
    scrollOffset_ = 0;
    const AppState& state = appState();
    // (Musical Cadence Mode's song headers are "<length> : <Artist> - <Title>")
    QMap<int, QString> songs;
    for (auto it = state.musicSongStarts.begin(); it != state.musicSongStarts.end(); ++it) {
        if (isSongHeader(it.value(), state.cadenceMode)) {
            const qsizetype at = it.value().indexOf(QStringLiteral(" : "));
            songs.insert(it.key(), at >= 0 ? it.value().mid(at + 3) : QString());
        }
    }
    rows_.clear();
    for (const grading::GradeRow& row : grading::workoutRows(state.workoutList, grades, songs)) {
        rows_.append(toRow(row));
    }
    average_ = grading::workoutAverage(grades);
    averageText_ = QStringLiteral("Workout Average");
    update();
}

void GradeList::refresh(const QList<const Ride*>& rides) {
    scrollOffset_ = 0;
    rows_.clear();
    QList<const grading::Grade*> records;
    int graded = 0;
    for (const Ride* ride : rides) {
        const std::optional<double> average = session::average(*ride);
        Row heading;
        heading.kind = Row::Kind::Ride;
        heading.label = session::heading(*ride);
        heading.pct = average;
        heading.grade = grading::letter(average);
        heading.note = session::note(*ride);
        heading.graded = ride->graded;
        rows_.append(heading);
        if (ride->graded) {
            ++graded;
            for (const grading::GradeRow& row : grading::workoutRows(ride->workoutList, ride->blockGrades,
                                                                      ride->songHeaders)) {
                Row shown = toRow(row);
                shown.ranges = ride->targetRanges;
                rows_.append(shown);
            }
            for (const auto& [key, grade] : ride->blockGrades.items()) {
                records.append(&grade);
            }
        }
    }
    average_ = grading::riddenAverage(records);
    averageText_ = graded > 1 ? QStringLiteral("Session Average") : QStringLiteral("Workout Average");
    update();
}

void GradeList::setRows(const QList<Row>& rows, std::optional<double> average, const QString& averageText) {
    scrollOffset_ = 0;
    rows_ = rows;
    average_ = average;
    averageText_ = averageText;
    update();
}

double GradeList::scaled(double value) {
    return value * theme::uiScale();
}

double GradeList::rowStep() const {
    return scaled(kRowHeight + kRowGap);
}

int GradeList::rowsPerColumn(double height) const {
    const double listHeight = height - scaled(kAverageHeight + 2 * kRowGap);
    return std::max(1, static_cast<int>((listHeight + scaled(kRowGap)) / rowStep()));
}

int GradeList::columnsFit(double width) {
    return std::max(1, static_cast<int>((width + scaled(kColumnGap)) / scaled(kMinColumnWidth + kColumnGap)));
}

void GradeList::setColumnFlow(bool flow) {
    columnFlow_ = flow;
    update();
}

int GradeList::columns() const {
    if (!columnFlow_) {
        return 1;
    }
    const qsizetype perColumn = rowsPerColumn(height());
    const qsizetype needed = std::max<qsizetype>(1, (rows_.size() + perColumn - 1) / perColumn);
    return static_cast<int>(std::min<qsizetype>(columnsFit(width()), needed));
}

QSize GradeList::fittedSize(const QSize& most) const {
    const qsizetype perColumn = rowsPerColumn(most.height());
    const qsizetype count = std::max<qsizetype>(1, rows_.size());
    const qsizetype columns = std::min<qsizetype>(columnsFit(most.width()), (count + perColumn - 1) / perColumn);
    const double width = columns * scaled(kColumnWidth) + (columns - 1) * scaled(kColumnGap);
    const double height = std::min(count, perColumn) * rowStep() - scaled(kRowGap) +
                          scaled(kAverageHeight + 2 * kRowGap);
    // (a little over, so the rows still fit however the scale rounds)
    return {std::min(most.width(), static_cast<int>(std::ceil(width))),
            std::min(most.height(), static_cast<int>(std::ceil(height + 0.5)))};
}

QList<GradeList::Row> GradeList::shownRows() {
    const int maxRows = rowsPerColumn(height()) * columns();
    if (rows_.size() <= maxRows || maxRows < 3) {
        scrollOffset_ = 0;
        return rows_.mid(0, maxRows);
    }
    // the furthest scroll leaves the last row in the bottom slot, beneath a top "more..."
    scrollOffset_ = std::max(0, std::min(scrollOffset_, static_cast<int>(rows_.size()) - maxRows + 1));
    Row more;
    more.kind = Row::Kind::More;
    QList<Row> shown;
    if (scrollOffset_ > 0) {
        shown.append(more);
    }
    const qsizetype available = maxRows - shown.size();
    const QList<Row> remaining = rows_.mid(scrollOffset_);
    if (remaining.size() > available) {
        shown.append(remaining.mid(0, available - 1));
        shown.append(more);
    } else {
        shown.append(remaining);
    }
    return shown;
}

void GradeList::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // the rows down each column in turn, the columns sharing the width
    const int columns = this->columns();
    const int perColumn = rowsPerColumn(height());
    const double columnGap = scaled(kColumnGap);
    const double width = (this->width() - (columns - 1) * columnGap) / columns;
    const double rowHeight = scaled(kRowHeight);
    const double badgeWidth = scaled(kBadgeWidth);
    const double gap = scaled(6);
    const QFont labelFont = themeFont(QStringLiteral("Orbitron"), kLabelPoints);
    const QList<Row> shown = shownRows();
    for (qsizetype i = 0; i < shown.size(); ++i) {
        const Row& row = shown[i];
        const double y = (i % perColumn) * rowStep();
        painter.save();
        painter.translate((i / perColumn) * (width + columnGap), 0);
        const double labelLeft = badgeWidth + gap + (row.kind == Row::Kind::Sub || row.indent ? scaled(kIndent) : 0);
        const QRectF labelRect(labelLeft, y, width - labelLeft, rowHeight);
        switch (row.kind) {
            case Row::Kind::More:
                drawLabel(painter, QRectF(badgeWidth + gap, y, width - badgeWidth - gap, rowHeight), kMoreText,
                          labelFont, themeColor(kPanelColor), themeColor(kPanelColor));
                break;
            case Row::Kind::Interval:
                painter.setPen(QColor(Qt::white));
                painter.setFont(themeFont(QStringLiteral("Orbitron"), 20));
                painter.drawText(labelRect.adjusted(scaled(8), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, row.label);
                break;
            case Row::Kind::Ride: {
                // (one not graded has no badge: its heading takes the badge's room too)
                if (row.graded) {
                    drawBadge(painter, QRectF(0, y, badgeWidth, rowHeight), row.grade, 17);
                }
                QRectF rect(row.graded ? badgeWidth + gap : 0, y, 0, rowHeight);
                rect.setRight(width);
                drawLabel(painter, rect, row.label, labelFont, themeColor(kRideColors.first),
                          themeColor(kRideColors.second), row.note, QColor(Qt::white));
                break;
            }
            case Row::Kind::Song:
                drawBadge(painter, QRectF(0, y, badgeWidth, rowHeight), row.grade, 17);
                drawLabel(painter, labelRect, QStringLiteral("♪ ") + row.label, labelFont,
                          themeColor(kSongColors.first), themeColor(kSongColors.second), {},
                          themeColor(theme::kLightText));
                break;
            default: {
                drawBadge(painter, QRectF(0, y, badgeWidth, rowHeight), row.grade, 17);
                const auto [start, end] = labelGradientColors(row.label, row.ranges);
                drawLabel(painter, labelRect, row.label, labelFont, start, end, row.note);
                break;
            }
        }
        painter.restore();
    }
    drawAverage(painter);
}

// a block label: rounded, the zone gradient behind its text, a note at the right (each rep's grade, ...)
void GradeList::drawLabel(QPainter& painter, const QRectF& rect, const QString& text, const QFont& font,
                          const QColor& start, const QColor& end, const QString& note,
                          const std::optional<QColor>& textColor) {
    QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
    gradient.setColorAt(0, start);
    gradient.setColorAt(1, end);
    const double radius = scaled(15);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(gradient));
    painter.drawRoundedRect(rect, radius, radius);
    QRectF inner = rect.adjusted(scaled(14), 0, -scaled(10), 0);
    painter.setPen(textColor.value_or(QColor(zones::blockText(start.name(), end.name()))));
    if (!note.isEmpty()) {
        const QFont noteFont = themeFont(QStringLiteral("Orbitron"), 10);
        const QFontMetricsF noteMetrics(noteFont);
        const double noteWidth = std::min(noteMetrics.horizontalAdvance(note), inner.width() * 0.45);
        painter.setFont(noteFont);
        painter.drawText(inner, Qt::AlignVCenter | Qt::AlignRight, noteMetrics.elidedText(note, Qt::ElideRight, noteWidth));
        inner.setRight(inner.right() - noteWidth - scaled(8));
    }
    painter.setFont(font);
    painter.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft,
                     QFontMetricsF(font).elidedText(text, Qt::ElideRight, inner.width()));
}

void GradeList::drawAverage(QPainter& painter) {
    const double height = scaled(kAverageHeight);
    const double top = this->height() - height;
    // a light rule above the average
    painter.setPen(QPen(QColor(255, 255, 255, 150), std::max(1.0, scaled(3))));
    painter.drawLine(QPointF(0, top - scaled(kRowGap)), QPointF(width(), top - scaled(kRowGap)));
    const double badgeWidth = scaled(kBadgeWidth);
    drawBadge(painter, QRectF(0, top, badgeWidth, height), grading::letter(average_), 24);
    const QRectF rect(badgeWidth + scaled(6), top, width() - badgeWidth - scaled(6), height);
    const double radius = scaled(15);
    painter.setPen(Qt::NoPen);
    painter.setBrush(themeColor(kAveragePanelColor));
    painter.drawRoundedRect(rect, radius, radius);
    QRectF inner = rect.adjusted(scaled(14), 0, -scaled(14), 0);
    const QFont font = themeFont(QStringLiteral("Orbitron"), 16);
    painter.setFont(font);
    painter.setPen(themeColor(QStringLiteral("#000000")));
    const QString pctText = average_ ? py::fixed(*average_, 1) + u'%' : QString();
    painter.drawText(inner, Qt::AlignVCenter | Qt::AlignRight, pctText);
    inner.setRight(inner.right() - QFontMetricsF(font).horizontalAdvance(pctText) - scaled(10));
    painter.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft,
                     QFontMetricsF(font).elidedText(averageText_, Qt::ElideRight, inner.width()));
}

// scrolling, the same as the workout screen's block lists
void GradeList::scrollTo(int offset) {
    if (offset != scrollOffset_) {
        // (shownRows holds the bottom end)
        scrollOffset_ = std::max(0, offset);
        update();
    }
}

void GradeList::wheelEvent(QWheelEvent* event) {
    wheelRemainder_ += event->angleDelta().y();
    const int steps = wheelRemainder_ / 120;
    if (steps) {
        wheelRemainder_ -= steps * 120;
        scrollTo(scrollOffset_ - steps);
    }
    event->accept();
}

void GradeList::mousePressEvent(QMouseEvent* event) {
    dragStartY_ = event->position().y();
    dragStartOffset_ = scrollOffset_;
    event->accept();
}

void GradeList::mouseMoveEvent(QMouseEvent* event) {
    if (dragStartY_) {
        scrollTo(dragStartOffset_ + static_cast<int>((*dragStartY_ - event->position().y()) / rowStep()));
    }
    event->accept();
}

void GradeList::mouseReleaseEvent(QMouseEvent* event) {
    dragStartY_.reset();
    event->accept();
}

}  // namespace awo

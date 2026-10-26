#include "PowerSpiral.hpp"

#include <QConicalGradient>
#include <QPainter>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>

#include "PyCompat.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"

namespace awo {

using namespace spiral;

namespace {

constexpr double kPi = std::numbers::pi;

// the ring's and the spiral's opacity while blinking
std::pair<double, double> blinkOpacity(PowerSpiral::Blink blink) {
    switch (blink) {
        case PowerSpiral::Blink::Block:
            return {1.0, 0.13};
        case PowerSpiral::Blink::Workout:
            return {0.13, 0.53};
        default:
            return {1.0, 1.0};
    }
}

}  // namespace

namespace spiral {

const QString kTrackColor = QStringLiteral("#000000");
const QString kLineColor = QStringLiteral("#1E2230");

double cadenceShare(std::optional<double> rpm) {
    if (!rpm) {
        return 1.0;
    }
    return std::min(1.0, std::max(0.0, (*rpm - kCadenceMinRpm) / (kCadenceMaxRpm - kCadenceMinRpm)));
}

// ---- Timeline ----

Timeline::Timeline(double duration, int bins) : bins_(bins), duration_(std::max(duration, 1e-6)) {
    // (which one it is, for what's drawn of it: a new block's isn't the last's)
    static std::atomic<int> serials{0};
    serial_ = serials++;
    sums_.resize(bins);
}

double Timeline::fraction() const {
    return std::min(1.0, std::max(0.0, seconds / duration_));
}

void Timeline::setDuration(double duration) {
    duration = std::max(duration, 1e-6);
    if (duration == duration_) {
        return;
    }
    duration_ = duration;
    sums_ = QList<Sums>(bins_);
    for (const RingSample& sample : samples_) {
        bin(sample);
    }
    ++version_;
}

void Timeline::add(double at, double power, std::optional<double> cadence) {
    const RingSample sample{std::min(std::max(at, 0.0), duration_), power, cadence};
    samples_.append(sample);
    bin(sample);
    ++version_;
}

void Timeline::bin(const RingSample& sample) {
    Sums& sums = sums_[std::min(bins_ - 1, static_cast<int>(sample.seconds / duration_ * bins_))];
    sums.power += sample.power;
    sums.count += 1;
    if (sample.cadence) {
        sums.cadence += *sample.cadence;
        sums.cadenceCount += 1;
    }
}

const QList<BinValue>& Timeline::values() const {
    if (values_ && values_->first == version_) {
        return values_->second;
    }
    QList<BinValue> values;
    BinValue last;
    for (const Sums& sums : sums_) {
        if (sums.count) {
            last = {sums.power / sums.count,
                    sums.cadenceCount ? std::optional(sums.cadence / sums.cadenceCount) : std::nullopt};
            if (!values.isEmpty() && !values.front().power) {
                values.fill(last);
            }
        }
        values.append(last);
    }
    values_ = std::pair(version_, values);
    return values_->second;
}

}  // namespace spiral

// ---- PowerSpiral ----

PowerSpiral::PowerSpiral(QWidget* parent) : QWidget(parent) {
    setMinimumSize(QSize(60, 60));
    clear();
    // (drawn in the theme's text color, and the zone colors)
    connect(&theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
    connect(&zones::notifier(), &zones::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void PowerSpiral::requestUpdate() {
    ++updatesRequested_;
    update();
}

void PowerSpiral::clear(double workoutDuration, const QList<session::RingHistory>& history) {
    historySeconds_ = 0.0;
    historyRanges_.clear();
    QList<RingSample> samples;
    for (const session::RingHistory& ride : history) {
        const double start = historySeconds_;
        for (const RingSample& sample : ride.samples) {
            samples.append({start + std::min(std::max(sample.seconds, 0.0), ride.seconds), sample.power, sample.cadence});
        }
        historyRanges_.append({start, start + ride.seconds, ride.powerRanges});
        historySeconds_ += ride.seconds;
    }
    workout_ = Timeline(historySeconds_ + workoutDuration);
    for (const RingSample& sample : samples) {
        workout_.add(sample.seconds, sample.power, sample.cadence);
    }
    historyCount_ = samples.size();
    workout_.seconds = historySeconds_;
    block_ = Timeline(1.0, kBins * kBlockTurns);
    blink_ = Blink::Default;
    requestUpdate();
}

Ring PowerSpiral::ridden() const {
    QList<RingSample> samples;
    for (qsizetype i = historyCount_; i < workout_.samples().size(); ++i) {
        const RingSample& sample = workout_.samples()[i];
        samples.append({sample.seconds - historySeconds_, sample.power, sample.cadence});
    }
    return {std::max(0.0, workout_.seconds - historySeconds_), session::perSecond(samples)};
}

void PowerSpiral::setBlock(double duration) {
    block_ = Timeline(duration, kBins * kBlockTurns);
    requestUpdate();
}

void PowerSpiral::setProgress(double workoutSeconds, double workoutDuration, double blockSeconds,
                              double blockDuration) {
    workout_.setDuration(historySeconds_ + workoutDuration);
    workout_.seconds = historySeconds_ + workoutSeconds;
    block_.setDuration(blockDuration);
    block_.seconds = blockSeconds;
    // redrawn only once an end has moved enough to see (the workout screen sets this every frame)
    if (ends(geometryNow()) != drawnEnds_) {
        requestUpdate();
    }
}

void PowerSpiral::addSample(double workoutSeconds, double blockSeconds, double power, std::optional<double> cadence) {
    workout_.add(historySeconds_ + workoutSeconds, power, cadence);
    block_.add(blockSeconds, power, cadence);
}

void PowerSpiral::setBlink(Blink blink) {
    if (blink != blink_) {
        blink_ = blink;
        requestUpdate();
    }
}

PowerSpiral::Geometry PowerSpiral::geometryNow() const {
    const double size = std::min(width(), height());
    const double rMax = size / 2;
    const double rInner = rMax * kHole;
    const double pitch = std::max(1.0, (rMax - rInner + kSpacePx) / (kBlockTurns + 2));
    const double lane = std::max(0.0, pitch - kSpacePx);
    const double ringR = rMax - lane / 2;
    return {QPointF(width() / 2.0, height() / 2.0), rInner, pitch, lane, ringR, ringR - pitch, this->size()};
}

std::pair<QPointF, double> PowerSpiral::innerCircle() const {
    const Geometry g = geometryNow();
    return {g.center, g.rInner - kSpacePx};
}

std::function<double(double)> PowerSpiral::spiralRadius(const Geometry& g) const {
    return [spiralR = g.spiralR, pitch = g.pitch](double theta) { return spiralR - pitch * theta / (2 * kPi); };
}

// how far along (px, in kRedrawPx steps) the ring's and the spiral's ends are: drawn again once they move
std::pair<long long, long long> PowerSpiral::ends(const Geometry& g) const {
    return {static_cast<long long>(py::round(workout_.fraction() * 2 * kPi * g.ringR / kRedrawPx)),
            static_cast<long long>(py::round(block_.fraction() * kBlockTurns * 2 * kPi * g.spiralR / kRedrawPx))};
}

// the line's outline from angle start to end (radians clockwise from North, 2 pi a turn), its middle radiusAt(angle)
// from the center, width px wide, with a round end at either end
QPainterPath PowerSpiral::ribbon(const Geometry& g, const RadiusAt& radiusAt, double start, double end,
                                 double width) const {
    const QPointF center = g.center;
    auto at = [&](double theta, double offset) {
        const double r = radiusAt(theta) + offset;
        return QPointF(center.x() + r * std::sin(theta), center.y() - r * std::cos(theta));
    };
    // half a circle round the end: from one edge to the other through the way the line goes (or came)
    auto cap = [&](double theta, double w, bool forward) {
        const QPointF middle = at(theta, 0);
        const QPointF out(std::sin(theta), -std::cos(theta));
        const QPointF along = QPointF(std::cos(theta), std::sin(theta)) * (forward ? 1 : -1);
        const double sign = forward ? 1 : -1;
        QList<QPointF> points;
        for (int k = 1; k < 12; ++k) {
            points.append(middle + (out * std::cos(kPi * k / 12) * sign + along * std::sin(kPi * k / 12)) * (w / 2));
        }
        return points;
    };
    const int steps = std::max(2, static_cast<int>((end - start) / (kPi / 180)));
    QList<double> angles;
    for (int k = 0; k <= steps; ++k) {
        angles.append(start + (end - start) * k / steps);
    }
    QList<QPointF> points;
    for (const double t : angles) {
        points.append(at(t, width / 2));
    }
    if (width > 0.5) {
        points.append(cap(end, width, true));
    }
    for (auto it = angles.rbegin(); it != angles.rend(); ++it) {
        points.append(at(*it, -width / 2));
    }
    if (width > 0.5) {
        points.append(cap(start, width, false));
    }
    QPainterPath path;
    path.addPolygon(QPolygonF(points));
    path.closeSubpath();
    return path;
}

// the whole ring, its end meeting its start at North: its outer edge all the way round and its inner edge, filled
// between them
QPainterPath PowerSpiral::fullRing(const Geometry& g, double width) const {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const int side : {1, -1}) {
        QPolygonF polygon;
        for (int k = 0; k < 360; ++k) {
            const double t = 2 * kPi * k / 360;
            const double r = g.ringR + side * width / 2;
            polygon.append(QPointF(g.center.x() + r * std::sin(t), g.center.y() - r * std::cos(t)));
        }
        path.addPolygon(polygon);
        path.closeSubpath();
    }
    return path;
}

// heightOf(bin): how far across the lane the timeline's bin's line is (none: nothing ridden there yet)
PowerSpiral::HeightOf PowerSpiral::heights(const Geometry& g, const Timeline& timeline) const {
    const QList<BinValue>& values = timeline.values();
    const double lane = g.lane;
    const int last = timeline.bins() - 1;
    return [&values, lane, last](int j) -> std::optional<double> {
        const BinValue& value = values[std::min(j, last)];
        if (!value.power) {
            return std::nullopt;
        }
        return lane * cadenceShare(value.cadence);
    };
}

// (the shape under the line, the line) from angle start to end: a point at each bin's middle and at either end,
// heightOf(bin) px out from the lane's inner edge, the lane's middle radiusAt(angle) from the center
std::pair<QPainterPath, QPainterPath> PowerSpiral::plot(const Geometry& g, const RadiusAt& radiusAt, double start,
                                                        double end, const HeightOf& heightOf) const {
    const QPointF center = g.center;
    const double half = g.lane / 2;
    auto at = [&](double theta, double offset) {
        const double r = radiusAt(theta) + offset;
        return QPointF(center.x() + r * std::sin(theta), center.y() - r * std::cos(theta));
    };
    const double step = 2 * kPi / kBins;
    QList<double> angles{start};
    QList<int> bins{static_cast<int>(start / step + 1e-9)};
    for (int j = static_cast<int>(start / step); j <= static_cast<int>(end / step); ++j) {
        const double t = (j + 0.5) * step;
        if (start + 1e-9 < t && t < end - 1e-9) {
            angles.append(t);
            bins.append(static_cast<int>(t / step));
        }
    }
    angles.append(end);
    bins.append(std::max(0, static_cast<int>(end / step - 1e-9)));
    QPainterPath area, line;
    QList<double> heights;
    for (const int j : bins) {
        const std::optional<double> h = heightOf(j);
        if (!h) {
            return {area, line};
        }
        heights.append(*h);
    }
    if (end <= start) {
        return {area, line};
    }
    QPolygonF top;
    for (qsizetype i = 0; i < angles.size(); ++i) {
        top.append(at(angles[i], heights[i] - half));
    }
    QPolygonF outline = top;
    for (auto it = angles.rbegin(); it != angles.rend(); ++it) {
        outline.append(at(*it, -half));
    }
    area.addPolygon(outline);
    area.closeSubpath();
    line.addPolygon(top);
    return {area, line};
}

// rangesOf(bin): the power ranges a bin's colored by, the ride's it's in (the session's earlier rides keep the FTP
// they were ridden at), else ranges
std::function<ftpcolors::ZoneRanges(int)> PowerSpiral::binRanges(const Timeline& timeline,
                                                                 const ftpcolors::ZoneRanges& ranges) const {
    if (&timeline != &workout_ || historyRanges_.isEmpty()) {
        return [ranges](int) { return ranges; };
    }
    const double step = timeline.duration() / timeline.bins();
    return [this, step, ranges](int j) {
        const double at = (j + 0.5) * step;
        for (const HistoryRange& ride : historyRanges_) {
            if (at < ride.end) {
                return ride.ranges;
            }
        }
        return ranges;
    };
}

// each of the timeline's bins' power's zone color, worked out again only for the bins whose power changed (a new
// sample changes a few; there are kBins a turn)
const QList<QColor>& PowerSpiral::binColors(const Timeline& timeline, const ftpcolors::ZoneRanges& ranges,
                                            const ColorsKey& colorsKey) {
    // (a new length moves the bins along the session's rides, each with its own ranges)
    const BinColorsKey key{timeline.serial(), colorsKey, timeline.duration()};
    const auto found =
        std::find_if(binColors_.begin(), binColors_.end(), [&key](const auto& item) { return item.first == key; });
    if (found != binColors_.end() && found->second.version == timeline.version()) {
        return found->second.colors;
    }
    BinColorsEntry entry = found != binColors_.end() ? found->second : BinColorsEntry{-1, {}, {}};
    std::optional<ftpcolors::Palette> fitted;
    const auto rangesOf = binRanges(timeline, ranges);
    const QList<BinValue>& values = timeline.values();
    for (qsizetype j = 0; j < values.size(); ++j) {
        const double power = values[j].power.value_or(0);
        if (j < entry.powers.size() && entry.powers[j] == power) {
            continue;
        }
        if (!fitted) {
            fitted = zones::palette();
        }
        const QColor color(ftpcolors::zoneHex(power, rangesOf(static_cast<int>(j)), *fitted));
        if (j < entry.powers.size()) {
            entry.powers[j] = power;
            entry.colors[j] = color;
        } else {
            entry.powers.append(power);
            entry.colors.append(color);
        }
    }
    entry.version = timeline.version();
    // (only the ring's and the spiral's kept)
    binColors_.removeIf([this, &key](const auto& item) {
        const BinColorsKey& k = item.first;
        const bool current = (k.serial == workout_.serial() && k.duration == workout_.duration()) ||
                             (k.serial == block_.serial() && k.duration == block_.duration());
        return !current || k == key;
    });
    binColors_.append({key, entry});
    return binColors_.back().second.colors;
}

// a turn's colors round the circle, each bin its power's zone color, from the timeline's bins from firstBin on, the
// turn's bins fromBin to toBin (Qt's conical gradient runs anticlockwise from North, so a point a fraction f of the
// way round clockwise is at 1 - f; the colors change where one bin meets the next, and carry on past either end)
QBrush PowerSpiral::powerBrush(const Geometry& g, const Timeline& timeline, int firstBin, int fromBin, int toBin,
                               const ftpcolors::ZoneRanges& ranges, const ColorsKey& colorsKey) {
    const QList<QColor>& colors = binColors(timeline, ranges, colorsKey);
    QConicalGradient gradient(g.center, 90);
    constexpr double edge = 0.05 / kBins;
    QGradientStops stops;
    // (lowest first: the last bin's first)
    for (int j = std::max(fromBin + 1, std::min(kBins, toBin)) - 1; j >= std::max(0, fromBin); --j) {
        const QColor& color = colors[std::min(firstBin + j, timeline.bins() - 1)];
        stops.append({1 - (j + 1.0) / kBins + edge, color});
        stops.append({1 - static_cast<double>(j) / kBins - edge, color});
    }
    gradient.setStops(stops);
    return QBrush(gradient);
}

// the bins on a turn fraction of the way round that are drawn and kept: whole kChunkBins, all of them at its end
int PowerSpiral::doneBins(double fraction) {
    if (fraction >= 1) {
        return kBins;
    }
    return static_cast<int>(fraction * kBins / kChunkBins + 1e-9) * kChunkBins;
}

double PowerSpiral::turnFraction(int k) const {
    return std::min(1.0, std::max(0.0, block_.fraction() * kBlockTurns - k));
}

// the timeline's plot on its turn (the ring's one, or one of the spiral's), fraction of the way round it: the finished
// stretch (done: up to the middle of its last bin, only worked out again once there's another kChunkBins) and the
// rest (tip: from a bin before that, so the two overlap and no seam shows), each with its own bins' colors
void PowerSpiral::paintRidden(QPainter& painter, int name, const Geometry& g, const Timeline& timeline,
                              const RadiusAt& radiusAt, int turn, double fraction, const ColorsKey& colorsKey,
                              const ftpcolors::ZoneRanges& ranges, const QPen& linePen, bool done, bool tip) {
    if (fraction <= 0) {
        return;
    }
    const double step = 2 * kPi / kBins;
    const double start = 2 * kPi * turn;
    const double end = 2 * kPi * (turn + fraction);
    const int doneCount = doneBins(fraction);
    const double doneEnd =
        doneCount >= kBins ? end : (doneCount ? std::min(end, start + (doneCount - 0.5) * step) : start);
    const HeightOf heightOf = heights(g, timeline);
    QList<Piece> pieces;
    if (done && doneEnd > start) {
        const DoneKey key{g.key, timeline.serial(), timeline.duration(), turn, doneCount, colorsKey};
        auto found = donePieces_.find(name);
        if (found == donePieces_.end() || !(found->second.first == key)) {
            auto [area, line] = plot(g, radiusAt, start, doneEnd, heightOf);
            Piece piece{powerBrush(g, timeline, turn * kBins, 0, doneCount, ranges, colorsKey), area, line};
            found = donePieces_.insert_or_assign(name, std::pair(key, piece)).first;
        }
        pieces.append(found->second.second);
    }
    if (tip && doneEnd < end) {
        auto [area, line] = plot(g, radiusAt, std::max(start, doneEnd - step), end, heightOf);
        pieces.append({powerBrush(g, timeline, turn * kBins, doneCount - 2,
                                  static_cast<int>(std::ceil(fraction * kBins)), ranges, colorsKey),
                       area, line});
    }
    for (const Piece& piece : pieces) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(piece.brush);
        painter.drawPath(piece.area);
        painter.setPen(linePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(piece.line);
    }
    painter.setPen(Qt::NoPen);
}

double PowerSpiral::ringOpacity() const {
    return blinkOpacity(blink_).first;
}

double PowerSpiral::spiralOpacity() const {
    return blinkOpacity(blink_).second;
}

void PowerSpiral::paintRing(QPainter& painter, const Geometry& g, const ColorsKey& colorsKey,
                            const ftpcolors::ZoneRanges& ranges, const QPen& linePen, bool done, bool tip) {
    // (all the way round at the end: the end meets the start)
    painter.setOpacity(ringOpacity());
    const double ringR = g.ringR;
    paintRidden(painter, -1, g, workout_, [ringR](double) { return ringR; }, 0, workout_.fraction(), colorsKey, ranges,
                linePen, done, tip);
}

// a tick across the ring where each of the session's earlier rides ends
void PowerSpiral::paintRideTicks(QPainter& painter, const Geometry& g, const QColor& lineColor) {
    if (historyRanges_.isEmpty()) {
        return;
    }
    painter.setOpacity(ringOpacity());
    QPen pen(lineColor, kTickPx);
    pen.setCapStyle(Qt::FlatCap);
    painter.setPen(pen);
    const double inner = g.ringR - g.lane / 2;
    const double outer = g.ringR + g.lane / 2;
    for (const HistoryRange& ride : historyRanges_) {
        const double theta = 2 * kPi * ride.end / workout_.duration();
        const QPointF out(std::sin(theta), -std::cos(theta));
        painter.drawLine(g.center + out * inner, g.center + out * outer);
    }
    painter.setPen(Qt::NoPen);
}

// the spiral's turn k, as far as the block's got along it
void PowerSpiral::paintTurn(QPainter& painter, const Geometry& g, int k, const ColorsKey& colorsKey,
                            const ftpcolors::ZoneRanges& ranges, const QPen& linePen, bool done, bool tip) {
    painter.setOpacity(spiralOpacity());
    paintRidden(painter, k, g, block_, spiralRadius(g), k, turnFraction(k), colorsKey, ranges, linePen, done, tip);
}

// a see-through picture the widget's size to draw into
QPixmap PowerSpiral::layer(double ratio) const {
    QPixmap picture(static_cast<int>(py::round(width() * ratio)), static_cast<int>(py::round(height() * ratio)));
    picture.setDevicePixelRatio(ratio);
    picture.fill(Qt::transparent);
    return picture;
}

void PowerSpiral::paintEvent(QPaintEvent*) {
    const Geometry g = geometryNow();
    if (g.lane < 0.5) {
        return;
    }
    const auto ends = this->ends(g);
    const ftpcolors::ZoneRanges ranges = zones::powerRanges();
    const ColorsKey colorsKey{ranges, zones::palette(), ftpcolors::fadeFrom(ranges)};
    QColor trackColor(theme::current().mapHex(kTrackColor));
    trackColor.setAlphaF(static_cast<float>(kTrackOpacity));
    QColor lineColor(theme::current().mapHex(kLineColor));
    lineColor.setAlphaF(static_cast<float>(kLineOpacity));
    QPen linePen(lineColor, kLinePx);
    linePen.setJoinStyle(Qt::RoundJoin);
    linePen.setCapStyle(Qt::FlatCap);
    auto begin = [](QPainter& painter) {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
    };
    // everything but the ring and the spiral's turn being ridden (they're what move) drawn once, until any of it
    // changes
    const double ratio = devicePixelRatioF();
    const int active = std::min(kBlockTurns - 1, static_cast<int>(block_.fraction() * kBlockTurns));
    StillKey stillKey{g.key, ratio, blink_, active, colorsKey, trackColor.rgba(), lineColor.rgba(), {}};
    for (int k = 0; k < kBlockTurns; ++k) {
        if (k != active) {
            const double fraction = turnFraction(k);
            const bool moving = 0 < fraction && fraction < 1;
            stillKey.turns.append({block_.serial(), block_.duration(), k, moving,
                                   moving ? static_cast<double>(ends.second) : fraction});
        }
    }
    if (!still_ || !(still_->first == stillKey)) {
        QPixmap still = layer(ratio);
        QPainter painter(&still);
        begin(painter);
        painter.setBrush(trackColor);
        if (!track_ || track_->first != g.key) {
            const double lane = g.lane;
            track_ = std::pair(g.key, fullRing(g, lane).united(ribbon(g, spiralRadius(g), 0, 2 * kPi * kBlockTurns, lane)));
        }
        painter.drawPath(track_->second);
        for (int k = 0; k < kBlockTurns; ++k) {
            if (k != active) {
                paintTurn(painter, g, k, colorsKey, ranges, linePen, true, true);
            }
        }
        painter.end();
        still_ = std::pair(stillKey, still);
    }
    // and the ring's and the turn being ridden's finished stretches, until another kChunkBins are: only the rest drawn
    // every time
    const MovingKey movingKey{g.key,
                              ratio,
                              blink_,
                              colorsKey,
                              lineColor.rgba(),
                              workout_.serial(),
                              workout_.duration(),
                              doneBins(workout_.fraction()),
                              block_.serial(),
                              block_.duration(),
                              active,
                              doneBins(turnFraction(active))};
    if (!movingDone_ || !(movingDone_->first == movingKey)) {
        QPixmap moving = layer(ratio);
        QPainter painter(&moving);
        begin(painter);
        paintRing(painter, g, colorsKey, ranges, linePen, true, false);
        paintTurn(painter, g, active, colorsKey, ranges, linePen, true, false);
        painter.end();
        movingDone_ = std::pair(movingKey, moving);
    }
    QPainter painter(this);
    painter.drawPixmap(0, 0, still_->second);
    painter.drawPixmap(0, 0, movingDone_->second);
    begin(painter);
    paintRing(painter, g, colorsKey, ranges, linePen, false, true);
    paintRideTicks(painter, g, lineColor);
    paintTurn(painter, g, active, colorsKey, ranges, linePen, false, true);
    painter.end();
    drawnEnds_ = ends;
}

}  // namespace awo

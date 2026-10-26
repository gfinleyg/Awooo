#pragma once

// The workout screen's spiral (ClassPowerSpiral.py), round and centered in the widget, every lane the same width with
// kSpacePx between them, and a hole in the middle for the grade (GradeOverlay asks innerCircle() where it is):
//
//   the ring    the outer lane, a circle: the workout, filled clockwise from North until it meets itself at the end;
//               the session's rides before it first (see Session), each drawn with its own power zones and a tick
//               where it ends
//   the spiral  inside it, kBlockTurns turns winding in clockwise from North: the block being ridden, the same length
//               whatever the block (an interval's every rep of every sub-block along it, one after the other)
//
// Both are drawn with what was ridden along them, a plot of the cadence across the lane (the inner edge at
// kCadenceMinRpm or under, the outer edge from kCadenceMaxRpm; the outer edge where there was no cadence to show): a
// line, filled from the lane's inner edge up to it in the color of the power's zone (power zones in Musical Cadence
// Mode too) and see-through above it.
//
// The workout screen sets how far along the workout and the block are (setProgress) and adds the power and cadence
// ridden as it goes (addSample); each keeps them in bins, kBins a turn. Finished stretches are drawn once, kChunkBins
// at a time; only the stretch the end's in is drawn every frame.

#include <QPainterPath>
#include <QPixmap>
#include <QWidget>

#include <array>
#include <functional>
#include <map>
#include <optional>

#include "FtpColors.hpp"
#include "Session.hpp"

namespace awo {

namespace spiral {

constexpr int kBins = 1440;  // a turn's, one every quarter degree
constexpr int kChunkBins = 8;  // the finished bins drawn again (and kept) this many at a time
constexpr int kBlockTurns = 3;
constexpr double kCadenceMinRpm = 40;
constexpr double kCadenceMaxRpm = 105;
constexpr double kSpacePx = 1.0;
constexpr double kHole = 0.36;  // the hole's radius, as a share of the spiral's
// the lanes, faint, in the theme's text color
extern const QString kTrackColor;
constexpr double kTrackOpacity = 0.09;
// the cadence line, in the theme's text color
extern const QString kLineColor;
constexpr double kLineOpacity = 0.9;
constexpr double kLinePx = 1.5;
// the ticks across the ring where the session's earlier rides end, in the cadence line's color
constexpr double kTickPx = 2.5;
// an end has to move this far (px) before it's drawn again
constexpr double kRedrawPx = 0.5;

// how far across its lane the line is at a cadence: none (no cadence to show) the outer edge
double cadenceShare(std::optional<double> rpm);

// a bin's power and cadence (none: nothing ridden there yet; a cadence none where none was shown)
struct BinValue {
    std::optional<double> power;
    std::optional<double> cadence;
    bool operator==(const BinValue&) const = default;
};

// a length of time (seconds), how far along it is and what was ridden along it, in bins
class Timeline {
public:
    explicit Timeline(double duration, int bins = kBins);

    int serial() const { return serial_; }
    int bins() const { return bins_; }
    double duration() const { return duration_; }
    int version() const { return version_; }
    const QList<RingSample>& samples() const { return samples_; }
    double seconds = 0.0;

    double fraction() const;
    // a new length (a ramp test's grows): what was ridden binned again where it now falls
    void setDuration(double duration);
    void add(double seconds, double power, std::optional<double> cadence);
    // each bin's averages; a bin nothing landed in the one before's, the bins before the first sample (it comes a
    // tick in) the first's; none all along with nothing ridden
    const QList<BinValue>& values() const;

private:
    struct Sums {
        double power = 0, cadence = 0;
        int count = 0, cadenceCount = 0;
    };
    void bin(const RingSample& sample);

    int serial_;
    int bins_;
    double duration_;
    QList<RingSample> samples_;
    QList<Sums> sums_;
    int version_ = 0;
    mutable std::optional<std::pair<int, QList<BinValue>>> values_;
};

}  // namespace spiral

class PowerSpiral : public QWidget {
    Q_OBJECT

public:
    enum class Blink { Default, Block, Workout };

    explicit PowerSpiral(QWidget* parent = nullptr);

    // nothing ridden: the ring and the spiral empty. history: the session's rides before this workout, along the
    // ring one after the other before it
    void clear(double workoutDuration = 1.0, const QList<session::RingHistory>& history = {});
    // (seconds, one sample a second) of this workout's part of the ring, for the session's next rides
    Ring ridden() const;
    // a new block, duration seconds long: the spiral empty
    void setBlock(double duration);
    // how far along the workout and the block are (seconds), and how long they are
    void setProgress(double workoutSeconds, double workoutDuration, double blockSeconds, double blockDuration);
    // the power (watts) and cadence (rpm, none: no cadence to show) ridden, workoutSeconds into the workout and
    // blockSeconds into the block
    void addSample(double workoutSeconds, double blockSeconds, double power, std::optional<double> cadence);
    // Block: the spiral fades; Workout: the ring fades, the spiral some
    void setBlink(Blink blink);

    struct Geometry {
        QPointF center;
        double rInner, pitch, lane, ringR, spiralR;
        QSize key;
    };
    // the center, the hole's radius, the distance from one lane to the next (pitch), a lane's width, the ring's
    // middle radius and the spiral's where it starts: the ring's outer edge touches the widget, the spiral's last
    // lane's inner edge the hole
    Geometry geometryNow() const;
    // (center, radius) of the hole in the middle
    std::pair<QPointF, double> innerCircle() const;
    std::function<double(double)> spiralRadius(const Geometry& g) const;

    const spiral::Timeline& workout() const { return workout_; }
    const spiral::Timeline& block() const { return block_; }
    double historySeconds() const { return historySeconds_; }
    // (tests) how many times it asked to be repainted
    int updatesRequested() const { return updatesRequested_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct ColorsKey {
        ftpcolors::ZoneRanges ranges;
        ftpcolors::Palette palette;
        double fade;
        bool operator==(const ColorsKey&) const = default;
    };
    struct Piece {
        QBrush brush;
        QPainterPath area, line;
    };
    using HeightOf = std::function<std::optional<double>(int)>;
    using RadiusAt = std::function<double(double)>;

    void requestUpdate();
    std::pair<long long, long long> ends(const Geometry& g) const;
    QPainterPath ribbon(const Geometry& g, const RadiusAt& radiusAt, double start, double end, double width) const;
    QPainterPath fullRing(const Geometry& g, double width) const;
    HeightOf heights(const Geometry& g, const spiral::Timeline& timeline) const;
    std::pair<QPainterPath, QPainterPath> plot(const Geometry& g, const RadiusAt& radiusAt, double start, double end,
                                               const HeightOf& heightOf) const;
    std::function<ftpcolors::ZoneRanges(int)> binRanges(const spiral::Timeline& timeline,
                                                        const ftpcolors::ZoneRanges& ranges) const;
    const QList<QColor>& binColors(const spiral::Timeline& timeline, const ftpcolors::ZoneRanges& ranges,
                                   const ColorsKey& colorsKey);
    QBrush powerBrush(const Geometry& g, const spiral::Timeline& timeline, int firstBin, int fromBin, int toBin,
                      const ftpcolors::ZoneRanges& ranges, const ColorsKey& colorsKey);
    static int doneBins(double fraction);
    double turnFraction(int k) const;
    void paintRidden(QPainter& painter, int name, const Geometry& g, const spiral::Timeline& timeline,
                     const RadiusAt& radiusAt, int turn, double fraction, const ColorsKey& colorsKey,
                     const ftpcolors::ZoneRanges& ranges, const QPen& linePen, bool done, bool tip);
    void paintRing(QPainter& painter, const Geometry& g, const ColorsKey& colorsKey, const ftpcolors::ZoneRanges& ranges,
                   const QPen& linePen, bool done, bool tip);
    void paintRideTicks(QPainter& painter, const Geometry& g, const QColor& lineColor);
    void paintTurn(QPainter& painter, const Geometry& g, int k, const ColorsKey& colorsKey,
                   const ftpcolors::ZoneRanges& ranges, const QPen& linePen, bool done, bool tip);
    double ringOpacity() const;
    double spiralOpacity() const;
    QPixmap layer(double ratio) const;

    spiral::Timeline workout_{1.0};
    spiral::Timeline block_{1.0, spiral::kBins * spiral::kBlockTurns};
    double historySeconds_ = 0.0;
    struct HistoryRange {
        double start, end;
        ftpcolors::ZoneRanges ranges;
    };
    QList<HistoryRange> historyRanges_;
    qsizetype historyCount_ = 0;
    Blink blink_ = Blink::Default;
    std::optional<std::pair<long long, long long>> drawnEnds_;
    int updatesRequested_ = 0;

    // what's been worked out, until it changes
    std::optional<std::pair<QSize, QPainterPath>> track_;
    struct DoneKey {
        QSize size;
        int serial;
        double duration;
        int turn;
        int done;
        ColorsKey colors;
        bool operator==(const DoneKey&) const = default;
    };
    // the finished stretches, by name (-1 the ring, else the spiral's turn)
    std::map<int, std::pair<DoneKey, Piece>> donePieces_;
    struct BinColorsEntry {
        int version;
        QList<double> powers;
        QList<QColor> colors;
    };
    struct BinColorsKey {
        int serial;
        ColorsKey colors;
        double duration;
        bool operator==(const BinColorsKey&) const = default;
    };
    QList<std::pair<BinColorsKey, BinColorsEntry>> binColors_;
    struct StillKey {
        QSize size;
        double ratio;
        Blink blink;
        int active;
        ColorsKey colors;
        QRgb track, line;
        QList<std::tuple<int, double, int, bool, double>> turns;
        bool operator==(const StillKey&) const = default;
    };
    std::optional<std::pair<StillKey, QPixmap>> still_;
    struct MovingKey {
        QSize size;
        double ratio;
        Blink blink;
        ColorsKey colors;
        QRgb line;
        int workoutSerial;
        double workoutDuration;
        int workoutDone;
        int blockSerial;
        double blockDuration;
        int active;
        int blockDone;
        bool operator==(const MovingKey&) const = default;
    };
    std::optional<std::pair<MovingKey, QPixmap>> movingDone_;
};

}  // namespace awo

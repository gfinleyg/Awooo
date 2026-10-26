#pragma once

// Grades each workout block on how closely the power ridden followed the target power (WorkoutGrading.py).
//
// The workout screen feeds a BlockGrader every 100ms tick while the workout runs (paused time never reaches it), with
// the target power at that moment (ramp steps and difficulty changes included) and the latest power reading, or none
// while the power sensor has dropped out. Dropouts are left out of the grade entirely. When the block ends, finish()
// gives its grade:
//
//   steady/ramp block, as fractions of the target (1.0 = on target):
//     r_avg   = sum(actual) / sum(target)            over the graded time (a ramp is compared second by second)
//     r_low   = lowest 3 s average                   capped at 1
//     r_high  = 95th percentile of the 3 s averages
//     I       = average target / FTP                 the block's intensity
//     base    = (r_low + 5*min(r_avg, 1)) / 6
//     overAvg = OVER_SLOPE * (r_avg - 1 - tol)       when positive; tol = 3% at or below FTP, rising to 8% at 2x
//                                                    FTP, and twice the slope for recovery blocks (under 0.75 FTP)
//     surge   = 0.2 * (r_high - 1 - tolHigh)         when positive; tolHigh = 10% at or below FTP, 20% at 2x FTP
//     bonus   = 0.04 * clamp(I - 0.5, 0, 1)
//     score   = base - min(overAvg + surge, 0.20) + bonus
//   blocks under kShortBlockSeconds skip the low/high terms: base = min(r_avg, 1)
//   free ride block: score = time with power above kFreeRideMinWatts / graded time
//
// Grace periods at the start (time to reach the new target) and end of each block aren't graded, nor is the time
// getting back up to the target after a pause (see resume). A workout ended part way through a block grades that
// block up to then (finish(endedAt)), weighted in the workout's average by the time ridden; blocks never reached
// aren't graded at all. A score over 100% is an S; otherwise A+ to F, see letter().

#include <QList>
#include <QMap>
#include <QString>

#include <optional>
#include <tuple>
#include <utility>

#include "WorkoutTypes.hpp"

namespace awo::grading {

// grace at the start of a block: kGraceBaseSeconds plus a second per kGraceWattsPerSecond of target change from the
// block before, at most kGraceMaxSeconds and kGraceMaxFraction of the block
constexpr double kGraceBaseSeconds = 3.0;
constexpr double kGraceWattsPerSecond = 50.0;
constexpr double kGraceMaxSeconds = 10.0;
constexpr double kGraceMaxFraction = 0.25;
// grace at the end of a block, at most kEndGraceMaxFraction of the block
constexpr double kEndGraceSeconds = 2.0;
constexpr double kEndGraceMaxFraction = 0.10;
// a power reading older than this counts as a dropout (the sensor sends ~4 a second)
constexpr double kDropoutSeconds = 2.0;
constexpr double kShortBlockSeconds = 10.0;
constexpr double kRollingSeconds = 3.0;
constexpr int kLowWeight = 1;
constexpr int kAvgWeight = 5;
constexpr double kHighPercentile = 0.95;
constexpr double kOverTolerance = 0.03;
constexpr double kOverToleranceExtra = 0.05;  // + extra x (I - 1), I capped at 2
constexpr double kOverSlope = 0.5;
constexpr double kRecoveryIntensity = 0.75;
constexpr double kRecoveryOverSlope = 1.0;
constexpr double kSurgeTolerance = 0.10;
constexpr double kSurgeToleranceExtra = 0.10;
constexpr double kSurgeSlope = 0.2;
constexpr double kMaxOverPenalty = 0.20;
constexpr double kBonusMax = 0.04;
constexpr double kBonusFromIntensity = 0.5;
constexpr double kFreeRideMinWatts = 10;

extern const QString kNoGrade;
extern const QString kNoGradeColor;

// The letter grade for a score in %, kNoGrade for none
QString letter(std::optional<double> pct);
// A letter's color, by its first character
QString gradeColor(const QString& grade);

double startGrace(double duration, double step);
double endGrace(double duration);

// A block's grade: the score and what went into it, for the synopsis. The workout screen adds what the block was
// (label, when, its highest target) when it records it
struct Grade {
    std::optional<double> pct;  // none when nothing could be graded
    bool free = false;
    double graded = 0;
    double dropped = 0;
    double grace = 0;
    double duration = 0;
    double ridden = 0;  // how much of the block was ridden
    bool endedEarly = false;
    std::optional<double> pedaling;  // free ride
    std::optional<double> avg, low, high, overAvg, surge, penalty, bonus, intensity;
    // recorded by the workout screen
    QString label;  // the block's text as ridden
    double start = 0;
    double end = 0;
    int top = 0;  // its highest target
};

// (dt, target, actual)
using Sample = std::tuple<double, double, double>;

class BlockGrader {
public:
    BlockGrader(double duration, double previousTarget, int ftp, bool freeRide);

    // The workout was paused (the trainer drops to 0 W) and is going again blockSeconds into the block: the time to
    // get back up to the target is grace, as at the start of a block
    void resume(double blockSeconds, double target);
    // One tick: blockSeconds into the block, dt long; actual empty during a dropout
    void add(double blockSeconds, double dt, double target, std::optional<double> actual);
    // The block's grade; endedAt: the block seconds the workout was ended at, when that was part way through it
    Grade finish(std::optional<double> endedAt = std::nullopt) const;

    std::optional<double> firstTarget() const { return firstTarget_; }

private:
    double duration_;
    double previousTarget_;
    int ftp_;
    bool freeRide_;
    std::optional<double> startGrace_;  // set from the first target seen
    double endGrace_;
    // each graded tick, and each tick in a grace period (graded instead when nothing else could be: a block too
    // short to have time outside its grace periods)
    QList<Sample> samples_;
    QList<Sample> graceSamples_;
    double droppedSeconds_ = 0;
    double graceSeconds_ = 0;
    std::optional<double> firstTarget_;
    double resumeGraceUntil_ = 0;  // block seconds, after a pause
};

// actual/target over each kRollingSeconds of graded time, one per tick once that much has been graded; just the
// whole if there's less
QList<double> rollingRatios(const QList<Sample>& samples);
double percentile(QList<double> values, double fraction);
// The latest power reading, or none if there's been none for kDropoutSeconds
std::optional<double> latestPower(const QList<double>& times, const QList<double>& values, double now);

// Which block a grade is for: (workout entry index, interval block index, repeat); (index, 0, 0) for a block on its own
struct GradeKey {
    int index = 0;
    int sub = 0;
    int rep = 0;

    bool operator==(const GradeKey&) const = default;
};

// The workout's grades in the order they were recorded (a grade recorded again keeps its place)
class Grades {
public:
    void set(const GradeKey& key, const Grade& grade);
    const Grade* find(const GradeKey& key) const;
    const QList<std::pair<GradeKey, Grade>>& items() const { return items_; }
    bool isEmpty() const { return items_.isEmpty(); }
    void clear() { items_.clear(); }

private:
    QList<std::pair<GradeKey, Grade>> items_;
};

// A grade list's row:
//   Block     a block ("note": "ended early", or "")
//   Interval  an interval's header ("3x"), then its blocks as Sub rows: their average over the repeats ridden, and
//             each repeat's grade in "note" ("S · A · B+")
//   Song      Musical Cadence Mode: a song ridden, graded by its blocks' average; its blocks are indented under it
struct GradeRow {
    enum class Kind { Block, Interval, Sub, Song };

    Kind kind;
    QString label;
    std::optional<double> pct;
    QString grade;
    QString note;
    bool indent = false;
};

// The rows for a grade list in workout order, only for what was ridden. songs: Musical Cadence Mode's {entry index:
// song title} of each song's first block
QList<GradeRow> workoutRows(const QList<WorkoutEntry>& entries, const Grades& grades, const QMap<int, QString>& songs = {});

// Scores weighted by how long each block was ridden, over the graded ones; none if none
std::optional<double> riddenAverage(const QList<const Grade*>& grades);
// The workout's grade
std::optional<double> workoutAverage(const Grades& grades);

}  // namespace awo::grading

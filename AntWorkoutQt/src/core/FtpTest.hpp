#pragma once

// The FTP tests (FtpTest.py), each an ungraded workout built like an .awo file, starting with the same warmup. Then:
//   effort  (20m, 1h)          one free ride effort; the HUD's spiral shows its average power (95% of it for the
//                              20m test, the FTP it works out to)
//   ramp    (10W/20W delta)    1m steady blocks, each a step up from the last, added by the HUD 10s before the one
//                              being ridden ends, until the rider stops pedaling for 10s; the spiral shows 75% of the
//                              best 60s average power ridden in the ramp
// The synopsis offers the FTP the test works out to as the new FTP.

#include <QList>
#include <QString>
#include <QStringList>

#include <deque>
#include <optional>
#include <utility>

#include "WorkoutTypes.hpp"

namespace awo::ftptest {

// the warmup, as .awo lines: 3m at 50% FTP, 1m at 100%, 2m at 60%, 1m at 110%, then an 8m ramp from 60% to 80%
extern const QStringList kWarmupLines;
// with no FTP set (only a ramp test can be ridden without one) the warmup is a free ride as long
constexpr int kWarmupSeconds = 180 + 60 + 120 + 60 + 480;

// (seconds before the effort starts, seconds shown, text)
struct ExtraMessage {
    int before;
    int seconds;
    QString text;
};

struct Test {
    enum class Kind { Effort, Ramp };

    QString key;           // "20m", "1h", "ramp10", "ramp20" (AppState's ftpTestMode)
    Kind kind;
    QString name;
    double ftpFactor;      // the share of the power measured that's the new FTP
    QString averageTitle;  // the spiral's heading over that FTP
    // effort tests
    int effortSeconds = 0;
    QList<ExtraMessage> extraMessages;
    // ramp tests: the first block's watts, each block's step up, and the FTP the zones are colored by with none set
    int startWatts = 0;
    int stepWatts = 0;
    int unsetFtpWatts = 0;
};

const QList<Test>& tests();
// (hardware tests) a shorter test: this warmup (as .awo lines) before every test, and key's effort this long
void shortenForTests(const QString& key, const QStringList& warmupLines, int effortSeconds);
const Test* find(const QString& key);  // nullptr for "" (not an FTP test) or an unknown key
bool isRamp(const QString& key);

// how long "GO!" and the two minutes left message stay up
constexpr int kGoSeconds = 10;
constexpr int kTwoMinutesLeftSeconds = 20;
// ramp tests: each block's length, how long before it ends the next is added, the best average power measured,
// and not pedaling (under this many watts, or no reading): how long before the warning, and before the test ends
constexpr int kRampBlockSeconds = 60;
constexpr int kRampAddBeforeSeconds = 10;
constexpr int kRampBestSeconds = 60;
constexpr int kNotPedalingWatts = 10;
constexpr int kNotPedalingWarnSeconds = 5;
constexpr int kNotPedalingEndSeconds = 10;
extern const QString kNotPedalingMessage;
// a ramp paused for longer than this starts over from its first block when it's unpaused
constexpr int kRampPauseSeconds = 60;

// The test as .awo lines (a ramp test's blocks are added as it's ridden, see rampBlock); the warmup is a free ride
// while no FTP is set
QStringList workoutLines(const QString& key, bool ftpSet = true);

// A ramp test's index'th block (0 the first): steady, the same watts at every difficulty (the ramp doesn't follow
// FTP), seated
WorkoutEntry rampBlock(const QString& key, int index);

// The test's timed messages in workout seconds; effortStart is when the warmup ends
QList<Message> messages(const QString& key, int effortStart, int ftp);

// The FTP the power a test measures works out to
int ftpFromAverage(const QString& key, double average);

// The test's effort as ridden, set by the workout screen: its average (none before any reading) and whether it
// was finished
struct Result {
    std::optional<double> average;
    bool completed = false;
};

// Time weighted average power over the effort; time with no power reading (a dropout) isn't counted
class EffortAverage {
public:
    void add(double seconds, std::optional<double> power);
    std::optional<double> average() const;

private:
    double wattSeconds_ = 0;
    double seconds_ = 0;
};

// The best time weighted average power over any `window` seconds ridden; time with no reading counts as 0 (in a
// ramp test that's not pedaling). Empty until `window` seconds have been ridden
class BestRollingAverage {
public:
    explicit BestRollingAverage(double window = kRampBestSeconds) : window_(window) {}

    void add(double seconds, std::optional<double> power);
    // forgets the time ridden so far, keeping the best: the next window doesn't reach back past here
    void startNewWindow();
    std::optional<double> average() const { return best_; }

private:
    double window_;
    std::deque<std::pair<double, double>> samples_;  // (seconds, power)
    double wattSeconds_ = 0;
    double seconds_ = 0;
    std::optional<double> best_;
};

}  // namespace awo::ftptest

#pragma once

// The workout as it's ridden (the workout logic of ClassHUDWindow.py, taken out of the screen): the workout clock and
// each block's, moving through the workout's entries (blocks, ramp steps, interval reps), the power (or cadence) asked
// for at the difficulty set, pausing, the end of the workout, each block's grade (WorkoutGrading), an FTP test's
// effort (FtpTest: a ramp test adds its blocks as they're ridden and ends when the rider stops pedaling), the timed
// messages, and the power requests the graph and synopsis draw (AppState's workoutPowerRequests*).
//
// The workout screen calls advanceClock() and tick() every 100 ms, start() on START, and so on, and shows what tick()
// says changed. Everything is in AppState, as the screen and synopsis read it.

#include <QDateTime>
#include <QStringList>

#include <functional>
#include <optional>
#include <variant>

#include "FtpTest.hpp"
#include "WorkoutGrading.hpp"
#include "WorkoutTypes.hpp"

namespace awo {

class WorkoutEngine {
public:
    // seconds on a steady clock (a ramp test's pause is timed with it)
    using Clock = std::function<double()>;

    // what a tick changed, for the screen to show
    struct TickEvents {
        bool running = false;           // the workout clock moved (started and unpaused)
        bool completed = false;         // the end of the workout was reached
        bool showGrades = false;        // (completed, graded: every block's grade takes over the left column)
        bool blockStarted = false;      // a new block (or interval) began: its labels, the spiral's block, up next
        bool intervalBlockStarted = false;  // the interval's next block began
        std::optional<QString> finishedGrade;  // the block that just ended's grade, shown over the spiral
        bool powerChanged = false;      // a new target (Ride At and the target arc)
        bool upNextChanged = false;     // what's coming up changed (a ramp test's next block, a difficulty)
        bool lengthChanged = false;     // the workout's length changed (a ramp test)
        bool difficultyApplied = false; // the difficulty set took effect: the current block's labels again
    };

    // a line of what's coming up: one text, or an interval (its header and a line for each of its blocks)
    using UpcomingLine = QStringList;

    explicit WorkoutEngine(Clock clock = {});

    // ---- Driving it ----

    // the clock moved timeDelta (any mode): the ride's seconds, and the first power request time
    void advanceClock(double timeDelta);
    // START
    void start();
    // the workout's events for this tick (after advanceClock); musicSeconds: Musical Cadence Mode's music position,
    // the workout's clock (none: the clock's own time)
    TickEvents tick(std::optional<double> musicSeconds = std::nullopt);
    // pause / resume (only once started); true when resuming started a paused-too-long ramp test over
    bool pauseRecording();
    // the slider: -10 to +10 difficulty steps
    void setDifficulty(int value);
    // Keep Existing Data and Restart: the workout as built, from the start, not started
    void restart();
    // ended part way (END, Exit to Synopsis): the block being ridden graded up to now
    void endGrading();

    // ---- What it's at ----

    bool started() const { return started_; }
    bool unpaused() const { return unpaused_; }
    bool newWorkoutStart() const { return newWorkoutStart_; }
    bool completeShown() const { return workoutCompleteShown_; }
    bool workoutComplete() const;
    int activeWorkout() const { return activeWorkout_; }
    int activeDifficulty() const { return activeDifficulty_; }
    int currentPower() const { return currentPower_; }
    // the block being ridden (a block, or an interval's header) and the entry setting its target now (the block, a
    // ramp step or an interval block)
    const std::optional<WorkoutEntry>& currentEventText() const { return currentEventText_; }
    const std::optional<WorkoutEntry>& currentEventSettings() const { return currentEventSettings_; }
    int currentIntervalBlockIndex() const { return currentIntervalBlockIndex_; }
    double blockDuration() const { return blockDuration_; }
    double blockSecondsPassed() const { return blockSecondsPassed_; }
    double workoutSecondsPassed() const { return workoutSecondsPassed_; }
    double eventTimerSeconds() const { return eventTimerSeconds_; }
    double timeDelta() const { return timeDelta_; }
    // (seconds along, seconds long) of the whole block for the spiral: an interval's every rep of every block
    std::pair<double, double> spiralBlockTime(double sinceTick = 0.0) const;
    // the interval (header) being ridden: its kind code as the Python app numbered entries (0 free, 1 steady,
    // 2 ramp, 3 interval), -1 before the first block
    int currentKind() const;

    // the FTP test being ridden (none: a workout)
    const ftptest::Test* ftpTest() const { return ftpTest_; }
    bool rampTest() const { return rampTest_; }
    bool inFtpTestEffort() const;
    double notPedalingSeconds() const { return notPedalingSeconds_; }
    // the spiral's middle through an FTP test's effort: (title, value)
    std::optional<std::pair<QString, QString>> effortDisplay() const;
    // a paused ramp test's pause button ("PAUSE\n42s", "PAUSE\nRESET"), none otherwise
    std::optional<QString> rampPauseText() const;

    // the timed message for the workout time now (a ramp test's not pedaling warning first)
    QString message() const;
    // the workout's entries after the one being ridden, as the lists show them at the difficulty set (a ramp test's
    // next block too, before it's added)
    QList<UpcomingLine> upcoming() const;
    // the upcoming targets for the graph's bars: (times, powers), times one more than powers, from secondsPassed,
    // collectionPeriod seconds of them at most
    std::pair<QList<double>, QList<double>> upcomingGraph(double collectionPeriod) const;

    // the difficulty slider's FTP text ("FTP: 270", "FTP: --" with none set)
    static QString difficultyFtpText(int value);

private:
    using Average = std::variant<ftptest::EffortAverage, ftptest::BestRollingAverage>;

    void storeWorkoutPowerTime(int power);
    void beginGrading(const grading::GradeKey& key, const Block& block);
    void gradeTick();
    std::optional<QString> finishGrading(bool endedEarly = false);
    void resetFtpTest();
    void restartRamp();
    ftptest::Result ftpTestResult(bool completed) const;
    void ftpTestTick(TickEvents& events);
    void rampTick(std::optional<double> power, TickEvents& events);
    void endRamp();
    std::optional<double> effortAverage() const;

    Clock clock_;
    // the workout as built (a ramp test adds blocks as it goes; a restart starts from this again)
    QList<WorkoutEntry> builtWorkoutList_;
    int builtWorkoutSeconds_ = 0;
    int effortStartIndex_ = 0;
    const ftptest::Test* ftpTest_ = nullptr;
    bool rampTest_ = false;
    bool cadenceMode_ = false;

    int activeWorkout_ = -1;
    int activeDifficulty_ = 10;
    double eventDuration_ = 0;
    std::optional<WorkoutEntry> currentEventText_;
    std::optional<WorkoutEntry> currentEventSettings_;
    int currentPower_ = 0;
    double blockDuration_ = 0;
    int currentIntervalBlockIndex_ = 0;
    double intTotal_[3] = {1, 1, 1};
    double intPassed_[3] = {0, 0, 0};

    double timeDelta_ = 0;
    double workoutDelta_ = 0;
    double workoutSecondsPassed_ = 0;
    double blockSecondsPassed_ = 0;
    double eventTimerSeconds_ = 0;
    double eventOverage_ = 0;
    double blockOverage_ = 0;

    bool started_ = false;
    bool unpaused_ = false;
    bool difficultyChanged_ = false;
    bool newWorkoutStart_ = false;
    bool workoutCompleteShown_ = false;
    bool workoutGraded_ = false;

    // the block being graded, its key and what's recorded with it; the interval being ridden (index, repeats)
    std::optional<grading::BlockGrader> grader_;
    grading::GradeKey gradeKey_;
    QString gradeLabel_;
    double gradeStart_ = 0;
    std::pair<int, int> gradeInterval_{0, 1};
    int gradeTopTarget_ = 0;
    int lastGradeTarget_ = 0;

    // an FTP test's effort average, and a ramp test's not pedaling time, warning and pause
    Average effort_;
    double notPedalingSeconds_ = 0;
    QString ftpTestMessage_;
    std::optional<double> rampPausedAt_;
};

}  // namespace awo

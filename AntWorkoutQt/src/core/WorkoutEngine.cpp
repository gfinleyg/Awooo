#include "WorkoutEngine.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>

#include "AppState.hpp"
#include "WorkoutHelpers.hpp"

namespace awo {

namespace {

double steadyClock() {
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
    }
    return timer.nsecsElapsed() / 1e9;
}

}  // namespace

WorkoutEngine::WorkoutEngine(Clock clock) : clock_(clock ? std::move(clock) : Clock(steadyClock)) {
    AppState& state = appState();
    state.blockGrades.clear();
    state.musicPlayed.clear();
    cadenceMode_ = state.workoutMode && state.cadenceMode;
    // an FTP test isn't graded; its effort's (last block's) average power shows in the spiral instead
    ftpTest_ = ftptest::find(state.ftpTestMode);
    // a ramp test adds its blocks as they're ridden; a restart starts from the workout as built again
    rampTest_ = ftpTest_ && ftpTest_->kind == ftptest::Test::Kind::Ramp;
    builtWorkoutList_ = state.workoutList;
    builtWorkoutSeconds_ = state.workoutDurationSeconds;
    // the effort starts with the workout's last block as built: an effort test's free ride, a ramp test's first block
    effortStartIndex_ = static_cast<int>(state.workoutList.size()) - 1;
    resetFtpTest();
}

// ---- Driving it ----

void WorkoutEngine::advanceClock(double timeDelta) {
    AppState& state = appState();
    // (the power requests start with the ride's first moment)
    if (state.workoutPowerRequestsSeconds.isEmpty()) {
        state.workoutPowerRequestsSeconds.append(state.secondsPassed);
        state.workoutPowerRequestsSysTime.append(QDateTime::currentDateTime());
    }
    timeDelta_ = timeDelta;
    state.secondsPassed += timeDelta;
}

void WorkoutEngine::start() {
    started_ = true;
    unpaused_ = true;
    newWorkoutStart_ = true;
}

void WorkoutEngine::setDifficulty(int value) {
    // (2% of FTP a step)
    activeDifficulty_ = value + 10;
    difficultyChanged_ = true;
}

int WorkoutEngine::currentKind() const {
    if (!currentEventText_) {
        return -1;
    }
    if (currentEventText_->type == WorkoutEntry::Type::Interval) {
        return 3;
    }
    return static_cast<int>(currentEventText_->block.type);
}

bool WorkoutEngine::workoutComplete() const {
    return activeWorkout_ + 1 == appState().workoutList.size() && blockSecondsPassed_ >= blockDuration_;
}

WorkoutEngine::TickEvents WorkoutEngine::tick(std::optional<double> musicSeconds) {
    AppState& state = appState();
    TickEvents events;
    bool upcomingUpdated = false;
    bool clusterUpdated = false;
    // the end of the workout, at the end of its last block (once: nothing changes after it)
    if (workoutComplete() && !workoutCompleteShown_) {
        workoutCompleteShown_ = true;
        events.completed = true;
        // the last block graded; every block's grade then takes over the left column
        if (!workoutGraded_) {
            events.finishedGrade = finishGrading();
            workoutGraded_ = true;
            if (ftpTest_) {
                // nothing graded: the spiral keeps the effort's average
                state.ftpTestResult = ftpTestResult(true);
            } else {
                events.showGrades = true;
            }
        }
        pauseRecording();
        started_ = false;
    }
    // no power asked for while paused
    if (!unpaused_) {
        storeWorkoutPowerTime(0);
    }
    if (unpaused_ && started_) {
        events.running = true;
        // the timers move (not on the first tick, which starts the first block)
        if (!newWorkoutStart_) {
            events.timersMoved = true;
            // (the music's progress in Musical Cadence Mode, so the targets can't drift from it)
            workoutDelta_ = musicSeconds ? std::max(0.0, *musicSeconds - workoutSecondsPassed_) : timeDelta_;
            blockSecondsPassed_ += workoutDelta_;
            workoutSecondsPassed_ += workoutDelta_;
            eventTimerSeconds_ += workoutDelta_;
            storeWorkoutPowerTime(currentPower_);
            // graded against the power asked for
            gradeTick();
            ftpTestTick(events);
            // (the spiral's time in an interval: only the interval block being ridden's)
            if (currentKind() == 3) {
                intPassed_[std::min(currentIntervalBlockIndex_, 2)] += workoutDelta_;
            }
        }
        // the next entry, when this one's time is up (or the workout's just starting)
        if (newWorkoutStart_ || eventTimerSeconds_ > eventDuration_ || blockSecondsPassed_ > blockDuration_) {
            if (eventTimerSeconds_ > eventDuration_) {
                eventOverage_ = std::max(0.0, eventTimerSeconds_ - eventDuration_);
            }
            if (blockSecondsPassed_ > blockDuration_) {
                blockOverage_ = std::max(0.0, blockSecondsPassed_ - blockDuration_);
            }
            if (activeWorkout_ + 1 != state.workoutList.size()) {
                eventTimerSeconds_ = eventOverage_;
                eventDuration_ = 0;
                ++activeWorkout_;
                WorkoutEntry current = state.workoutList[activeWorkout_];
                const int difficulty = activeDifficulty_;
                if (current.type == WorkoutEntry::Type::Block || current.type == WorkoutEntry::Type::Interval) {
                    // a new block: the one that just ended graded, the new one graded from here (an interval from its
                    // first rep)
                    const std::optional<QString> finishedGrade = finishGrading();
                    const int gradedIndex = activeWorkout_;
                    const WorkoutEntry graded = current;
                    blockDuration_ = 0.0;
                    if (!newWorkoutStart_) {
                        blockSecondsPassed_ = blockOverage_;
                    }
                    currentPower_ = 0;
                    if (current.type == WorkoutEntry::Type::Block) {
                        const Block& block = current.block;
                        blockDuration_ = block.seconds;
                        currentEventText_ = current;
                        if (block.type == BlockType::Ramp) {
                            // the ramp's steps follow it: the first sets the target
                            ++activeWorkout_;
                            current = state.workoutList[activeWorkout_];
                            currentEventSettings_ = current;
                            eventDuration_ = current.stepSeconds;
                            currentPower_ = current.stepPower[difficulty];
                        } else {
                            eventDuration_ = block.seconds;
                            if (block.type == BlockType::Steady) {
                                currentPower_ = block.power[difficulty];
                            }
                            currentEventSettings_ = current;
                        }
                    } else {
                        // an interval: the spiral's length is every rep of each of its blocks
                        for (int i = 0; i < 3; ++i) {
                            intTotal_[i] = 1;
                            intPassed_[i] = 0;
                        }
                        currentEventText_ = current;
                        currentIntervalBlockIndex_ = 0;
                        for (qsizetype i = 0; i < current.intervalBlocks.size() && i < 3; ++i) {
                            intTotal_[i] = static_cast<double>(current.repeats) * current.intervalBlocks[i].seconds;
                        }
                        // its first block's entry follows it (and a ramp's first step after that)
                        ++activeWorkout_;
                        current = state.workoutList[activeWorkout_];
                        const Block& block = current.block;
                        blockDuration_ = block.seconds;
                        if (block.type == BlockType::FreeRide) {
                            eventDuration_ = block.seconds;
                            currentPower_ = 0;
                        } else if (block.type == BlockType::Steady) {
                            eventDuration_ = block.seconds;
                            currentPower_ = block.power[difficulty];
                        } else {
                            ++activeWorkout_;
                            current = state.workoutList[activeWorkout_];
                            eventDuration_ = current.stepSeconds;
                            currentPower_ = current.stepPower[difficulty];
                        }
                        currentEventSettings_ = current;
                    }
                    events.blockStarted = true;
                    if (graded.type == WorkoutEntry::Type::Interval) {
                        gradeInterval_ = {gradedIndex, graded.repeats};
                        beginGrading({gradedIndex, 0, 0}, graded.intervalBlocks.front());
                    } else {
                        beginGrading({gradedIndex, 0, 0}, graded.block);
                    }
                    if (finishedGrade) {
                        events.finishedGrade = finishedGrade;
                    }
                    clusterUpdated = upcomingUpdated = true;
                    events.upNextChanged = true;
                } else if (current.type == WorkoutEntry::Type::RampStep) {
                    // the ramp's next step
                    currentEventSettings_ = current;
                    eventDuration_ = current.stepSeconds;
                    currentPower_ = current.stepPower[difficulty];
                } else {
                    // the interval's next block: the one that just ended graded (each rep of each block on its own)
                    const std::optional<QString> finishedGrade = finishGrading();
                    const auto [intervalIndex, intervalReps] = gradeInterval_;
                    const grading::GradeKey gradedKey{intervalIndex, current.subIndex, intervalReps - current.repeats};
                    const Block gradedBlock = current.block;
                    currentIntervalBlockIndex_ = current.subIndex;
                    blockSecondsPassed_ = blockOverage_;
                    blockDuration_ = current.block.seconds;
                    if (current.block.type == BlockType::FreeRide) {
                        eventDuration_ = current.block.seconds;
                        currentPower_ = 0;
                    } else if (current.block.type == BlockType::Steady) {
                        eventDuration_ = current.block.seconds;
                        currentPower_ = current.block.power[difficulty];
                    } else {
                        // a ramp: its first step sets the target
                        ++activeWorkout_;
                        current = state.workoutList[activeWorkout_];
                        currentPower_ = current.stepPower[difficulty];
                        eventDuration_ = current.stepSeconds;
                    }
                    currentEventSettings_ = current;
                    events.intervalBlockStarted = true;
                    beginGrading(gradedKey, gradedBlock);
                    if (finishedGrade) {
                        events.finishedGrade = finishedGrade;
                    }
                }
                events.powerChanged = true;
                newWorkoutStart_ = false;
            }
        }
    }
    // a difficulty set (while the workout runs): the targets and labels at it
    if (difficultyChanged_ && started_) {
        if (!upcomingUpdated) {
            events.upNextChanged = true;
        }
        if (!clusterUpdated) {
            const int kind = currentKind();
            if (kind == 1) {
                currentPower_ = currentEventText_->block.power[activeDifficulty_];
                events.powerChanged = events.difficultyApplied = true;
            } else if (kind == 2) {
                currentPower_ = currentEventSettings_->stepPower[activeDifficulty_];
                events.powerChanged = events.difficultyApplied = true;
            } else if (kind == 3) {
                if (currentEventSettings_->type == WorkoutEntry::Type::IntervalBlock) {
                    // (free ride, or steady)
                    currentPower_ = 0;
                    if (currentEventSettings_->block.type == BlockType::Steady) {
                        currentPower_ = currentEventSettings_->block.power[activeDifficulty_];
                    }
                } else {
                    // a ramp step
                    currentPower_ = currentEventSettings_->stepPower[activeDifficulty_];
                }
                events.powerChanged = events.difficultyApplied = true;
            }
        }
        difficultyChanged_ = false;
    }
    return events;
}

bool WorkoutEngine::pauseRecording() {
    if (!started_) {
        return false;
    }
    unpaused_ = !unpaused_;
    bool restarted = false;
    if (unpaused_) {
        // grading stopped with the workout clock; getting back up to the target isn't graded either
        if (grader_) {
            grader_->resume(blockSecondsPassed_, currentPower_);
        }
        // a ramp test's not pedaling time starts over (the rider wasn't pedaling while paused)
        notPedalingSeconds_ = 0.0;
        ftpTestMessage_.clear();
        // a ramp paused too long starts over from its first block
        if (rampPausedAt_) {
            const bool expired = clock_() - *rampPausedAt_ >= ftptest::kRampPauseSeconds;
            rampPausedAt_.reset();
            if (expired) {
                restartRamp();
                restarted = true;
            }
        }
    } else if (rampTest_ && inFtpTestEffort() && !workoutComplete()) {
        // pausing in a ramp test's ramp counts down on the pause button (the pause a completed workout makes doesn't)
        rampPausedAt_ = clock_();
    }
    return restarted;
}

void WorkoutEngine::restart() {
    AppState& state = appState();
    // the workout as built (a ramp test added blocks to it)
    state.workoutList = builtWorkoutList_;
    state.workoutDurationSeconds = builtWorkoutSeconds_;
    started_ = unpaused_ = false;
    newWorkoutStart_ = false;
    workoutCompleteShown_ = false;
    activeDifficulty_ = 10;
    eventDuration_ = 0;
    currentEventText_.reset();
    currentEventSettings_.reset();
    workoutSecondsPassed_ = blockSecondsPassed_ = eventTimerSeconds_ = 0.0;
    activeWorkout_ = -1;
    grader_.reset();
    gradeKey_ = {};
    lastGradeTarget_ = 0;
    state.blockGrades.clear();
    state.musicPlayed.clear();
    workoutGraded_ = false;
    resetFtpTest();
}

void WorkoutEngine::endGrading() {
    finishGrading(true);
}

// ---- Power requests and grading ----

// the power asked for (the graph's bars, the synopsis): a new one appended, the same one just carried on
void WorkoutEngine::storeWorkoutPowerTime(int power) {
    AppState& state = appState();
    if (!state.workoutPowerRequestsWatts.isEmpty() && state.workoutPowerRequestsWatts.back() == power) {
        state.workoutPowerRequestsSeconds.back() = state.secondsPassed;
        state.workoutPowerRequestsSysTime.back() = QDateTime::currentDateTime();
        return;
    }
    state.workoutPowerRequestsSeconds.append(state.secondsPassed);
    state.workoutPowerRequestsWatts.append(power);
    state.workoutPowerRequestsSysTime.append(QDateTime::currentDateTime());
}

// a block (or one rep of an interval block) graded from now
void WorkoutEngine::beginGrading(const grading::GradeKey& key, const Block& block) {
    // FTP tests aren't graded, nor are switch single leg blocks (ridden as usual, left out of the grades)
    if (ftpTest_ || block.position == SwitchSingleLeg) {
        return;
    }
    const AppState& state = appState();
    // Musical Cadence Mode grades cadence against the target cadence, the block's own target standing in for FTP (an
    // intensity of 1: the grader's usual tolerances at threshold)
    int ftp = state.ftp;
    if (cadenceMode_) {
        ftp = block.type == BlockType::Steady ? block.power[activeDifficulty_] : 1;
    }
    grader_.emplace(block.seconds, lastGradeTarget_, ftp, block.type == BlockType::FreeRide);
    gradeKey_ = key;
    gradeLabel_ = blockText(block, activeDifficulty_, state.targetUnit());
    gradeStart_ = state.secondsPassed;
    gradeTopTarget_ = 0;
}

// (ticks only come while the workout runs, so paused time is never graded)
void WorkoutEngine::gradeTick() {
    if (!grader_) {
        // (a block that isn't graded still sets the target the next one is coming from)
        lastGradeTarget_ = currentPower_;
        return;
    }
    const AppState& state = appState();
    const SavedValues& actuals = cadenceMode_ ? state.savedCadenceVals : state.savedPowerVals;
    const std::optional<double> actual = grading::latestPower(actuals.times, actuals.values, state.secondsPassed);
    grader_->add(blockSecondsPassed_, workoutDelta_, currentPower_, actual);
    lastGradeTarget_ = currentPower_;
    gradeTopTarget_ = std::max(gradeTopTarget_, currentPower_);
}

// the block being ridden's grade recorded; its letter, none without a block or anything graded in it (the power
// sensor was out the whole time). endedEarly: graded up to now, the workout ended part way through it
std::optional<QString> WorkoutEngine::finishGrading(bool endedEarly) {
    if (!grader_) {
        return std::nullopt;
    }
    AppState& state = appState();
    grading::Grade result = grader_->finish(endedEarly ? std::optional(blockSecondsPassed_) : std::nullopt);
    result.label = gradeLabel_;
    result.start = gradeStart_;
    result.top = gradeTopTarget_;
    result.end = state.secondsPassed;
    state.blockGrades.set(gradeKey_, result);
    grader_.reset();
    if (!result.pct) {
        return std::nullopt;
    }
    return grading::letter(result.pct);
}

// ---- The FTP test ----

void WorkoutEngine::resetFtpTest() {
    // an effort test's average power, a ramp test's best 60 s
    if (rampTest_) {
        effort_ = ftptest::BestRollingAverage();
    } else {
        effort_ = ftptest::EffortAverage();
    }
    notPedalingSeconds_ = 0.0;
    ftpTestMessage_.clear();
    rampPausedAt_.reset();
    appState().ftpTestResult.reset();
}

std::optional<double> WorkoutEngine::effortAverage() const {
    return std::visit([](const auto& average) { return average.average(); }, effort_);
}

// the ramp starts over from its first block, as if the warmup had just ended (on the next tick); the best 60 s so far
// is kept, and the workout clock keeps going
void WorkoutEngine::restartRamp() {
    AppState& state = appState();
    state.workoutList = builtWorkoutList_;
    activeWorkout_ = effortStartIndex_ - 1;
    blockSecondsPassed_ = eventTimerSeconds_ = 0.0;
    blockOverage_ = eventOverage_ = 0.0;
    blockDuration_ = ftptest::kRampBlockSeconds;
    state.workoutDurationSeconds = static_cast<int>(workoutSecondsPassed_ + ftptest::kRampBlockSeconds);
    // (no 60 s spans the pause and the restart)
    if (auto* best = std::get_if<ftptest::BestRollingAverage>(&effort_)) {
        best->startNewWindow();
    }
    state.ftpTestResult = ftpTestResult(false);
    newWorkoutStart_ = true;
}

bool WorkoutEngine::inFtpTestEffort() const {
    // from the end of the warmup on: an effort test's free ride, every block of a ramp test
    return ftpTest_ && activeWorkout_ >= effortStartIndex_;
}

// the power the test measured, and whether it counts: a ramp test counts however it ended (stopping pedaling, or
// END) once it has a best 60 s
ftptest::Result WorkoutEngine::ftpTestResult(bool completed) const {
    const std::optional<double> average = effortAverage();
    return {average, rampTest_ ? average.has_value() : completed};
}

// (paused time doesn't count toward the average: ticks only come while the workout runs)
void WorkoutEngine::ftpTestTick(TickEvents& events) {
    if (!inFtpTestEffort()) {
        return;
    }
    const AppState& state = appState();
    const std::optional<double> power =
        grading::latestPower(state.savedPowerVals.times, state.savedPowerVals.values, state.secondsPassed);
    std::visit([&](auto& average) { average.add(timeDelta_, power); }, effort_);
    if (rampTest_) {
        rampTick(power, events);
    }
    appState().ftpTestResult = ftpTestResult(false);
}

void WorkoutEngine::rampTick(std::optional<double> power, TickEvents& events) {
    // stopping pedaling for long enough ends the test
    const bool pedaling = power && *power >= ftptest::kNotPedalingWatts;
    notPedalingSeconds_ = pedaling ? 0.0 : notPedalingSeconds_ + timeDelta_;
    ftpTestMessage_ = notPedalingSeconds_ >= ftptest::kNotPedalingWarnSeconds ? ftptest::kNotPedalingMessage : QString();
    if (notPedalingSeconds_ >= ftptest::kNotPedalingEndSeconds) {
        endRamp();
        events.lengthChanged = true;
        return;
    }
    // the next block is added kRampAddBeforeSeconds before the one being ridden ends
    AppState& state = appState();
    if (activeWorkout_ == state.workoutList.size() - 1 &&
        blockDuration_ - blockSecondsPassed_ <= ftptest::kRampAddBeforeSeconds) {
        const WorkoutEntry next = ftptest::rampBlock(state.ftpTestMode,
                                                     static_cast<int>(state.workoutList.size()) - effortStartIndex_);
        state.workoutList.append(next);
        state.workoutDurationSeconds += next.block.seconds;
        events.lengthChanged = events.upNextChanged = true;
    }
}

// the ramp test is over: the block being ridden ends now (the next tick sees the workout complete) and the one queued
// after it is dropped
void WorkoutEngine::endRamp() {
    AppState& state = appState();
    state.workoutList.resize(activeWorkout_ + 1);
    blockDuration_ = blockSecondsPassed_;
    state.workoutDurationSeconds = static_cast<int>(workoutSecondsPassed_);
    ftpTestMessage_.clear();
}

std::optional<std::pair<QString, QString>> WorkoutEngine::effortDisplay() const {
    if (!inFtpTestEffort()) {
        return std::nullopt;
    }
    // its average so far (for the 20m test, 95% of it: the FTP it works out to), a ramp test's 75% of its best 60 s
    const std::optional<double> average = effortAverage();
    const QString value = average ? QString::number(ftptest::ftpFromAverage(appState().ftpTestMode, *average))
                                  : QStringLiteral("--");
    return std::pair(ftpTest_->averageTitle, value);
}

std::optional<QString> WorkoutEngine::rampPauseText() const {
    if (!rampPausedAt_) {
        return std::nullopt;
    }
    const int remaining = static_cast<int>(std::ceil(ftptest::kRampPauseSeconds - (clock_() - *rampPausedAt_)));
    return remaining > 0 ? QStringLiteral("PAUSE\n%1s").arg(remaining) : QStringLiteral("PAUSE\nRESET");
}

// ---- What's shown ----

std::pair<double, double> WorkoutEngine::spiralBlockTime(double sinceTick) const {
    if (currentKind() == 3) {
        const qsizetype count = std::min<qsizetype>(currentEventText_->intervalBlocks.size(), 3);
        double passed = 0, total = 0;
        for (qsizetype i = 0; i < count; ++i) {
            passed += intPassed_[i];
            total += intTotal_[i];
        }
        return {passed + sinceTick, total};
    }
    return {blockSecondsPassed_ + sinceTick, blockDuration_};
}

QString WorkoutEngine::message() const {
    QString text;
    if (started_) {
        // (workout time only moves while unpaused, so a paused message stays up until the ride goes on)
        int latestStart = -1;
        for (const Message& message : appState().workoutMessages) {
            // overlapping: the one that started most recently
            if (message.start <= workoutSecondsPassed_ && workoutSecondsPassed_ < message.end &&
                message.start >= latestStart) {
                latestStart = message.start;
                text = message.text;
            }
        }
        // a ramp test's not pedaling warning first
        if (!ftpTestMessage_.isEmpty()) {
            text = ftpTestMessage_;
        }
    }
    return text;
}

QList<WorkoutEngine::UpcomingLine> WorkoutEngine::upcoming() const {
    const AppState& state = appState();
    const QString unit = state.targetUnit();
    QList<UpcomingLine> lines;
    for (qsizetype index = activeWorkout_ + 1; index < state.workoutList.size(); ++index) {
        // Musical Cadence Mode: a line for each song as it starts
        if (cadenceMode_ && state.musicSongStarts.contains(static_cast<int>(index))) {
            lines.append({state.musicSongStarts.value(static_cast<int>(index))});
        }
        const WorkoutEntry& entry = state.workoutList[index];
        if (entry.type == WorkoutEntry::Type::Block) {
            for (const QString& text : blockText(entry, activeDifficulty_, unit)) {
                lines.append({text});
            }
        } else if (entry.type == WorkoutEntry::Type::Interval) {
            lines.append(blockText(entry, activeDifficulty_, unit));
        }
    }
    // a ramp test's next block isn't added until just before this one ends: up next is what it will be
    if (lines.isEmpty() && rampTest_ && notPedalingSeconds_ < ftptest::kNotPedalingEndSeconds) {
        const WorkoutEntry next =
            ftptest::rampBlock(state.ftpTestMode, static_cast<int>(state.workoutList.size()) - effortStartIndex_);
        lines.append({blockText(next, activeDifficulty_, unit).front()});
    }
    return lines;
}

std::pair<QList<double>, QList<double>> WorkoutEngine::upcomingGraph(double collectionPeriod) const {
    const AppState& state = appState();
    QList<double> times, powers;
    const qsizetype first = activeWorkout_ == -1 ? 0 : activeWorkout_;
    bool firstEvent = true;
    for (qsizetype index = first; index < state.workoutList.size(); ++index) {
        const WorkoutEntry& entry = state.workoutList[index];
        double duration = 0;
        double power = 0;
        if (entry.type == WorkoutEntry::Type::Block && entry.block.type == BlockType::FreeRide) {
            duration = entry.block.seconds;
        } else if (entry.type == WorkoutEntry::Type::Block && entry.block.type == BlockType::Steady) {
            duration = entry.block.seconds;
            power = entry.block.power[activeDifficulty_];
        } else if (entry.type == WorkoutEntry::Type::RampStep) {
            duration = entry.stepSeconds;
            power = entry.stepPower[activeDifficulty_];
        } else if (entry.type == WorkoutEntry::Type::IntervalBlock && entry.block.type != BlockType::Ramp) {
            duration = entry.block.seconds;
            if (entry.block.type == BlockType::Steady) {
                power = entry.block.power[activeDifficulty_];
            }
        } else {
            // (interval and ramp headers: their entries after them are drawn)
            continue;
        }
        // the entry being ridden: what's left of it
        double remaining = duration;
        if (firstEvent) {
            remaining = duration - eventTimerSeconds_;
            firstEvent = false;
        }
        remaining = std::min(remaining, collectionPeriod);
        collectionPeriod -= remaining;
        if (times.isEmpty()) {
            times = {state.secondsPassed, state.secondsPassed + remaining};
        } else {
            times.append(times.back() + remaining);
        }
        powers.append(power);
        if (collectionPeriod <= 0) {
            break;
        }
    }
    return {times, powers};
}

QString WorkoutEngine::difficultyFtpText(int value) {
    const int ftp = appState().ftp;
    if (ftp == 0) {
        return QStringLiteral("FTP: --");
    }
    return QStringLiteral("FTP: %1").arg(ftp + static_cast<int>(ftp * value * 0.02));
}

}  // namespace awo

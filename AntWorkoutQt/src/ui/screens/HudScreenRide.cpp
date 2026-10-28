// The workout screen's ride (ClassHUDWindow.py's process_events and what it calls): each tick's workout events shown,
// the blinking, pausing and restarting, the graph, the sliders and the END prompt. Building the screen is HudScreen.cpp.

#include <QGraphicsScene>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "AnalogGauge.hpp"
#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "ConfirmEndWorkoutWindow.hpp"
#include "GradeWidgets.hpp"
#include "HudScreen.hpp"
#include "LongVLabels.hpp"
#include "Logger.hpp"
#include "MaxStar.hpp"
#include "MusicPlayer.hpp"
#include "PlotItems.hpp"
#include "PowerSpiral.hpp"
#include "PyCompat.hpp"
#include "SavedSettings.hpp"
#include "Session.hpp"
#include "SmoothProgressBar.hpp"
#include "SolidColoredColumns.hpp"
#include "SoundEffects.hpp"
#include "Theme.hpp"
#include "WorkoutGraph.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

const QString kWorkoutComplete = QStringLiteral("Workout Complete!\nCooldown, or press END for more options");
const QString kPressStart = QStringLiteral("Press Start to begin workout");
const QString kLastBlock = QStringLiteral("You're on the last block, way to go!");
const QString kRideAtStyle = QStringLiteral("font: 900 %1pt 'Orbitron'; background-color: rgba(0, 0, 0, 0);");
constexpr int kRideAtPoints = 35;
const QString kRideAtLongest = QStringLiteral("Ride At: 1000 Watts");
constexpr int kRideAtLines = 2;
const QString kMessageStyle = QStringLiteral("font: 700 %1pt 'Orbitron'; background-color: rgba(0, 0, 0, 0);");
constexpr int kMessagePoints = 22;
constexpr int kMessageLines = 3;
constexpr int kMessageGap = 12;

double steadySeconds(const QElapsedTimer& clock) {
    return clock.nsecsElapsed() / 1e9;
}

double maxOrZero(const QList<double>& values) {
    return values.isEmpty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

QString dimColor(const QString& hex) {
    const QColor color(hex);
    const QColor grey(QStringLiteral("#b4b5b8"));
    return QColor(static_cast<int>(py::round(color.red() + (grey.red() - color.red()) * 0.5)),
                  static_cast<int>(py::round(color.green() + (grey.green() - color.green()) * 0.5)),
                  static_cast<int>(py::round(color.blue() + (grey.blue() - color.blue()) * 0.5)))
        .name();
}

const SavedValues& savedValues(const QString& name) {
    const AppState& state = appState();
    if (name == u"hr") {
        return state.savedHRVals;
    }
    if (name == u"cadence") {
        return state.savedCadenceVals;
    }
    return state.savedPowerVals;
}

}  // namespace

// ---- The clock ----

void HudScreen::tick() {
    const double now = steadySeconds(clock_);
    const double delta = now - lastTimestamp_;
    lastTimestamp_ = now;
    processEvents(delta);
}

void HudScreen::processEvents(double timeDelta) {
    const AppState& state = appState();
    engine_->advanceClock(timeDelta);
    // (a simulated second, or the real clock: the next real tick counts from here)
    lastTimestamp_ = steadySeconds(clock_);
    layout::setText(timeElapsed_, minimizedDuration(state.secondsPassed));
    if (graphUpdates_) {
        hudGraphDataUpdate();
    }
    updatePowerRings();
    if (!state.workoutMode) {
        return;
    }
    updateRampPauseCountdown();
    // START blinks until it's pressed
    if (startButtonBlink_) {
        startButtonBlinkCounter_ += engine_->timeDelta();
        if (startButtonBlinkCounter_ > .3) {
            startButtonBlinkCounter_ = 0;
            startButtonBlinkOn_ = !startButtonBlinkOn_;
        }
    }
    setStartButtonStyle();
    // (Musical Cadence Mode: the workout moves on as far as the music has)
    showTickEvents(engine_->tick(cadenceMode_ ? musicSeconds() : std::nullopt));
    updateTrainerTarget();
    if (cadenceMode_) {
        updateSongTitle();
        recordSong();
    }
}

// (only restyled when the look changes: a stylesheet goes through the theme and re-polishes the button)
void HudScreen::setStartButtonStyle() {
    const QString style = startButtonBlink_ && startButtonBlinkOn_ ? startButtonBlinkOnStyle_ : startButtonBlinkOffStyle_;
    if (style != startButtonStyle_) {
        startButtonStyle_ = style;
        theme::setSheet(startEndButton_, style);
    }
}

void HudScreen::showTickEvents(const WorkoutEngine::TickEvents& events) {
    const AppState& state = appState();
    if (events.completed) {
        log::event(QStringLiteral("WORKOUT"), QStringLiteral("Workout complete at %1").arg(minimizedDuration(state.secondsPassed)));
        showWorkoutComplete(events);
    }
    if (events.running) {
        if (events.timersMoved) {
            updateSpiralProgress();
            recordSpiralSample();
        }
        const double blockDuration = engine_->blockDuration();
        const double blockSeconds = engine_->blockSecondsPassed();
        const double workoutSeconds = engine_->workoutSecondsPassed();
        timeProgressBar_->setValue(static_cast<int>(workoutSeconds * 100));
        currentBlockProgressBar_->setValue(static_cast<int>(blockSeconds * 100));
        layout::setText(timeBlockRemainingValue_,
                        QStringLiteral("Block time remaining: ") +
                            minimizedDuration(static_cast<int>(blockDuration - blockSeconds)));
        layout::setText(workoutTime_, minimizedDuration(workoutSeconds) + QStringLiteral(" / ") +
                                          minimizedDuration(state.workoutDurationSeconds));
        layout::setText(remainingWorkoutTime_,
                        QStringLiteral("Done in: ") + minimizedDuration(state.workoutDurationSeconds - workoutSeconds));
        if (events.blockStarted) {
            showBlock();
        } else if (events.intervalBlockStarted) {
            showIntervalBlock();
        }
        if (events.finishedGrade && !events.completed) {
            gradeOverlay_->showGrade(*events.finishedGrade, engine_->blockDuration());
        }
    }
    if (events.lengthChanged) {
        timeProgressBar_->setRange(0, 100 * state.workoutDurationSeconds);
    }
    if (events.powerChanged) {
        updateRequestedPowerDisplays();
    }
    if (events.running) {
        progressBlinkCheck();
        showEffortAverage();
    }
    updateWorkoutMessage();
    if (events.upNextChanged) {
        updateUpNextRemaining();
    }
    if (events.difficultyApplied) {
        showDifficultyLabels();
    }
}

// the end of the workout: the block labels to one, the grades in the left column, everything stopped where it is
void HudScreen::showWorkoutComplete(const WorkoutEngine::TickEvents& events) {
    for (QLabel* label : intervalLabels()) {
        label->hide();
    }
    blockMultiplierLabel_->hide();
    blockNonIntLabel_->show();
    workoutBlinkDefault();
    blockBlinkDefault();
    pauseButton_->setEnabled(false);
    sound::play(QStringLiteral("complete"));
    if (events.finishedGrade) {
        gradeOverlay_->showGrade(*events.finishedGrade);
    }
    if (events.showGrades) {
        showWorkoutGrades(true);
    }
    // (paused: "Paused" in Ride At, cleared below)
    showPaused();
    blockNonIntLabel_->setAlignment(Qt::AlignCenter);
    layout::setText(blockNonIntLabel_, kWorkoutComplete);
    setLabelColor(blockNonIntLabel_);
    layout::setText(workoutTime_, QStringLiteral("Done"));
    layout::setText(remainingWorkoutTime_, QString());
    layout::setText(timeBlockRemainingValue_, QStringLiteral("Workout Complete"));
    layout::setText(rideAtLabel_, QString());
    layout::setText(upNextLabelHeader_, QString());
    layout::setText(remainingWorkoutHeader_, QString());
    layout::setText(upNextLabelDescription_, QString());
    // (no light blue box: nothing is up next)
    theme::setSheet(upNextLabelDescription_, QString());
    remainingWorkoutList_->setFullText(QStringLiteral("DONE"));
    // the difficulty back to 0 and off (the volume stays as it's set)
    if (!cadenceMode_) {
        difficultySlider_->setValue(0);
        difficultySlider_->setEnabled(false);
    }
    currentBlockProgressBar_->setRange(0, 1);
    currentBlockProgressBar_->setValue(1);
    timeProgressBar_->setRange(0, 1);
    timeProgressBar_->setValue(1);
    const auto [along, length] = engine_->spiralBlockTime();
    spiralWorkoutProgress_->setProgress(appState().workoutDurationSeconds, appState().workoutDurationSeconds, along,
                                        length);
}

// a new block (or interval): its labels, the block timer, the spiral's block
void HudScreen::showBlock() {
    const QString unit = appState().targetUnit();
    for (QLabel* label : intervalLabels()) {
        label->hide();
    }
    blockMultiplierLabel_->hide();
    blockNonIntLabel_->show();
    const WorkoutEntry& entry = *engine_->currentEventText();
    const int difficulty = engine_->activeDifficulty();
    if (entry.type == WorkoutEntry::Type::Block) {
        // (a free ride's text is at the written difficulty: it has no target)
        const int shownDifficulty = entry.block.type == BlockType::FreeRide ? 10 : difficulty;
        layout::setText(blockNonIntLabel_, blockText(entry, shownDifficulty, unit).front());
        setLabelColor(blockNonIntLabel_);
    } else {
        blockNonIntLabel_->hide();
        const QStringList text = blockText(entry, difficulty, unit);
        layout::setText(blockMultiplierLabel_, text.front());
        blockMultiplierLabel_->show();
        QLabel* const* labels = entry.intervalBlocks.size() == 2 ? blockInt2Label_ : blockInt3Label_;
        for (qsizetype i = 0; i < entry.intervalBlocks.size(); ++i) {
            layout::setText(labels[i], text[i + 1]);
            setLabelColor(labels[i]);
            labels[i]->show();
            labels[i]->setEnabled(i == 0);
        }
        blockBlinkDefault();
    }
    // the new block's spiral (an interval's: all its reps)
    spiralWorkoutProgress_->setBlock(engine_->spiralBlockTime().second);
    layout::setText(timeBlockRemainingValue_, QStringLiteral("Block time remaining: ") +
                                                  minimizedDuration(static_cast<int>(engine_->blockDuration())));
    currentBlockProgressBar_->setRange(0, static_cast<int>(engine_->blockDuration() * 100));
    currentBlockProgressBar_->setValue(0);
}

// the interval's next block: its rep count, and its label the one lit
void HudScreen::showIntervalBlock() {
    const WorkoutEntry& settings = *engine_->currentEventSettings();
    // (a ramp's step follows its interval block: the rep count is the interval block's)
    const WorkoutEntry& block = settings.type == WorkoutEntry::Type::RampStep
                                    ? appState().workoutList[engine_->activeWorkout() - 1]
                                    : settings;
    layout::setText(blockMultiplierLabel_, QStringLiteral("%1x").arg(block.repeats));
    const int index = engine_->currentIntervalBlockIndex();
    const qsizetype count = engine_->currentEventText()->intervalBlocks.size();
    QLabel* const* labels = count == 2 ? blockInt2Label_ : blockInt3Label_;
    for (qsizetype i = 0; i < count; ++i) {
        labels[i]->setEnabled(i == index);
    }
    layout::setText(timeBlockRemainingValue_, QStringLiteral("Block time remaining: ") +
                                                  minimizedDuration(static_cast<int>(engine_->blockDuration())));
    currentBlockProgressBar_->setRange(0, static_cast<int>(engine_->blockDuration() * 100));
    currentBlockProgressBar_->setValue(0);
    blockBlinkDefault();
}

// a difficulty set: the block's labels at it
void HudScreen::showDifficultyLabels() {
    const QString unit = appState().targetUnit();
    const WorkoutEntry& entry = *engine_->currentEventText();
    const int difficulty = engine_->activeDifficulty();
    if (entry.type == WorkoutEntry::Type::Block) {
        layout::setText(blockNonIntLabel_, blockText(entry, difficulty, unit).front());
        setLabelColor(blockNonIntLabel_);
        return;
    }
    const QStringList text = blockText(entry, difficulty, unit);
    QLabel* const* labels = entry.intervalBlocks.size() == 2 ? blockInt2Label_ : blockInt3Label_;
    for (qsizetype i = 0; i < entry.intervalBlocks.size(); ++i) {
        layout::setText(labels[i], text[i + 1]);
        setLabelColor(labels[i]);
    }
}

// ---- Between ticks ----

void HudScreen::updateSmoothBars() {
    if (cadenceMode_) {
        updateCenterRings();
    }
    // only while the timer's running: paused, before START and once finished the bars hold where they were set
    if (!(engine_->unpaused() && engine_->started()) || engine_->newWorkoutStart()) {
        return;
    }
    // since the last tick, capped at a tick and a half so a stalled tick can't run the bars ahead
    double sinceTick = std::min(steadySeconds(clock_) - lastTimestamp_, 0.15);
    if (const std::optional<double> music = cadenceMode_ ? musicSeconds() : std::nullopt) {
        // how far the music has got past the last tick
        sinceTick = std::min(std::max(0.0, *music - engine_->workoutSecondsPassed()), 0.15);
    }
    const AppState& state = appState();
    if (state.workoutDurationSeconds > 0) {
        timeProgressBar_->setFraction((engine_->workoutSecondsPassed() + sinceTick) / state.workoutDurationSeconds);
    }
    if (engine_->blockDuration() > 0) {
        currentBlockProgressBar_->setFraction((engine_->blockSecondsPassed() + sinceTick) / engine_->blockDuration());
        if (state.workoutDurationSeconds > 0) {
            updateSpiralProgress(sinceTick);
        }
    }
    progressBlinkCheck(sinceTick);
}

// (sinceTick: how far past the last tick to draw, so the spiral moves between ticks)
void HudScreen::updateSpiralProgress(double sinceTick) {
    if (!engine_->currentEventText()) {
        return;
    }
    const auto [along, length] = engine_->spiralBlockTime(sinceTick);
    spiralWorkoutProgress_->setProgress(engine_->workoutSecondsPassed() + sinceTick, appState().workoutDurationSeconds,
                                        along, length);
}

// the power and cadence being ridden (each a short average) into the spiral's ring and spiral
void HudScreen::recordSpiralSample() {
    const AppState& state = appState();
    std::optional<double> power = recentAverage(state.savedPowerVals);
    // (no cadence to show: the spiral's lines at the lanes' outer edges)
    std::optional<double> cadence;
    if (state.showsCadence()) {
        cadence = recentAverage(state.savedCadenceVals, kSpiralCadenceAverageSeconds);
        // the cadence sensor gone quiet: the outer edge while the power's still coming, else not pedaling
        if (!cadence && !power) {
            cadence = 0;
        }
    }
    spiralWorkoutProgress_->addSample(engine_->workoutSecondsPassed(), engine_->spiralBlockTime().first,
                                      power.value_or(0), cadence);
}

// the average of the readings in the last seconds, none after grading's dropout time without one
std::optional<double> HudScreen::recentAverage(const SavedValues& saved, double seconds) const {
    const double now = appState().secondsPassed;
    if (!grading::latestPower(saved.times, saved.values, now)) {
        return std::nullopt;
    }
    double total = 0;
    int count = 0;
    for (qsizetype i = saved.times.size() - 1; i >= 0; --i) {
        if (now - saved.times[i] > seconds) {
            break;
        }
        total += saved.values[i];
        ++count;
    }
    return count ? total / count : saved.values.back();
}

// ---- Blinking ----

// The workout timer blinks through the workout's last kBlinkWindowSeconds and the block timer through each block's.
// On or off comes straight from the time left (on the first half period, off the next...), so the rhythm stays even
// however often it's called; between ticks sinceTick keeps each switch on time. Shorter than kBlinkMinDurationSeconds:
// no blinking.
void HudScreen::progressBlinkCheck(double sinceTick) {
    const double workoutLeft =
        appState().workoutDurationSeconds - (engine_->workoutSecondsPassed() + sinceTick);
    const double blockLeft = engine_->blockDuration() - (engine_->blockSecondsPassed() + sinceTick);
    workoutEnding_ = workoutLeft < kBlinkWindowSeconds;
    const bool workoutOn =
        appState().workoutDurationSeconds >= kBlinkMinDurationSeconds && blinkPhaseOn(workoutLeft);
    const bool blockOn = engine_->blockDuration() >= kBlinkMinDurationSeconds && blinkPhaseOn(blockLeft);
    if (workoutOn != blinkWorkout_) {
        workoutOn ? workoutBlinkOn() : workoutBlinkDefault();
    }
    if (blockOn != blinkBlock_) {
        blockOn ? blockBlinkOn() : blockBlinkDefault();
    }
    // one spiral state for both blinks, the workout's winning while the workout is ending
    const QString state = workoutOn                       ? QStringLiteral("workout")
                          : blockOn && !workoutEnding_ ? QStringLiteral("block")
                                                       : QStringLiteral("default");
    if (state != spiralBlinkState_) {
        spiralBlinkState_ = state;
        spiralWorkoutProgress_->setBlink(state == u"workout" ? PowerSpiral::Blink::Workout
                                         : state == u"block" ? PowerSpiral::Blink::Block
                                                             : PowerSpiral::Blink::Default);
    }
}

bool HudScreen::blinkPhaseOn(double secondsRemaining) const {
    if (!(0 < secondsRemaining && secondsRemaining < kBlinkWindowSeconds)) {
        return false;
    }
    return static_cast<int>(secondsRemaining / kBlinkHalfPeriodSeconds) % 2 == 1;
}

void HudScreen::workoutBlinkDefault() {
    blinkWorkout_ = false;
    setWorkoutTimerColors(QStringLiteral("#1f15e1"), QStringLiteral("#bdd6de"), QStringLiteral("#9fc5d1"));
}

void HudScreen::workoutBlinkOn() {
    blinkWorkout_ = true;
    setWorkoutTimerColors(QStringLiteral("#1f15e1"), QStringLiteral("#dadbde"), QStringLiteral("#dadbde"));
}

// (the workout timer labels are rounded, so every recolor carries the radius too; the bar stays square)
void HudScreen::setWorkoutTimerColors(const QString& chunk, const QString& bar, const QString& label) {
    timeProgressBar_->setBarColors(QColor(chunk), QColor(bar));
    const QString style = QStringLiteral("background-color:%1; border-radius: %2px;").arg(label).arg(timerRadius_);
    for (QLabel* timer : {workoutTimeHeader_, workoutTime_, remainingWorkoutTime_}) {
        theme::setSheet(timer, style);
    }
}

void HudScreen::blockBlinkDefault() {
    blinkBlock_ = false;
    currentBlockProgressBar_->setBarColors(QColor(QStringLiteral("#1f15e1")), QColor(QStringLiteral("#bdd6de")));
    theme::setSheet(timeBlockRemainingValue_, QStringLiteral("background-color:#9fc5d1"));
    theme::setSheet(blockMultiplierLabel_, QStringLiteral("background-color:#9fc5d1"));
    setLabelColor(blockNonIntLabel_);
    for (QLabel* label : intervalLabels()) {
        setLabelColor(label);
    }
}

void HudScreen::blockBlinkOn() {
    blinkBlock_ = true;
    const QString lit = QStringLiteral("background-color:#dadbde");
    currentBlockProgressBar_->setBarColors(QColor(QStringLiteral("#1f15e1")), QColor(QStringLiteral("#dadbde")));
    theme::setSheet(blockNonIntLabel_, lit);
    theme::setSheet(timeBlockRemainingValue_, lit);
    const int index = engine_->currentIntervalBlockIndex();
    const QString repeats = blockMultiplierLabel_->text();
    if (index == 0) {
        theme::setSheet(blockInt2Label_[0], lit);
        theme::setSheet(blockInt3Label_[0], lit);
    } else if (index == 1) {
        theme::setSheet(blockInt2Label_[1], lit);
        theme::setSheet(blockInt3Label_[1], lit);
        const auto& entry = engine_->currentEventText();
        if (entry && entry->intervalBlocks.size() == 2 && repeats == u"1x") {
            theme::setSheet(blockMultiplierLabel_, lit);
        }
    } else if (index == 2) {
        theme::setSheet(blockInt3Label_[2], lit);
        if (repeats == u"1x") {
            theme::setSheet(blockMultiplierLabel_, lit);
        }
    }
}

// ---- Labels ----

// a block label's colors in its own sheet (it beats the screen's QLabel:disabled rule, so it carries its own: an
// interval's blocks not being ridden dim, their background halfway to grey)
QString HudScreen::labelColor(const QString& text) const {
    const QString css = labelColorCss(text);
    static const QRegularExpression background(QStringLiteral("background-color:[^;]*;"));
    static const QRegularExpression hex(QStringLiteral("#[0-9a-fA-F]{6}"));
    QString dimmed;
    const QRegularExpressionMatch match = background.match(css);
    if (match.hasMatch()) {
        dimmed = match.captured(0);
        QString replaced;
        qsizetype last = 0;
        auto it = hex.globalMatch(dimmed);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            replaced += dimmed.mid(last, m.capturedStart() - last) + dimColor(m.captured(0));
            last = m.capturedEnd();
        }
        dimmed = replaced + dimmed.mid(last);
    }
    return QStringLiteral("QLabel{%1} QLabel:disabled{color:#6a6a6a; %2}").arg(css, dimmed);
}

QString HudScreen::labelColorCss(const QString& text) const {
    if (text.isEmpty() || text == kLastBlock || text == kPressStart || text == kWorkoutComplete) {
        return QStringLiteral("background-color: #9fc5d1;");
    }
    // (Up Next can be a block with the song it starts under it: colored as the block)
    if (isSongHeader(text.split(u'\n').front(), appState().cadenceMode)) {
        return kSongHeaderCss;
    }
    const auto targets = targetsFromText(text);
    if (!targets) {
        // free ride
        return zones::freeRideCss();
    }
    return zones::blockCss(targets->first, targets->second);
}

void HudScreen::setLabelColor(QLabel* label) {
    theme::setSheet(label, labelColor(label->text()));
}

// the up next box and the coming up list, at the difficulty set
void HudScreen::updateUpNextRemaining() {
    QList<WorkoutEngine::UpcomingLine> lines = engine_->upcoming();
    if (lines.isEmpty()) {
        layout::setText(upNextLabelDescription_, kLastBlock);
        setLabelColor(upNextLabelDescription_);
        // (the list shows "Almost done!": nothing is coming up)
        layout::setText(remainingWorkoutHeader_, QString());
        return;
    }
    QString upNext;
    if (lines.front().size() == 1 && isSongHeader(lines.front().front(), appState().cadenceMode) && lines.size() > 1) {
        // a new song next: its first block, with the song under it
        upNext = lines[1].front() + u'\n' + lines.front().front();
        lines.removeFirst();
    } else if (lines.front().size() > 1) {
        // an interval
        upNext = lines.front().front() + u' ' + lines.front().mid(1).join(QStringLiteral("\n      "));
    } else {
        upNext = lines.front().front();
    }
    layout::setText(upNextLabelDescription_, upNext);
    setLabelColor(upNextLabelDescription_);
    lines.removeFirst();
    QString comingUp;
    for (const WorkoutEngine::UpcomingLine& line : lines) {
        if (line.size() > 1) {
            comingUp += line.front() + u' ' + line.mid(1).join(QStringLiteral("\n      ")) + u'\n';
        } else {
            comingUp += line.front() + u'\n';
        }
    }
    // (an empty list shows "Almost done!": nothing is "Coming up...")
    layout::setText(remainingWorkoutHeader_, comingUp.isEmpty() ? QString() : QStringLiteral("Coming up..."));
    remainingWorkoutList_->setFullText(comingUp);
}

// the target arc at the power (or cadence) asked for, and Ride At
void HudScreen::updateRequestedPowerDisplays() {
    if (appState().workoutMode) {
        currentPower_ = engine_->currentPower();
    }
    const auto [c0, c1] = zones::gaugeArc(currentPower_);
    targetGauge_->gauge()->setArc(currentPower_, {{0, QColor(QStringLiteral("#88") + c0)},
                                                  {.5, QColor(QStringLiteral("#88") + c1)}});
    layout::setText(rideAtLabel_, currentPower_ == 0 ? QStringLiteral("Free Ride")
                                                     : QStringLiteral("Ride At: %1 %2").arg(currentPower_).arg(
                                                           appState().targetUnit()));
    // (the spiral colors itself from the power ridden: redrawn for new zone colors)
    if (spiralWorkoutProgress_) {
        spiralWorkoutProgress_->update();
    }
}

void HudScreen::updateWorkoutMessage() {
    const QString text = engine_->message();
    if (workoutMessageLabel_->text() != text) {
        setWorkoutMessage(text);
    }
}

void HudScreen::setWorkoutMessage(const QString& text) {
    layout::setText(workoutMessageLabel_, text);
    fitWorkoutMessage();
    // (again once the new text has been laid out)
    messageFitTimer_->start();
}

// Ride At and the message under it centered on the power gauge and clear of the gauges either side (whichever show):
// their text wraps to stay between them, and only if it still needs more lines than it has does the font shrink
void HudScreen::fitWorkoutMessage() {
    fitBetweenGauges(rideAtLabel_, kRideAtStyle, kRideAtPoints, kRideAtLines, kRideAtLongest);
    fitBetweenGauges(workoutMessageLabel_, kMessageStyle, kMessagePoints, kMessageLines, workoutMessageLabel_->text());
}

void HudScreen::fitBetweenGauges(QLabel* label, const QString& style, int points, int lines, const QString& text) {
    const QRect labelRect(label->mapTo(this, QPoint(0, 0)), label->size());
    // centered on the power gauge (the cadence gauge in Musical Cadence Mode), else on the label's own space
    int center = labelRect.center().x();
    Gauge* middle = targetGauge_ ? targetGauge_ : wattGaugeDisplay_;
    if (middle && middle->isVisible()) {
        center = QRect(middle->mapTo(this, QPoint(0, 0)), middle->size()).center().x();
    }
    int left = 0, right = width();
    for (Gauge* gauge : {wattGaugeDisplay_, cadenceGaugeDisplay_, hrGaugeDisplay_}) {
        if (!gauge || gauge == middle || !gauge->isVisible()) {
            continue;
        }
        const QRect rect(gauge->mapTo(this, QPoint(0, 0)), gauge->size());
        if (rect.bottom() < labelRect.top() || rect.top() > labelRect.bottom()) {
            continue;
        }
        if (rect.center().x() < center) {
            left = std::max(left, rect.right());
        } else {
            right = std::min(right, rect.left());
        }
    }
    // the same room either side of the center, so the text stays centered on it
    const int half = std::max(20, std::min({center - (left + kMessageGap), (right - kMessageGap) - center,
                                            center - labelRect.left(), labelRect.right() - center}));
    const int marginLeft = std::max(0, center - half - labelRect.left());
    const int marginRight = std::max(0, labelRect.right() - (center + half));
    label->setContentsMargins(marginLeft, 0, marginRight, 0);
    const int textWidth = std::max(40, label->width() - marginLeft - marginRight);
    theme::setSheet(label, style.arg(points));
    while (!text.isEmpty() && points > 6) {
        const QFontMetrics metrics = label->fontMetrics();
        const QRect needed = metrics.boundingRect(QRect(0, 0, textWidth, 100000), Qt::TextWordWrap, text);
        if (needed.height() <= lines * metrics.lineSpacing() && needed.width() <= textWidth) {
            break;
        }
        --points;
        theme::setSheet(label, style.arg(points));
    }
}

void HudScreen::showWorkoutGrades(bool show) {
    workoutGraded_ = show;
    for (QWidget* widget : std::initializer_list<QWidget*>{upNextLabelHeader_, upNextLabelDescription_,
                                                           remainingWorkoutHeader_, remainingWorkoutList_}) {
        widget->setVisible(!show);
    }
    workoutGradesHeader_->setVisible(show);
    workoutGradeList_->setVisible(show);
    if (show) {
        workoutGradeList_->refresh();
    }
}

// the spiral's middle through an FTP test's effort: the FTP its average works out to
void HudScreen::showEffortAverage() {
    if (const auto display = engine_->effortDisplay()) {
        gradeOverlay_->showValue(display->first, display->second);
    }
}

void HudScreen::updateRampPauseCountdown() {
    if (const auto text = engine_->rampPauseText(); text && layout::text(pauseButton_) != *text) {
        layout::setText(pauseButton_, *text);
    }
}

// ---- START, PAUSE, restart, END ----

void HudScreen::startWorkout() {
    layout::setText(startEndButton_, QStringLiteral("END"));
    pauseButton_->setEnabled(true);
    // (FTP tests don't get one: the test sets the power; Musical Cadence Mode's volume slider is already live)
    difficultySlider_->setEnabled(cadenceMode_ || !engine_->ftpTest());
    startedAt_ = steadySeconds(clock_);
    startButtonBlink_ = false;
    const AppState& state = appState();
    log::event(QStringLiteral("WORKOUT"),
               QStringLiteral("Started: %1%2").arg(state.workoutMode ? state.workoutDisplayName() : QStringLiteral("free ride"),
                                                   cadenceMode_ ? QStringLiteral(" (Musical Cadence Mode)") : QString()));
    // the trainer follows the workout from here on
    trainerCommanded_ = true;
    engine_->start();
    // Musical Cadence Mode: an FE-C trainer is left free (no target power), and the music starts
    if (cadenceMode_) {
        if (bridge_ && bridge_->trainerSelected()) {
            bridge_->releaseTrainerPower();
        }
        musicStart();
    }
}

void HudScreen::pauseRecording() {
    if (!engine_->started()) {
        return;
    }
    const bool restarted = engine_->pauseRecording();
    musicPause(!engine_->unpaused());
    showPaused();
    if (engine_->unpaused() && restarted) {
        timeProgressBar_->setRange(0, 100 * appState().workoutDurationSeconds);
    }
}

void HudScreen::showPaused() {
    if (engine_->unpaused()) {
        pauseButton_->setDown(false);
        updateRequestedPowerDisplays();
        if (layout::text(pauseButton_) != u"PAUSE") {
            layout::setText(pauseButton_, QStringLiteral("PAUSE"));
        }
    } else {
        pauseButton_->setDown(true);
        updateRampPauseCountdown();
        layout::setText(rideAtLabel_, QStringLiteral("Paused"));
        // the target arc off
        targetGauge_->gauge()->setArc(0, targetGauge_->gauge()->newArcColors);
    }
}

void HudScreen::spacePause() {
    // (not straight after START: a held or doubled Space that started the workout doesn't pause it too)
    if (steadySeconds(clock_) - startedAt_ >= kSpacePauseGuardSeconds) {
        pauseButton_->click();
    }
}

bool HudScreen::endAvailable() const {
    // (its text as set: shown as a symbol, the button itself has none)
    return layout::text(startEndButton_) == u"END";
}

void HudScreen::restartWorkout() {
    musicStop();
    const AppState& state = appState();
    if (state.workoutMode) {
        layout::setText(startEndButton_, QStringLiteral("START"));
        // (let back up too: pausing, the END prompt's included, held it down)
        pauseButton_->setEnabled(false);
        pauseButton_->setDown(false);
        startButtonBlink_ = true;
        if (!cadenceMode_) {
            difficultySlider_->setValue(0);
            difficultySlider_->setEnabled(false);
        }
        engine_->restart();
        timeProgressBar_->setRange(0, 100 * state.workoutDurationSeconds);
        layout::setText(upNextLabelHeader_, QStringLiteral("Up Next:"));
        layout::setText(remainingWorkoutHeader_, QStringLiteral("Coming up..."));
        layout::setText(blockNonIntLabel_, kPressStart);
        blockNonIntLabel_->show();
        blockMultiplierLabel_->hide();
        for (QLabel* label : intervalLabels()) {
            label->hide();
        }
        blockBlinkDefault();
        currentBlockProgressBar_->setValue(0);
        layout::setText(timeBlockRemainingValue_, QString());
        workoutBlinkDefault();
        spiralBlinkState_ = QStringLiteral("default");
        timeProgressBar_->setValue(0);
        layout::setText(workoutTime_, minimizedDuration(engine_->workoutSecondsPassed()) + QStringLiteral(" / ") +
                                          minimizedDuration(state.workoutDurationSeconds));
        layout::setText(remainingWorkoutTime_,
                        QStringLiteral("Done in: ") + minimizedDuration(state.workoutDurationSeconds));
        updateRequestedPowerDisplays();
        layout::setText(rideAtLabel_, QString());
        layout::setText(workoutMessageLabel_, QString());
        updateUpNextRemaining();
        pastPowerIndex_ = 0;
        spiralWorkoutProgress_->clear(state.workoutDurationSeconds, session::ringHistory());
        gradeOverlay_->clear();
        showWorkoutGrades(false);
        layout::setText(pauseButton_, QStringLiteral("PAUSE"));
    }
    layout::setText(timeElapsed_, QStringLiteral("0s"));
}

void HudScreen::endGrading() {
    engine_->endGrading();
}

void HudScreen::confirmEndWorkout() {
    delete endPrompt_;
    endPrompt_ = new ConfirmEndWorkoutWindow(this);
    endPrompt_->show();
}

bool HudScreen::escapePressed() {
    if (!isVisible() || !endAvailable()) {
        return false;
    }
    if (endPrompt_ && endPrompt_->isVisible()) {
        endPrompt_->raise();
        endPrompt_->activateWindow();
    } else {
        confirmEndWorkout();
    }
    return true;
}

// ---- The trainer ----

// workout mode: the trainer follows Ride At while the workout runs, released whenever that's above the Power Drive
// Cutoff, at 0 while paused or once complete
void HudScreen::updateTrainerTarget() {
    if (!trainerControl_ || !trainerCommanded_ || cadenceMode_ || !bridge_) {
        return;
    }
    if (engine_->started() && engine_->unpaused()) {
        const int cutoff = appState().powerDriveCutoff;
        if (cutoff && engine_->currentPower() > cutoff) {
            bridge_->releaseTrainerPower();
        } else {
            bridge_->setTrainerTargetPower(engine_->currentPower());
        }
    } else {
        bridge_->setTrainerTargetPower(0);
    }
}

// ---- The slider ----

void HudScreen::difficultyChanged(int value) {
    // 2% of FTP a step; the label's font stays, only its color changes
    layout::setText(difficultyPercentage_, WorkoutEngine::difficultyFtpText(value));
    const QString color = value == 0 ? QStringLiteral("green") : value > 0 ? QStringLiteral("red") : QStringLiteral("blue");
    theme::setSheet(difficultyPercentage_, difficultyTextStyle_ + QStringLiteral(" color: %1;").arg(color));
    engine_->setDifficulty(value);
}

// Musical Cadence Mode's volume, 0-100% (squared, so each step sounds about as big as the next), saved for next time
void HudScreen::volumeChanged(int value) {
    layout::setText(difficultyPercentage_, QStringLiteral("%1%").arg(value));
    musicVolume_ = std::pow(value / 100.0, 2);
    if (musicPlayer_) {
        musicPlayer_->volume = musicVolume_;
    }
    musicVolumePercent_ = value;
    settings::update({{QStringLiteral("musicVolume"), value}});
}

// free mode's Watts slider: the FE-C trainer's target power
void HudScreen::trainerWattsChanged(int value) {
    trainerWattsText_->setText(QString::number(value));
    currentPower_ = value;
    updateRequestedPowerDisplays();
    if (bridge_) {
        bridge_->setTrainerTargetPower(value);
    }
}

// typed watts move the slider (which sets the trainer); anything not a whole number in range goes back
void HudScreen::trainerWattsEdited() {
    const QString text = trainerWattsText_->text().trimmed();
    if (py::isDigits(text) && difficultySlider_->minimum() <= text.toInt() && text.toInt() <= difficultySlider_->maximum()) {
        difficultySlider_->setValue(text.toInt());
    }
    trainerWattsText_->setText(QString::number(difficultySlider_->value()));
}

// the wheel/Up/Down: steps of 2% FTP, or 5% volume or (free mode) 5 W, landing on a whole step or (free mode) one of
// the slider's stops; nothing while it's off
void HudScreen::nudgeSlider(int steps) {
    if (!steps || !difficultySlider_ || !difficultySlider_->isEnabled() || !isVisible()) {
        return;
    }
    const int step = cadenceMode_ || !appState().workoutMode ? 5 : 1;
    int value = difficultySlider_->value();
    for (int i = 0; i < std::abs(steps); ++i) {
        int next = 0;
        if (steps > 0) {
            next = (py::mod(value, step) == 0 ? value / step + 1 : static_cast<int>(std::floor(value / double(step))) + 1) * step;
            for (const int stop : sliderStops_) {
                if (stop > value) {
                    next = std::min(next, stop);
                }
            }
        } else {
            next = -(static_cast<int>(std::floor(-value / double(step))) + 1) * step;
            for (const int stop : sliderStops_) {
                if (stop < value) {
                    next = std::max(next, stop);
                }
            }
        }
        value = std::max(difficultySlider_->minimum(), std::min(next, difficultySlider_->maximum()));
    }
    difficultySlider_->setValue(value);
}

// a step a wheel notch (120); touchpads send smaller steps that add up
void HudScreen::sliderWheel(QWheelEvent* event) {
    sliderWheelRemainder_ += event->angleDelta().y();
    const int steps = sliderWheelRemainder_ / 120;
    if (steps) {
        sliderWheelRemainder_ -= steps * 120;
        nudgeSlider(steps);
    }
    event->accept();
}

void HudScreen::wheelEvent(QWheelEvent* event) {
    // (from anywhere on the screen that doesn't scroll itself: the block and grade lists do)
    if (difficultySlider_) {
        sliderWheel(event);
    } else {
        QWidget::wheelEvent(event);
    }
}

bool HudScreen::eventFilter(QObject* watched, QEvent* event) {
    if (watched == rideAtWidget_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        messageFitTimer_->start();
    }
    if (event->type() == QEvent::Wheel && watched == difficultySlider_) {
        sliderWheel(static_cast<QWheelEvent*>(event));
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void HudScreen::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // (once the layout has placed everything at the new size)
    messageFitTimer_->start();
}

void HudScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // (built and fitted while hidden, before the layout gave everything its real place)
    messageFitTimer_->start();
}

// ---- Zone colors ----

void HudScreen::gaugeChooser() {
    // each zone fading in where the needle points at its start, for any FTP
    wattGaugeDisplay_->gauge()->setValueColors(zones::zoneStops(zones::powerRanges()));
    if (cadenceMode_ && cadenceGaugeDisplay_) {
        cadenceGaugeDisplay_->gauge()->setValueColors(zones::zoneStops(appState().ftpRanges));
    }
}

void HudScreen::refreshFtpColors() {
    gaugeChooser();
    powerRings_.recolor();
    updatePowerRings();
    if (targetGauge_ && targetGauge_->gauge()->arcCenterValue) {
        const auto [c0, c1] = zones::gaugeArc(currentPower_);
        targetGauge_->gauge()->setArc(targetGauge_->gauge()->arcCenterValue,
                                      {{0, QColor(QStringLiteral("#88") + c0)}, {.5, QColor(QStringLiteral("#88") + c1)}});
    }
    // a workout's screen (not free ride's)
    if (upNextLabelDescription_) {
        spiralWorkoutProgress_->update();
        QList<QLabel*> labels{upNextLabelDescription_};
        // (a blinking block gets its colors back when it stops)
        if (!blinkBlock_) {
            labels.append(blockNonIntLabel_);
            labels += intervalLabels();
        }
        for (QLabel* label : labels) {
            setLabelColor(label);
        }
        workoutPowerPlot_->recolor();
        workoutPowerCompletedPlot_->recolor();
    }
}

// Musical Cadence Mode's rings: the band's color at the target cadence (so they match the band's tip when riding on
// target; free ride's color with no target), brightened on the music's beat (or at level, given)
void HudScreen::updateCenterRings(std::optional<double> level) {
    if (!cadenceGaugeDisplay_) {
        return;
    }
    if (!level) {
        level = 0.0;
        const std::optional<double> seconds =
            engine_->started() && engine_->unpaused() ? musicSeconds() : std::nullopt;
        const QList<double>& pulses = appState().musicPulses;
        if (seconds) {
            const qsizetype index = std::upper_bound(pulses.begin(), pulses.end(), *seconds) - pulses.begin() - 1;
            if (index >= 0) {
                level = std::max(0.0, 1 - (*seconds - pulses[index]) / kBeatSeconds);
            }
        }
    }
    const QColor color(QStringLiteral("#") + zones::gaugeRing(currentPower_));
    cadenceGaugeDisplay_->gauge()->setCenterRings(QList<QColor>{color.darker(130), color}, level.value_or(0.0));
}

void HudScreen::updatePowerRings() {
    powerRings_.update(wattGaugeDisplay_);
}

// ---- The graph ----

void HudScreen::hudGraphDataUpdate() {
    AppState& state = appState();
    const double window = state.graphSecondsShown - state.graphBreakpoint;
    double diff = 0;
    if (state.workoutMode) {
        // the workout's coming targets, 4 to 12 minutes of them depending on how far it's got
        const double period = std::max(static_cast<double>(state.graphBreakpoint), state.graphSecondsShown - state.secondsPassed);
        std::tie(timeGraphList_, powerGraphList_) = engine_->upcomingGraph(period);
        // the targets ridden, as far back as fits before the breakpoint
        if (state.secondsPassed > window) {
            while (pastPowerIndex_ + 1 < state.workoutPowerRequestsSeconds.size()) {
                if (state.secondsPassed - state.workoutPowerRequestsSeconds[pastPowerIndex_ + 1] > window) {
                    ++pastPowerIndex_;
                } else {
                    break;
                }
            }
        }
    }
    // one formula for the x window the whole ride (clamped to 0 before the breakpoint), so it never snaps
    diff = std::max(0.0, state.secondsPassed - window);
    progressGraph_->setXRange(diff, diff + state.graphSecondsShown);
    nowLine_->setValue(state.secondsPassed);
    updateMaxStars(diff);

    // each y axis up to its highest visible data (the targets are on the cadence axis in Musical Cadence Mode)
    const double maxTarget = std::max(maxOrZero(powerGraphList_),
                                      maxOrZero(state.workoutPowerRequestsWatts.mid(pastPowerIndex_)));
    const double maxPower = maxOrZero(state.savedPowerVals.values);
    double powerMax = cadenceMode_ ? maxPower : std::max(maxTarget, maxPower);
    if (powerMax == 0) {
        powerMax = 150;
    }
    double hrCadenceMax = std::max(maxOrZero(state.savedHRVals.values), maxOrZero(state.savedCadenceVals.values));
    if (cadenceMode_) {
        hrCadenceMax = std::max(hrCadenceMax, maxTarget);
    }
    if (hrCadenceMax == 0) {
        hrCadenceMax = 190;
    }
    graphTops_ = {{QStringLiteral("power"), {powerMax + 100, 0}}, {QStringLiteral("hrCadence"), {hrCadenceMax + 20, 0}}};
    fitGraphTops();
    if (state.workoutMode) {
        const ftpcolors::ZoneRanges ranges = state.ftpRanges;
        auto colorer = [ranges](ftpcolors::GraphState graphState) {
            return [ranges, graphState](double value, const QList<double>&) {
                return zones::graphColor(value, ranges, graphState);
            };
        };
        // the targets coming up: until the workout's started (or while paused) they move along after the time now,
        // grey and nearly see-through
        workoutPowerPlot_->setData(timeGraphList_, powerGraphList_, ranges.limits,
                                   colorer(engine_->unpaused() ? ftpcolors::GraphState::Upcoming
                                                               : ftpcolors::GraphState::Paused));
        // the oldest one ridden starts no earlier than the curves' cutoff, so no bars show left of where they end
        QList<double> completedSeconds = state.workoutPowerRequestsSeconds.mid(pastPowerIndex_);
        if (!completedSeconds.isEmpty()) {
            completedSeconds.front() = std::max(completedSeconds.front(), diff);
        }
        workoutPowerCompletedPlot_->setData(completedSeconds, state.workoutPowerRequestsWatts.mid(pastPowerIndex_),
                                            ranges.limits, colorer(ftpcolors::GraphState::Past));
    }
    // the curves (power, heart rate, cadence) from the readings; brief dropouts drawn straight across, long ones as 0
    plotSavedVals(QStringLiteral("power"), powerProducedPlot_, ftpcolors::kSensorPower);
    if (state.antHRM) {
        plotSavedVals(QStringLiteral("hr"), hrPlot_, ftpcolors::kSensorHr);
    }
    if (state.showsCadence()) {
        plotSavedVals(QStringLiteral("cadence"), cadencePlot_, ftpcolors::kSensorCadence);
    }
    zeroGaugesOnDropout();
}

// a sensor's curve and its current reading diamond, only redone when there's something new: a reading, or through a
// dropout the zero line growing
void HudScreen::plotSavedVals(const QString& savedName, plot::CurveItem* curve, const QString& color) {
    const AppState& state = appState();
    const SavedValues& saved = savedValues(savedName);
    const bool dropout = !saved.times.isEmpty() && state.secondsPassed - saved.times.back() > kDropoutZeroSeconds;
    const QString key = QStringLiteral("%1;%2;%3;%4")
                            .arg(saved.times.size())
                            .arg(saved.times.isEmpty() ? 0.0 : saved.times.front(), 0, 'g', 17)
                            .arg(saved.times.isEmpty() ? 0.0 : saved.times.back(), 0, 'g', 17)
                            .arg(dropout ? state.secondsPassed : -1.0, 0, 'g', 17);
    if (plottedKeys_.value(savedName) == key) {
        return;
    }
    plottedKeys_.insert(savedName, key);
    const auto [xs, ys] = withDropoutsAsZero(saved.times, saved.values, state.secondsPassed);
    // (a halo under the line: it shows over any bar or background)
    const auto [pen, halo] = zones::sensorPens(color);
    curve->setData(xs, ys, pen, halo);
    for (const auto& [marker, name] : currentMarkers_) {
        if (name == savedName) {
            if (xs.isEmpty()) {
                marker->clear();
            } else {
                marker->setData({xs.back()}, {ys.back()});
            }
        }
    }
}

// each sensor's star to its highest reading on the graph (from start on): the latest of equal ones, unless the one
// it's over still is one
void HudScreen::updateMaxStars(double start) {
    const double now = steadySeconds(starClock_);
    for (const auto& [star, name] : maxStars_) {
        const SavedValues& saved = savedValues(name);
        const auto first = std::lower_bound(saved.times.begin(), saved.times.end(), start) - saved.times.begin();
        star->onGraph = first < saved.times.size();
        if (!star->onGraph) {
            continue;
        }
        const double top = *std::max_element(saved.values.begin() + first, saved.values.end());
        bool stays = false;
        if (star->x && *star->x >= start && star->y == top) {
            // (still a reading there: the saved values start over on a restart)
            const auto at = std::lower_bound(saved.times.begin(), saved.times.end(), *star->x) - saved.times.begin();
            stays = at < saved.times.size() && saved.times[at] == *star->x && saved.values[at] == top;
        }
        if (!stays) {
            qsizetype index = first;
            for (qsizetype i = first; i < saved.times.size(); ++i) {
                if (saved.values[i] >= saved.values[index]) {
                    index = i;
                }
            }
            star->goTo(saved.times[index], saved.values[index], now);
        }
        // clear of the current reading's diamond too while that's its max
        star->markerReach = star->x == saved.times.back() ? kCurrentMarkerReach : 0.0;
    }
}

// each y range up to its top, or higher where the stars and bubbles shown need room; again on every resize
void HudScreen::fitGraphTops() {
    for (const auto& [name, top] : graphTops_.asKeyValueRange()) {
        plot::ViewBox* vbox = name == u"power" ? progressGraph_->powerVBox() : progressGraph_->hrCadenceVBox();
        QList<std::pair<double, double>> rooms;
        for (MaxBubble* bubble : maxBubbles_) {
            if (bubble->star()->vbox() == vbox && bubble->star()->onGraph && bubble->star()->y) {
                rooms.append({bubble->headroom(), *bubble->star()->y});
            }
        }
        vbox->setYRange(0, MaxStar::topWithRoom(vbox, top.first, rooms), 0.0);
    }
}

void HudScreen::stepMaxStars() {
    if (!isVisible()) {
        return;
    }
    const double now = steadySeconds(starClock_);
    for (int phase = 0; phase < maxStars_.size(); ++phase) {
        maxStars_[phase].first->step(now, phase);
    }
    // the bubbles beside them, each clear of the stars and of the bubbles placed before it
    QList<QRectF> avoid;
    for (const auto& [star, name] : maxStars_) {
        if (star->isVisible()) {
            avoid.append(star->sceneBoundingRect());
        }
    }
    for (MaxBubble* bubble : maxBubbles_) {
        bubble->step(now, avoid);
    }
}

// a sensor quiet longer than kDropoutZeroSeconds reads 0 instead of freezing on its last value (its next reading
// sets it again)
void HudScreen::zeroGaugesOnDropout() {
    const AppState& state = appState();
    QList<std::pair<Gauge*, const SavedValues*>> gauges{{wattGaugeDisplay_, &state.savedPowerVals}};
    if (state.antHRM && hrGaugeDisplay_) {
        gauges.append({hrGaugeDisplay_, &state.savedHRVals});
    }
    if (state.showsCadence() && cadenceGaugeDisplay_) {
        gauges.append({cadenceGaugeDisplay_, &state.savedCadenceVals});
    }
    for (const auto& [gauge, saved] : gauges) {
        // (the reading in the middle shows the 0: the needle stops at the minimum, e.g. 40 on the heart rate gauge)
        if (!saved->times.isEmpty() && state.secondsPassed - saved->times.back() > kDropoutZeroSeconds &&
            gauge->gauge()->reading() > 0) {
            gauge->gauge()->setValue(0);
        }
    }
    // the speed too: a stopped wheel sends no new revolutions, so the pipeline never reports the 0 itself
    if (speedMph_ > 0 && state.secondsPassed - speedReadAt_ > kDropoutZeroSeconds) {
        setSpeed(0, false);
    }
}

}  // namespace awo

namespace awo {

ConfirmEndWorkoutWindow* HudScreen::endPrompt() const {
    return endPrompt_;
}

}  // namespace awo

namespace awo {

void HudScreen::runSimulatedRide(double untilSeconds, SimulatedRider& rider,
                                 const std::function<void(double, const RiderReading&)>& record) {
    timer_->stop();
    if (smoothBarTimer_) {
        smoothBarTimer_->stop();
    }
    // the power gauge's rings a second each, one a simulated tick
    powerRings_.setSeconds(1);
    AppState& state = appState();
    const bool workoutMode = state.workoutMode;
    // (Musical Cadence Mode: silent music, its clock a second a tick)
    if (cadenceMode_) {
        simulatedMusic_ = 0.0;
    }
    if (workoutMode) {
        startWorkout();
    }
    // (only the end is drawn)
    graphUpdates_ = false;
    RiderReading reading;
    double revolutions = 0.0;
    for (int i = 0; i < static_cast<int>(untilSeconds) + 5; ++i) {
        if (workoutMode && (!engine_->started() || engine_->workoutSecondsPassed() >= untilSeconds)) {
            break;
        }
        if (!workoutMode && state.secondsPassed >= untilSeconds) {
            break;
        }
        if (cadenceMode_ && simulatedMusic_) {
            *simulatedMusic_ += 1;
        }
        processEvents(1.0);
        const double t = state.secondsPassed;
        const double asked = workoutMode ? engine_->currentPower() : currentPower_;
        reading = rider.sample(t, asked, state.ftp, cadenceMode_);
        revolutions += reading.speedMph / 2.23694 / state.wheelCircumference;
        saveSimulatedReading(t, reading);
        if (record) {
            record(t, reading);
        }
    }
    graphUpdates_ = true;
    hudGraphDataUpdate();
    simulatedRevolutions_ = revolutions;
    showSimulatedReading(reading);
    updatePowerRings();
    if (cadenceMode_) {
        // the beat caught at its brightest
        updateCenterRings(1.0);
    }
}

void HudScreen::showSimulatedReading(const RiderReading& reading) {
    wattGaugeDisplay_->gauge()->setValue(reading.power);
    if (hrGaugeDisplay_) {
        hrGaugeDisplay_->gauge()->setValue(reading.heartRate);
    }
    if (cadenceGaugeDisplay_) {
        cadenceGaugeDisplay_->gauge()->setValue(reading.cadence);
    }
    setSpeed(py::round(reading.speedMph, 1));
    setDistance(py::round(appState().wheelCircumference * simulatedRevolutions_ * 0.000621371, 2));
}

void HudScreen::playOn(bool on) {
    if (on) {
        if (cadenceMode_ && !simulatedMusic_) {
            simulatedMusic_ = engine_->workoutSecondsPassed();
        }
        lastTimestamp_ = steadySeconds(clock_);
        musicSince_ = lastTimestamp_;
        simulatedMusicRunning_ = true;
        timer_->start();
        if (smoothBarTimer_) {
            smoothBarTimer_->start();
        }
    } else {
        catchUpSimulatedMusic();
        simulatedMusicRunning_ = false;
        timer_->stop();
        if (smoothBarTimer_) {
            smoothBarTimer_->stop();
        }
    }
}

void HudScreen::playReading(SimulatedRider& rider) {
    AppState& state = appState();
    const double asked = state.workoutMode ? engine_->currentPower() : currentPower_;
    const RiderReading reading = rider.sample(state.secondsPassed, asked, state.ftp, cadenceMode_);
    // (the workout's own pause holds the ride)
    if (!state.workoutMode || engine_->unpaused()) {
        simulatedRevolutions_ += reading.speedMph / 2.23694 / state.wheelCircumference;
        saveSimulatedReading(state.secondsPassed, reading);
    }
    showSimulatedReading(reading);
}

}  // namespace awo

namespace awo {

// ---- Musical Cadence Mode's music (AppState's musicTracks, see music::MusicPlayer): where it is is the workout's
// clock

void HudScreen::musicStart() {
    musicStop();
    if (simulatedMusic_) {
        *simulatedMusic_ = 0.0;
        return;
    }
    musicPlayer_ = std::make_unique<music::MusicPlayer>(appState().musicTracks);
    musicPlayer_->volume = musicVolume_;
    try {
        musicPlayer_->start();
    } catch (const std::exception& e) {
        // no sound device: the ride goes on, timed by the clock instead
        log::event(QStringLiteral("ERROR"), QStringLiteral("Music: can't play (RuntimeError: %1), timing the ride by the clock")
                                                .arg(QString::fromLocal8Bit(e.what())));
        musicPlayer_.reset();
        // (as a timed message, which the next tick would otherwise clear)
        const int now = static_cast<int>(engine_->workoutSecondsPassed());
        appState().workoutMessages.append({now, now + 15, QStringLiteral("Music can't be played, see the log")});
    }
}

void HudScreen::catchUpSimulatedMusic() {
    if (!simulatedMusic_ || !simulatedMusicRunning_) {
        return;
    }
    const double now = steadySeconds(clock_);
    if (!simulatedMusicPaused_) {
        *simulatedMusic_ += now - musicSince_;
    }
    musicSince_ = now;
}

std::optional<double> HudScreen::musicSeconds() {
    if (simulatedMusic_) {
        catchUpSimulatedMusic();
        return *simulatedMusic_;
    }
    if (!musicPlayer_) {
        return std::nullopt;
    }
    // songs that couldn't be played go to the log (the clock has moved on past them)
    const QList<std::pair<QString, QString>> skipped = musicPlayer_->skipped();
    while (musicSkipsLogged_ < skipped.size()) {
        const auto& [path, reason] = skipped[musicSkipsLogged_++];
        log::event(QStringLiteral("ERROR"), QStringLiteral("Music: skipped %1: %2").arg(path, reason));
    }
    return musicPlayer_->position();
}

void HudScreen::musicPause(bool paused) {
    catchUpSimulatedMusic();
    simulatedMusicPaused_ = paused;
    if (musicPlayer_) {
        musicPlayer_->paused = paused;
    }
}

void HudScreen::musicStop() {
    if (musicPlayer_) {
        musicPlayer_->stop();
        musicPlayer_.reset();
        musicSkipsLogged_ = 0;
    }
}

int HudScreen::songAt(double seconds) {
    const QList<MusicTrack>& tracks = appState().musicTracks;
    int index = 0;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks[i].start <= seconds) {
            index = i;
        }
    }
    return index;
}

QString HudScreen::songTime(double seconds) {
    const int whole = static_cast<int>(seconds);
    if (whole >= 3600) {
        return QStringLiteral("%1:%2:%3")
            .arg(whole / 3600)
            .arg(whole / 60 % 60, 2, 10, QLatin1Char('0'))
            .arg(whole % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
}

// the title bar: <app>: Musical Cadence Mode: <song> : <time into it>/<its length> while the music plays
void HudScreen::updateSongTitle() {
    const AppState& state = appState();
    QString text = QStringLiteral("Awooo Workout: ") + state.workoutDisplayName();
    const std::optional<double> seconds = musicSeconds();
    if (seconds && !state.musicTracks.isEmpty()) {
        const MusicTrack& track = state.musicTracks[songAt(*seconds)];
        const double into = std::min(std::max(*seconds - track.start, 0.0), track.duration);
        text += QStringLiteral(": %1 : %2/%3").arg(track.title, songTime(into), songTime(track.duration));
    }
    if (text != songTitle_) {
        songTitle_ = text;
        emit titleChanged(QStringLiteral("   ") + text);
    }
}

// where in the ride each song was heard (AppState's musicPlayed), for the synopsis' song strip. A paused song's span
// runs on through the pause; a skipped one is never heard, so it has none
void HudScreen::recordSong() {
    AppState& state = appState();
    const std::optional<double> seconds = musicSeconds();
    if (!seconds || state.musicTracks.isEmpty()) {
        return;
    }
    const int index = songAt(*seconds);
    const MusicTrack& track = state.musicTracks[index];
    // (the music's over)
    if (*seconds >= track.start + track.duration) {
        return;
    }
    QList<SongHeard>& played = state.musicPlayed;
    if (!played.isEmpty()) {
        // (up to now: the song before ends where this one starts, with no gap of a tick between them)
        played.back().end = state.secondsPassed;
    }
    if (played.isEmpty() || played.back().track != index) {
        played.append({index, state.secondsPassed, state.secondsPassed});
    }
}

void HudScreen::hideEvent(QHideEvent* event) {
    // leaving the workout screen (END, back to setup) stops the music; minimizing the window doesn't
    if (cadenceMode_ && !event->spontaneous()) {
        musicStop();
    }
    QWidget::hideEvent(event);
}

}  // namespace awo

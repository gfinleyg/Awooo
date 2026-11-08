#pragma once

// The workout screen (ClassHUDWindow.py's widgets; its logic is core/WorkoutEngine): the power, cadence and heart
// rate gauges, the block being ridden and what's up next, the workout and block timers and the spiral, the graph,
// speed/distance and the elapsed time, the difficulty (or, in free mode with an FE-C trainer, watts; in Musical Cadence
// Mode, volume) slider, START/PAUSE/END, and the end-of-workout grades.
//
// Every 100 ms it moves the workout on (processEvents) and shows what changed; between ticks the timer bars and the
// spiral glide (about 60 frames a second), and the graph's max stars ride and twinkle. Readings come from the
// ReadingPipeline onto the gauges; the graph and grades read them from AppState.

#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include <memory>
#include <optional>

#include "AppState.hpp"
#include "SimulatedRider.hpp"
#include "FtpColors.hpp"
#include "WorkoutEngine.hpp"

class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTimer;
class QVariantAnimation;

namespace awo {

namespace ant {
class AntBridge;
class ReadingPipeline;
}  // namespace ant

namespace music {
class MusicPlayer;
}

namespace plot {
class CurveItem;
class InfiniteLine;
class ScatterItem;
class SolidColoredColumns;
}  // namespace plot

class BlockBubble;
class BlockHighlight;
class ConfirmEndWorkoutWindow;
class Gauge;
class GradeList;
class GradeOverlay;
class LongVLabels;
class MaxBubble;
class MaxStar;
class PowerSpiral;
class SmoothProgressBar;
class WorkoutGraph;
class ZoneLegend;

// the workout clock's tick
constexpr int kTickMs = 100;
// the power gauge's nested circles: the power now in the middle, then each one out the average over this many ticks
// before
constexpr int kPowerRingTicks = 5;
constexpr double kPowerRingSeconds = kPowerRingTicks * kTickMs / 1000.0;
// as many rings as fit at the gauge's size with each at least this wide, never fewer than kPowerRingMin
constexpr double kPowerRingMinPx = 6;
constexpr int kPowerRingMin = 3;
// Space starts a workout and then pauses it: not for this long after the start
constexpr double kSpacePauseGuardSeconds = 0.5;
// the cadence gauge's rings' beat: they brighten on each pedal stroke's beat (Musical Cadence Mode: the music's, else
// the chainring's turn) and fade over this long
constexpr double kBeatSeconds = 0.2;

// The power gauge as the workout screen shows it, the reading in the middle as big as fits in its rings
Gauge* newPowerGauge();

// the heart rate gauge's hearts: light pink up to kHeartPinkBpm, through red to dark red at kHeartDarkOfMax of the max
// heart rate (none set: the gauge's 200 BPM top); each color can be changed by a theme
inline const QString kHeartPink = QStringLiteral("#ffb3c6");
inline const QString kHeartRed = QStringLiteral("#e8102a");
inline const QString kHeartDark = QStringLiteral("#800014");
constexpr double kHeartPinkBpm = 90;
constexpr double kHeartDarkOfMax = 0.95;
QList<std::pair<double, QColor>> heartColors(int maxHr);
// The heart rate gauge as the workout screen shows it: its center nested hearts the size of the other gauges' rings,
// in the color for the heart rate at maxHr, the reading in the middle as big as fits and the units below
Gauge* newHeartRateGauge(int maxHr);

// The power gauge's nested circles, each the band's color at a power: the innermost the power now, then out from it
// the average over each block of the ride (kPowerRingSeconds long, or setSeconds), newest to oldest. A block's average
// is worked out once, when it ends, and kept: each ring holds its color until the next block ends and moves it out one.
class PowerRings {
public:
    explicit PowerRings(double seconds = kPowerRingSeconds) : seconds_(seconds) {}

    // blocks this long from now on: every block again from the readings at the next update
    void setSeconds(double seconds);
    // every tick: finishes any blocks ended since the last, then colors the gauge's rings
    void update(Gauge* gauge);
    // the power zone colors changed: the kept blocks in the new ones
    void recolor();

private:
    double blockEnd(int index) const { return *start_ + index * seconds_; }
    std::pair<double, QColor> block(double start, double end) const;
    int mostBlocks() const;

    double seconds_;
    QList<std::pair<double, QColor>> blocks_;
    std::optional<double> start_;
    int ended_ = 0;
    std::optional<ftpcolors::ZoneRanges> ranges_;
};

// the average power read after start up to end (AppState's saved power, as the gauge showed it); with no reading in
// that time, the power then (powerAt)
double blockAverage(double start, double end);
// the power read at a time: blended between readings, 0 before the first and once the meter's been quiet
// kDropoutZeroSeconds
double powerAt(double when);

class HudScreen : public QWidget {
    Q_OBJECT

public:
    // the graph's current reading diamonds' size, and how far one reaches above its reading (its outline too)
    static constexpr double kCurrentMarkerSize = 11;
    static constexpr double kCurrentMarkerReach = kCurrentMarkerSize / 2 + 0.5;
    // the timers' blink at the end of the workout and of each block
    static constexpr double kBlinkWindowSeconds = 10;
    static constexpr double kBlinkHalfPeriodSeconds = 0.5;
    static constexpr double kBlinkMinDurationSeconds = 2;
    // the last block's "Up Next:" and its "last block" note go this far into it, the block grades taking their place
    static constexpr double kLastBlockNoteSeconds = 30;
    // the left column's width (design px): the block grades' when they take it over, so it's that wide from the
    // start and the screen doesn't move when they take it over
    static constexpr int kUpNextWidth = 490;
    // the lines a workout message under Ride At may use (a longer one is made smaller), kept for it whether there's a
    // message or not, so Ride At never moves
    static constexpr int kMessageLines = 2;
    // the spiral's power and cadence: each the average over the last this many seconds
    static constexpr double kSpiralAverageSeconds = 3.0;

    // bridge: the ANT connection (its readings go onto the gauges once start() is called); engineClock: the workout
    // engine's (tests: a frozen one, as the Python golden rides had)
    explicit HudScreen(ant::AntBridge* bridge, QWidget* parent = nullptr, WorkoutEngine::Clock engineClock = {});
    ~HudScreen() override;

    // the readings onto the gauges from now on, and off again (the workout's over or left)
    void startReadings();
    void stopReadings();
    // the workout is over or left: nothing moves any more
    void stopTimers();

    // ---- The workout ----

    WorkoutEngine& engine() { return *engine_; }
    // one tick of the workout clock, timeDelta seconds since the last (the 100 ms timer's, or a simulated second)
    void processEvents(double timeDelta);
    // START
    void startWorkout();
    // PAUSE (and resume)
    void pauseRecording();
    // Keep Existing Data and Restart
    void restartWorkout();
    void endGrading();
    // END: the prompt (restart, setup, synopsis, keep going)
    void confirmEndWorkout();
    // the START/END button reads END: a free ride, or a workout once it's started
    bool endAvailable() const;
    int currentPower() const { return currentPower_; }
    bool cadenceMode() const { return cadenceMode_; }

    // a reading onto the gauges and speed/distance, as the readings pipeline gives it
    void showPower(int watts);
    void showHeartRate(int bpm);
    void showCadence(double rpm, int decimals);
    // speed in MPH, distance in miles (shown in the distance unit); reading: a new sensor reading (a stopped
    // wheel's 0 comes from zeroGaugesOnDropout)
    void setSpeed(double mph, bool reading = true);
    void setDistance(double miles);
    // the graph drawn from the readings and power requests now
    void hudGraphDataUpdate();
    void updatePowerRings();
    PowerRings& powerRings() { return powerRings_; }

    // PreviewRide.run_hud: the workout (free mode: the elapsed time) ridden by the simulated rider a second a tick until
    // untilSeconds, or the workout's end, its own timers stopped; the graph drawn once at the end. record gets every
    // reading
    void runSimulatedRide(double untilSeconds, SimulatedRider& rider,
                          const std::function<void(double, const RiderReading&)>& record = {});
    // a reading on the gauges and speed/distance, as the simulated ride shows it (the distance from the revolutions
    // its speed made)
    void showSimulatedReading(const RiderReading& reading);
    // (LayoutViewer's Play) the workout going on live from where a simulated ride left it: its own timers, the silent
    // music's clock in real time; off: frozen where it's got to
    void playOn(bool on);
    // while playing: a second's reading from the rider, as the readings pipeline would give it
    void playReading(SimulatedRider& rider);
    // a timed .awo message's line, as the workout shows it (empty: none)
    void setWorkoutMessage(const QString& text);
    // (the theme picker's preview, redrawn in a ride sandbox of its own: not along with the app's screens) the zone
    // colors no longer followed by themselves, and followed now
    void detachFromColors();
    void refreshColors() { refreshFtpColors(); }
    // a silent stand-in for Musical Cadence Mode's music, its clock moved on by hand (simulated rides, tests; none:
    // the real music)
    void simulateMusic(std::optional<double> seconds) { simulatedMusic_ = seconds; }
    std::optional<double> simulatedMusic() const { return simulatedMusic_; }
    // Musical Cadence Mode's music playing (none: not started, stopped, or simulated)
    const music::MusicPlayer* musicPlayer() const { return musicPlayer_.get(); }
    const QList<std::pair<MaxStar*, QString>>& maxStars() const { return maxStars_; }
    const QList<MaxBubble*>& maxBubbles() const { return maxBubbles_; }
    // each completed block's average power and cadence for the graph, oldest first (one shown at a time)
    const QList<BlockBubble*>& blockBubbles() const { return blockBubbles_; }
    // in the last kBubbleLeadSeconds of a block at least that long: the target the next block starts at, for the block
    // bubbles to move to a block with that target ahead of it (none when the next is a switch single leg block)
    static constexpr double kBubbleLeadSeconds = 10;
    std::optional<double> upcomingBubbleTarget() const;
    // the target the block bubbles go by now: the one being ridden, or through a switch single leg block the one the
    // block before it ended at
    double currentBubbleTarget() const { return bubbleTarget_; }
    // the graph's stars and bubbles to where they are at now (seconds on their clock)
    void stepGraphMarkers(double now);
    // the target line on the graph, for the bubbles to keep clear of (scene coordinates): rects reaching
    // kTargetLineReach pixels either side of it
    static constexpr double kTargetLineReach = 1.5;
    QList<QRectF> targetLineRects() const;

    // ---- What's on it (tests, LayoutViewer, the theme picker's preview) ----

    QPushButton* startEndButton() const { return startEndButton_; }
    QPushButton* pauseButton() const { return pauseButton_; }
    QSlider* difficultySlider() const { return difficultySlider_; }
    QLabel* rideAtLabel() const { return rideAtLabel_; }
    QLabel* workoutMessageLabel() const { return workoutMessageLabel_; }
    QLabel* blockLabel() const { return blockNonIntLabel_; }
    QLabel* blockCountLabel() const { return blockMultiplierLabel_; }
    // the lit block's slide and swell, drawn over the block's labels (none outside Workout Mode)
    BlockHighlight* blockHighlight() const { return blockHighlight_; }
    QLabel* upNextLabel() const { return upNextLabelDescription_; }
    // an interval up next: its repeat count's cap, down the box's left side
    QLabel* upNextCap() const { return upNextCap_; }
    QLabel* upNextHeader() const { return upNextLabelHeader_; }
    LongVLabels* remainingList() const { return remainingWorkoutList_; }
    // a block finished: the old Up Next box sliding off to the right and the coming up list's top block going up behind
    // "Coming up..." into its place (C++ only); finishing it puts the boxes straight where they go
    bool upNextSlideRunning() const;
    void finishUpNextSlide();
    // (the copies going up into the box and off out of it while it runs, their box label; null when there's none)
    QLabel* upNextArriving() const;
    QLabel* upNextLeaving() const;
    ZoneLegend* zoneLegend() const { return zoneLegend_; }
    QLabel* workoutTimeLabel() const { return workoutTime_; }
    QLabel* remainingTimeLabel() const { return remainingWorkoutTime_; }
    QLabel* elapsedLabel() const { return timeElapsed_; }
    QLabel* blockRemainingLabel() const { return timeBlockRemainingValue_; }
    QLabel* speedLabel() const { return speedValueLabel_; }
    QLabel* distanceLabel() const { return distValueLabel_; }
    QLabel* difficultyText() const { return difficultyPercentage_; }
    QLabel* batteryWarning() const { return batteryWarningLabel_; }
    // the speed (MPH) and distance (miles) as read, and the readings pipeline (none before startReadings)
    double speedMph() const { return speedMph_; }
    double distanceMiles() const { return distanceMiles_; }
    const ant::ReadingPipeline* readings() const { return readings_.get(); }
    QList<QLabel*> intervalLabels() const;
    Gauge* powerGauge() const { return wattGaugeDisplay_; }
    Gauge* cadenceGauge() const { return cadenceGaugeDisplay_; }
    Gauge* heartRateGauge() const { return hrGaugeDisplay_; }
    PowerSpiral* spiral() const { return spiralWorkoutProgress_; }
    GradeOverlay* gradeOverlay() const { return gradeOverlay_; }
    GradeList* gradeList() const { return workoutGradeList_; }
    WorkoutGraph* graph() const { return progressGraph_; }
    SmoothProgressBar* workoutBar() const { return timeProgressBar_; }
    SmoothProgressBar* blockBar() const { return currentBlockProgressBar_; }
    ConfirmEndWorkoutWindow* endPrompt() const;
    bool showingGrades() const { return workoutGraded_; }

    // Esc: the END prompt once END is available (the window's Esc asks the screen first)
    Q_INVOKABLE bool escapePressed();
    // Ride At and the message under it kept between the gauges either side (the layout calls it after a fit)
    Q_INVOKABLE void fitWorkoutMessage();

signals:
    // the END prompt's choices: the synopsis, back to setup
    void synopsisRequested();
    void setupRequested();
    // Musical Cadence Mode: the window's title, with the song playing and how far into it
    void titleChanged(const QString& title);

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUpNext();
    void buildCurrentBlock();
    void buildGauges();
    void buildGraph();
    void buildTimers();
    void buildOtherInfo();
    void buildSlider();
    void buildButtons();

    void tick();
    void updateSmoothBars();
    void showTickEvents(const WorkoutEngine::TickEvents& events);
    void showBlock();
    void showIntervalBlock();
    void showDifficultyLabels();
    void showWorkoutComplete(const WorkoutEngine::TickEvents& events);
    void showPaused();
    void updateSpiralProgress(double sinceTick = 0.0);
    void recordSpiralSample();
    std::optional<double> recentAverage(const SavedValues& saved, double seconds = kSpiralAverageSeconds) const;
    void progressBlinkCheck(double sinceTick = 0.0);
    bool blinkPhaseOn(double secondsRemaining) const;
    void workoutBlinkDefault();
    void workoutBlinkOn();
    void setWorkoutTimerColors(const QString& chunk, const QString& bar, const QString& label);
    void blockBlinkDefault();
    void blockBlinkOn();
    void updateUpNextRemaining();
    void updateUpNextBoxes();
    void hideLastBlockNote();
    void updateRequestedPowerDisplays();
    void updateWorkoutMessage();
    // a side gauge outside the grid, placed by placeSideGauges, its cell kept so nothing else moves
    void keepSideGaugeCell(Gauge* gauge, const QSize& minimum, const QSize& maximum, int row, int column, int rows,
                           int columns);
    void placeSideGauges();
    void placeWorkoutSideGauges();
    void placeFreeRideSideGauges();
    void fitBetweenGauges(QLabel* label, const QString& style, int points, int lines, const QString& text);
    void showWorkoutGrades(bool show);
    void refreshWorkoutGrades();
    void showEffortAverage();
    void updateRampPauseCountdown();
    void updateTrainerTarget();
    void updateBatteryWarning();
    void updateSpeedLabel();
    void unitsChanged();
    void zeroGaugesOnDropout();
    void plotSavedVals(const QString& savedName, plot::CurveItem* curve, const QString& color);
    void updateMaxStars(double graphStart);
    void fitGraphTops();
    void stepMaxStars();
    // the block being ridden (from blockStart_) ended now: its bubble on the graph
    void finishBlockBubble();
    void clearBlockBubbles();
    void difficultyChanged(int value);
    void volumeChanged(int value);
    void trainerWattsChanged(int value);
    void trainerWattsEdited();
    void nudgeSlider(int steps);
    void sliderWheel(QWheelEvent* event);
    void spacePause();
    void gaugeChooser();
    // the zones the cadence gauge's band and rings are colored by
    ftpcolors::ZoneRanges cadenceBandRanges() const;
    void refreshFtpColors();
    void updateCenterRings(std::optional<double> level = std::nullopt);
    void updatePowerRingsTick();
    // Musical Cadence Mode's music: made on START, playing until the ride is restarted or left
    void musicStart();
    // the workout's time from where the music is (none without music)
    std::optional<double> musicSeconds();
    void musicPause(bool paused);
    void musicStop();
    // the index in AppState's musicTracks of the song playing seconds into the music
    static int songAt(double seconds);
    static QString songTime(double seconds);
    void updateSongTitle();
    void recordSong();
    // (extra: more of the label's own css, where it isn't dimmed)
    QString labelColor(const QString& text, const QString& extra = {}) const;
    QString labelColorCss(const QString& text) const;
    void setLabelColor(QLabel* label);
    // an interval's cap colors, for its blocks' text (see zones::intervalCapCss)
    QString intervalCapCss(const QStringList& blocks) const;
    // the Up Next box's cap: shown, and its box making room for it, while an interval is up next
    void colorUpNextCap();
    // the Up Next slide from the list's top rows (rect in the Up Next box's parent), the old box's copy (if any) leaving
    // from where it is
    void startUpNextSlide(QWidget* leaving, const QRect& from);
    void hideUpNextCap();
    // the interval being ridden's repeat count, in its blocks' colors
    void colorBlockCount();
    void setStartButtonStyle();

    ant::AntBridge* bridge_;
    std::unique_ptr<ant::ReadingPipeline> readings_;
    std::unique_ptr<WorkoutEngine> engine_;
    bool cadenceMode_;
    // an FE-C trainer as the power source: its target follows the workout (or the free mode Watts slider)
    bool trainerControl_ = false;
    bool trainerCommanded_ = false;
    // (Musical Cadence Mode's music comes in Phase 8: its volume is kept for it)
    int musicVolumePercent_ = 100;
    double musicVolume_ = 1.0;
    int currentPower_ = 0;
    bool workoutGraded_ = false;
    // a workout's side gauges as last worked out for this screen size: they stay put when the grades take over
    QTimer* sideGaugeTimer_ = nullptr;
    QSize sideGaugesSize_;
    int sideGaugesRadius_ = 0;
    double sideGaugesY_ = 0;
    double sideGaugesReach_ = 0;
    QString spiralBlinkState_ = QStringLiteral("default");
    bool blinkWorkout_ = false;
    bool blinkBlock_ = false;
    bool workoutEnding_ = false;
    bool startButtonBlink_ = false;
    bool startButtonBlinkOn_ = false;
    double startButtonBlinkCounter_ = 0;
    QString startButtonStyle_;
    QString startButtonBlinkOnStyle_, startButtonBlinkOffStyle_;
    double startedAt_ = 0;
    int timerRadius_ = 15;
    int blockRemainingRadius_ = 11;
    int pastPowerIndex_ = 0;
    QList<double> timeGraphList_, powerGraphList_;
    // each y range ("power", "hrCadence"): its top before the room for the stars, and its highest target (0: none)
    QMap<QString, std::pair<double, double>> graphTops_;
    QHash<QString, QString> plottedKeys_;
    int sliderWheelRemainder_ = 0;
    QSet<int> sliderStops_;
    // speed in MPH and distance in miles as read, and when the speed was last read (secondsPassed)
    double speedMph_ = 0;
    double speedReadAt_ = 0;
    double distanceMiles_ = 0;
    QElapsedTimer clock_;
    double lastTimestamp_ = 0;

    QGridLayout* hudLayout_;
    QLabel* upNextLabelHeader_ = nullptr;
    QLabel* upNextLabelDescription_ = nullptr;
    QLabel* upNextCap_ = nullptr;
    // the interval up next: its repeat count and its blocks' text (none: a block)
    QString upNextRepeats_;
    QStringList upNextBlocks_;
    QLabel* remainingWorkoutHeader_ = nullptr;
    // the Up Next slide: its clock (ms), the block going up and the old box's picture (over the real box, kept out of
    // sight till it's done), and where they come from
    QVariantAnimation* upNextSlide_ = nullptr;
    QPointer<QWidget> upNextArriving_;
    QPointer<QWidget> upNextLeaving_;
    QRect upNextArriveFrom_;
    QRect upNextLeaveFrom_;
    LongVLabels* remainingWorkoutList_ = nullptr;
    ZoneLegend* zoneLegend_ = nullptr;
    QLabel* workoutGradesHeader_ = nullptr;
    GradeList* workoutGradeList_ = nullptr;
    QWidget* currentBlockLayoutWidget_ = nullptr;
    BlockHighlight* blockHighlight_ = nullptr;
    QLabel* blockMultiplierLabel_ = nullptr;
    QLabel* blockInt3Label_[3] = {};
    QLabel* blockInt2Label_[2] = {};
    QLabel* blockNonIntLabel_ = nullptr;
    SmoothProgressBar* currentBlockProgressBar_ = nullptr;
    QLabel* timeBlockRemainingValue_ = nullptr;
    Gauge* wattGaugeDisplay_ = nullptr;
    PowerRings powerRings_;
    Gauge* cadenceGaugeDisplay_ = nullptr;
    Gauge* hrGaugeDisplay_ = nullptr;
    Gauge* targetGauge_ = nullptr;
    QLabel* rideAtLabel_ = nullptr;
    QLabel* workoutMessageLabel_ = nullptr;
    QWidget* rideAtWidget_ = nullptr;
    QTimer* messageFitTimer_ = nullptr;
    WorkoutGraph* progressGraph_ = nullptr;
    plot::SolidColoredColumns* workoutPowerPlot_ = nullptr;
    plot::SolidColoredColumns* workoutPowerCompletedPlot_ = nullptr;
    plot::CurveItem* powerProducedPlot_ = nullptr;
    plot::CurveItem* hrPlot_ = nullptr;
    plot::CurveItem* cadencePlot_ = nullptr;
    plot::InfiniteLine* nowLine_ = nullptr;
    QList<std::pair<plot::ScatterItem*, QString>> currentMarkers_;
    QList<std::pair<MaxStar*, QString>> maxStars_;
    QList<MaxBubble*> maxBubbles_;
    QList<BlockBubble*> blockBubbles_;
    // when the block being ridden started, and the target it was at the tick before (where its bar ends)
    std::optional<double> blockStart_;
    double lastTarget_ = 0;
    bool blockIsSwitchLeg_ = false;
    double bubbleTarget_ = 0;
    QLabel* batteryWarningLabel_ = nullptr;
    PowerSpiral* spiralWorkoutProgress_ = nullptr;
    GradeOverlay* gradeOverlay_ = nullptr;
    QLabel* workoutTimeHeader_ = nullptr;
    QLabel* workoutTime_ = nullptr;
    SmoothProgressBar* timeProgressBar_ = nullptr;
    QLabel* remainingWorkoutTime_ = nullptr;
    // the four rows above, outside the grid so their width doesn't change its columns: narrowed as the heart rate gauge
    // needs, from the top of the rows kept for them (see placeWorkoutSideGauges)
    QWidget* timerStack_ = nullptr;
    QWidget* timerFirstRow_ = nullptr;
    // speed through elapsed time, a bubble behind each row
    QGridLayout* infoLayout_ = nullptr;
    QLabel* speedLabelTitle_ = nullptr;
    QLabel* speedValueLabel_ = nullptr;
    QLabel* mphLabel_ = nullptr;
    QLabel* distLabel_ = nullptr;
    QLabel* distValueLabel_ = nullptr;
    QLabel* milesLabel_ = nullptr;
    QLabel* timerLabel_ = nullptr;
    QLabel* timeElapsed_ = nullptr;
    QLabel* difficultyLabel_ = nullptr;
    QLabel* difficultyPercentage_ = nullptr;
    QString difficultyTextStyle_;
    QLineEdit* trainerWattsText_ = nullptr;
    QSlider* difficultySlider_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* startEndButton_ = nullptr;
    QPointer<ConfirmEndWorkoutWindow> endPrompt_;
    QTimer* timer_;
    QTimer* smoothBarTimer_ = nullptr;
    QTimer* starTimer_;
    QElapsedTimer starClock_;
    std::unique_ptr<music::MusicPlayer> musicPlayer_;
    qsizetype musicSkipsLogged_ = 0;
    QString songTitle_;
    // a simulated ride's music: no sound, its clock moved on a second a tick (PreviewRide's ViewerMusicPlayer)
    std::optional<double> simulatedMusic_;
    // (playing on live: the silent music's clock runs, from musicSince_ on the screen's clock)
    bool simulatedMusicRunning_ = false;
    bool simulatedMusicPaused_ = false;
    double musicSince_ = 0;
    void catchUpSimulatedMusic();
    // (off while a simulated ride runs: it's drawn once, at the end)
    bool graphUpdates_ = true;
    double simulatedRevolutions_ = 0;
};

}  // namespace awo

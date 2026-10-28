#include "HudScreen.hpp"

#include <QEvent>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "AnalogGauge.hpp"
#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "ConfirmEndWorkoutWindow.hpp"
#include "GradeWidgets.hpp"
#include "KeyShortcuts.hpp"
#include "LongVLabels.hpp"
#include "MusicPlayer.hpp"
#include "MaxStar.hpp"
#include "PlotItems.hpp"
#include "PowerSpiral.hpp"
#include "PyCompat.hpp"
#include "ReadingPipeline.hpp"
#include "SavedSettings.hpp"
#include "Session.hpp"
#include "SmoothProgressBar.hpp"
#include "SolidColoredColumns.hpp"
#include "SoundEffects.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutGraph.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

namespace awo {

namespace {

// the text styles (laid out for 1920x1200; other screen sizes are scaled, see AppLayout)
const QString kXlText = QStringLiteral("font: 900 22pt 'Orbitron';");
const QString kDifficultyText = QStringLiteral("font: 900 14pt 'Orbitron';");
// Ride At and the timed workout message under it (see fitWorkoutMessage): font, design size and lines each may use,
// and the room they keep from the gauges either side
const QString kRideAtStyle = QStringLiteral("font: 900 %1pt 'Orbitron'; background-color: rgba(0, 0, 0, 0);");
constexpr int kRideAtPoints = 35;
// (sized once for its longest text, not on every power change)
const QString kRideAtLongest = QStringLiteral("Ride At: 1000 Watts");
constexpr int kRideAtLines = 2;
const QString kMessageStyle = QStringLiteral("font: 700 %1pt 'Orbitron'; background-color: rgba(0, 0, 0, 0);");
constexpr int kMessagePoints = 22;
constexpr int kMessageLines = 3;
constexpr int kMessageGap = 12;
const QString kWorkoutComplete = QStringLiteral("Workout Complete!\nCooldown, or press END for more options");
const QString kPressStart = QStringLiteral("Press Start to begin workout");
const QString kLastBlock = QStringLiteral("You're on the last block, way to go!");

// the max bubbles' text beside each sensor curve's star, by its saved values
const QHash<QString, QString> kMaxBubbleTexts = {{QStringLiteral("power"), QStringLiteral("Max Pwr: %1 W")},
                                                 {QStringLiteral("hr"), QStringLiteral("Max HR: %1 bpm")},
                                                 {QStringLiteral("cadence"), QStringLiteral("Max Cad: %1 rpm")}};

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

QString sensorColor(const QString& name) {
    if (name == u"hr") {
        return ftpcolors::kSensorHr;
    }
    if (name == u"cadence") {
        return ftpcolors::kSensorCadence;
    }
    return ftpcolors::kSensorPower;
}

QLabel* centeredLabel(const QString& text = {}) {
    auto* label = new QLabel(text);
    label->setAlignment(Qt::AlignCenter);
    return label;
}

double maxOrZero(const QList<double>& values) {
    return values.isEmpty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

// a color halfway to grey (an interval's blocks not being ridden)
QString dimColor(const QString& hex, const QColor& grey = QColor(QStringLiteral("#b4b5b8")), double amount = 0.5) {
    const QColor color(hex);
    return QColor(static_cast<int>(py::round(color.red() + (grey.red() - color.red()) * amount)),
                  static_cast<int>(py::round(color.green() + (grey.green() - color.green()) * amount)),
                  static_cast<int>(py::round(color.blue() + (grey.blue() - color.blue()) * amount)))
        .name();
}

}  // namespace

// ---- The power gauge and its rings ----

Gauge* newPowerGauge() {
    auto* gauge = new Gauge;
    AnalogGauge* w = gauge->gauge();
    w->setNeedleColor(QColor(kGaugeRed));
    w->setGaugeTheme(3);
    w->setOuterCircleBg({{0.25, QColor(0, 0, 0, 0)}, {0.18, QColor(255, 255, 255, 255)}});
    w->setMouseTracking(false);
    w->setEnableBarGraph(false);
    w->units = QStringLiteral("Watts");
    w->bigNeedleCenterSize = 5;
    w->altUnitsDisplay = true;
    // (fitted, the reading is smaller than the default size: the units keep theirs)
    w->fitValueInCenter = true;
    // paler than the cadence gauge's only enough for the reading over them: the innermost shows the power back then
    w->centerRingsPale = {0.1, 0.4};
    w->centerRingCount = kPowerRingMin;
    w->centerRingMinPx = kPowerRingMinPx;
    return gauge;
}

void PowerRings::setSeconds(double seconds) {
    seconds_ = seconds;
    start_.reset();
}

int PowerRings::mostBlocks() const {
    // as many as the readings kept go back, so a bigger gauge shows its rings' colors right away
    const AppState& state = appState();
    return static_cast<int>(std::floor((state.graphSecondsShown - state.graphBreakpoint) / seconds_));
}

std::pair<double, QColor> PowerRings::block(double start, double end) const {
    const double power = blockAverage(start, end);
    return {power, zones::zoneColor(power, ranges_)};
}

void PowerRings::recolor() {
    ranges_ = zones::powerRanges();
    for (auto& [power, color] : blocks_) {
        color = zones::zoneColor(power, ranges_);
    }
}

void PowerRings::update(Gauge* gauge) {
    const double now = appState().secondsPassed;
    // (power zones, in Musical Cadence Mode too)
    if (zones::powerRanges() != ranges_) {
        recolor();
    }
    const std::optional<double> newest = start_ ? std::optional(blockEnd(ended_)) : std::nullopt;
    if (!newest || now < *newest || now - *newest > mostBlocks() * seconds_) {
        // the first tick, a new ride's clock or a long stall: every block again from the readings, the newest ending
        // half a tick ago, so the ends fall between ticks
        start_ = now - kTickMs / 2000.0;
        ended_ = 0;
        blocks_.clear();
        for (int i = 0; i < mostBlocks(); ++i) {
            blocks_.append(block(blockEnd(-i - 1), blockEnd(-i)));
        }
    }
    while (now > blockEnd(ended_ + 1)) {
        ++ended_;
        blocks_.insert(0, block(blockEnd(ended_ - 1), blockEnd(ended_)));
    }
    while (blocks_.size() > mostBlocks()) {
        blocks_.removeLast();
    }
    AnalogGauge* w = gauge->gauge();
    QList<QColor> colors{zones::zoneColor(w->reading(), ranges_)};
    for (qsizetype i = 0; i < std::min<qsizetype>(blocks_.size(), w->ringCount() - 1); ++i) {
        colors.append(blocks_[i].second);
    }
    w->setCenterRings(colors);
}

double blockAverage(double start, double end) {
    const SavedValues& saved = appState().savedPowerVals;
    const auto first = std::upper_bound(saved.times.begin(), saved.times.end(), start) - saved.times.begin();
    const auto last = std::upper_bound(saved.times.begin(), saved.times.end(), end) - saved.times.begin();
    if (last == first) {
        return powerAt((start + end) / 2);
    }
    double sum = 0;
    for (auto i = first; i < last; ++i) {
        sum += saved.values[i];
    }
    return sum / (last - first);
}

double powerAt(double when) {
    const SavedValues& saved = appState().savedPowerVals;
    const auto index = std::upper_bound(saved.times.begin(), saved.times.end(), when) - saved.times.begin();
    if (index == 0) {
        return 0;
    }
    if (index == saved.times.size()) {
        return when - saved.times.back() <= kDropoutZeroSeconds ? saved.values.back() : 0;
    }
    const double t0 = saved.times[index - 1], t1 = saved.times[index];
    return saved.values[index - 1] + (saved.values[index] - saved.values[index - 1]) * (when - t0) / (t1 - t0);
}

// ---- Building it ----

HudScreen::HudScreen(ant::AntBridge* bridge, QWidget* parent, WorkoutEngine::Clock engineClock)
    : QWidget(parent), bridge_(bridge), timer_(new QTimer(this)), starTimer_(new QTimer(this)) {
    const AppState& state = appState();
    // Musical Cadence Mode: the targets are cadences set by the music, which is the workout's clock; the cadence gauge
    // takes the power gauge's place
    cadenceMode_ = state.workoutMode && state.cadenceMode;
    // the Volume slider's position (0-100%), as saved
    const QJsonValue savedVolume = settings::load().value(QStringLiteral("musicVolume"));
    if (savedVolume.isDouble() && savedVolume.toInt() >= 0 && savedVolume.toInt() <= 100 &&
        savedVolume.toDouble() == savedVolume.toInt()) {
        musicVolumePercent_ = savedVolume.toInt();
    }
    // an FE-C trainer as the power source gets its target power from here: the workout's in workout mode, the Watts
    // slider's in free mode; Musical Cadence Mode leaves it free
    trainerControl_ = bridge_ && bridge_->trainerSelected() && !cadenceMode_;
    engine_ = std::make_unique<WorkoutEngine>(std::move(engineClock));
    clock_.start();
    lastTimestamp_ = clock_.nsecsElapsed() / 1e9;
    setWindowTitle(QStringLiteral("Awooo Workout HUD"));
    startButtonBlinkOnStyle_ = QStringLiteral(
        "QPushButton {color: #000001;                                             background-color: "
        "qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,stop: 0 #1CB5E0, stop: 1 #dadbde);                              "
        "               border-style: solid; border-width: 2px; border-radius: 15px; padding: 4px;                       "
        "                      font: bold 34px 'Segoe UI Black'; border-color: white;}                                   "
        "          QPushButton:pressed {background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #1CB5E0, "
        "stop: 1 #1f15e2);}                                             QPushButton:disabled { background-color: "
        "qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #4a4952, stop: 1 #716f7d);                              "
        "               color: grey;}");
    startButtonBlinkOffStyle_ = startButtonBlinkOnStyle_;
    startButtonBlinkOffStyle_.replace(QStringLiteral("border-style: solid"), QStringLiteral("border-style: outset"));
    startButtonBlinkOffStyle_.replace(QStringLiteral("border-color: white"), QStringLiteral("border-color: black"));

    theme::setSheet(this, QStringLiteral("\n                                QLabel:disabled{color:#808080;}\n"
                                         "                           "));
    hudLayout_ = new QGridLayout(this);
    if (state.workoutMode) {
        buildUpNext();
        buildCurrentBlock();
    }
    buildGauges();
    buildGraph();
    buildTimers();
    buildOtherInfo();
    if (state.workoutMode || trainerControl_) {
        buildSlider();
    }
    buildButtons();

    // the readings onto the gauges (once startReadings is called)
    if (bridge_) {
        readings_ = std::make_unique<ant::ReadingPipeline>(bridge_);
        connect(readings_.get(), &ant::ReadingPipeline::powerShown, this, &HudScreen::showPower);
        connect(readings_.get(), &ant::ReadingPipeline::heartRateShown, this, &HudScreen::showHeartRate);
        connect(readings_.get(), &ant::ReadingPipeline::cadenceShown, this, &HudScreen::showCadence);
        connect(readings_.get(), &ant::ReadingPipeline::speedShown, this,
                [this](double mph) { setSpeed(py::round(mph, 1)); });
        connect(readings_.get(), &ant::ReadingPipeline::distanceShown, this,
                [this](double miles) { setDistance(py::round(miles, 2)); });
        connect(bridge_, &ant::AntBridge::batteryChanged, this, &HudScreen::updateBatteryWarning);
    }
    // Up Next and the coming up list, before START
    if (state.workoutMode) {
        blockBlinkDefault();
        workoutBlinkDefault();
        updateUpNextRemaining();
    }
    updateBatteryWarning();

    // the clock's tick; the elapsed time moves from here on
    connect(timer_, &QTimer::timeout, this, &HudScreen::tick);
    timer_->setInterval(kTickMs);
    timer_->start();
    // about 60 frames a second between ticks: the two timer bars and the spiral glide rather than step
    if (state.workoutMode) {
        smoothBarTimer_ = new QTimer(this);
        smoothBarTimer_->setTimerType(Qt::PreciseTimer);
        connect(smoothBarTimer_, &QTimer::timeout, this, &HudScreen::updateSmoothBars);
        smoothBarTimer_->setInterval(16);
        smoothBarTimer_->start();
    }
    // the graph's max stars riding to a new max, or twinkling at rest
    starClock_.start();
    connect(starTimer_, &QTimer::timeout, this, &HudScreen::stepMaxStars);
    starTimer_->setInterval(MaxStar::kStepMs);
    starTimer_->start();

    // redrawn when the zone colors change, and in the distance unit when that changes
    connect(&zones::notifier(), &zones::Notifier::changed, this, &HudScreen::refreshFtpColors);
    connect(&Units::current(), &Units::changed, this, &HudScreen::unitsChanged);
    if (upNextLabelDescription_) {
        // (LayoutViewer lists them with the zone colored widgets)
        QList<QWidget*> colored{blockNonIntLabel_, upNextLabelDescription_, spiralWorkoutProgress_};
        colored += {blockInt2Label_[0], blockInt2Label_[1], blockInt3Label_[0], blockInt3Label_[1], blockInt3Label_[2]};
        for (QWidget* widget : colored) {
            widget->setProperty("awoFtpColored", true);
        }
    }
}

HudScreen::~HudScreen() = default;

void HudScreen::buildUpNext() {
    auto* upNext = new QGridLayout;
    upNextLabelHeader_ = new QLabel(QStringLiteral("Up Next:"));
    theme::setSheet(upNextLabelHeader_, kXlText);
    upNextLabelHeader_->setMinimumWidth(490);
    upNextLabelHeader_->setMaximumHeight(40);
    upNext->addWidget(upNextLabelHeader_, 0, 0);
    upNextLabelDescription_ = new QLabel;
    upNextLabelDescription_->setFixedHeight(90);
    upNextLabelDescription_->setMinimumWidth(490);
    upNext->addWidget(upNextLabelDescription_, 1, 0, 3, 1);
    remainingWorkoutHeader_ = new QLabel(QStringLiteral("Coming up..."));
    remainingWorkoutHeader_->setMaximumHeight(40);
    remainingWorkoutHeader_->setMinimumWidth(490);
    theme::setSheet(remainingWorkoutHeader_, kXlText);
    upNext->addWidget(remainingWorkoutHeader_, 4, 0);
    remainingWorkoutList_ = new LongVLabels(appState().ftpRanges);
    // (a ramp test's blocks are added as they're ridden, so its list is always empty, not almost done)
    remainingWorkoutList_->showAlmostDone = !engine_->rampTest();
    remainingWorkoutList_->setMinimumWidth(490);
    remainingWorkoutList_->setFixedHeight(560);
    upNext->addWidget(remainingWorkoutList_, 5, 0, 21, 1);
    // once the workout is complete each block's grade takes over the whole column
    workoutGradesHeader_ = new QLabel(QStringLiteral("Workout Grades"));
    theme::setSheet(workoutGradesHeader_, kXlText);
    workoutGradesHeader_->setMaximumHeight(40);
    workoutGradesHeader_->hide();
    upNext->addWidget(workoutGradesHeader_, 0, 0);
    workoutGradeList_ = new GradeList;
    workoutGradeList_->setMinimumWidth(490);
    workoutGradeList_->hide();
    upNext->addWidget(workoutGradeList_, 1, 0, 25, 1);
    hudLayout_->addLayout(upNext, 0, 0, 20, 6);
}

void HudScreen::buildCurrentBlock() {
    currentBlockLayoutWidget_ = new QWidget;
    currentBlockLayoutWidget_->setFixedHeight(190);
    currentBlockLayoutWidget_->setMinimumWidth(810);
    auto* block = new QGridLayout;
    blockMultiplierLabel_ = centeredLabel();
    theme::setSheet(blockMultiplierLabel_, QStringLiteral("font: 900 24pt 'Orbitron';"));
    block->addWidget(blockMultiplierLabel_, 0, 0, 6, 2);
    for (int i = 0; i < 3; ++i) {
        blockInt3Label_[i] = centeredLabel();
        block->addWidget(blockInt3Label_[i], 2 * i, 2, 2, 14);
    }
    for (int i = 0; i < 2; ++i) {
        blockInt2Label_[i] = centeredLabel();
        block->addWidget(blockInt2Label_[i], 3 * i, 2, 3, 14);
    }
    blockNonIntLabel_ = centeredLabel(kPressStart);
    theme::setSheet(blockNonIntLabel_, QStringLiteral("font: 900 22pt 'Orbitron';"));
    block->addWidget(blockNonIntLabel_, 0, 0, 6, 16);
    for (QLabel* label : intervalLabels()) {
        label->hide();
    }
    blockMultiplierLabel_->hide();
    currentBlockProgressBar_ = new SmoothProgressBar;
    currentBlockProgressBar_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    currentBlockProgressBar_->setRange(0, 100);
    currentBlockProgressBar_->setValue(0);
    currentBlockProgressBar_->setTextVisible(false);
    block->addWidget(currentBlockProgressBar_, 6, 0, 1, 16);
    timeBlockRemainingValue_ = centeredLabel();
    // rounded like the timer boxes (see timerRadius_)
    constexpr int blockRemainingHeight = 26;
    blockRemainingRadius_ = blockRemainingHeight / 2 - 2;
    timeBlockRemainingValue_->setMinimumHeight(blockRemainingHeight);
    block->addWidget(timeBlockRemainingValue_, 7, 0, 1, 16);
    currentBlockLayoutWidget_->setLayout(block);
    hudLayout_->addWidget(currentBlockLayoutWidget_, 0, 6, 4, 16, Qt::AlignTop);
}

QList<QLabel*> HudScreen::intervalLabels() const {
    return {blockInt2Label_[0], blockInt2Label_[1], blockInt3Label_[0], blockInt3Label_[1], blockInt3Label_[2]};
}

void HudScreen::buildGauges() {
    const AppState& state = appState();
    wattGaugeDisplay_ = newPowerGauge();
    // (colored for as many rings as fit wherever it ends up)
    connect(wattGaugeDisplay_->gauge(), &AnalogGauge::ringCountChanged, this, &HudScreen::updatePowerRings);
    gaugeChooser();
    if (cadenceMode_) {
        // in the cadence gauge's usual corner; the cadence gauge is the big one in the middle
        wattGaugeDisplay_->setMaximumSize(220, 165);
        wattGaugeDisplay_->setMinimumSize(200, 150);
        hudLayout_->addWidget(wattGaugeDisplay_, 16, 6, 4, 3, Qt::AlignBottom);
        // (small: a numbered tick every 100 W, nothing between)
        wattGaugeDisplay_->gauge()->scalaSubdivCount = 1;
    } else if (state.workoutMode) {
        hudLayout_->addWidget(wattGaugeDisplay_, 4, 7, 16, 15);
    } else {
        hudLayout_->addWidget(wattGaugeDisplay_, 0, 4, 20, 24);
    }
    updatePowerRings();
    if (state.showsCadence()) {
        // the chainring spins at the cadence
        cadenceGaugeDisplay_ = new Gauge(QStringLiteral("Chainring.svg"), std::nullopt, true);
        AnalogGauge* w = cadenceGaugeDisplay_->gauge();
        w->setNeedleColor(QColor(kGaugeRed));
        w->setMouseTracking(false);
        w->setOuterCircleBg({{0, QColor(0, 0, 0, 0)}});
        w->setScalePolygonColors(QGradientStops{{0, QColor(0, 0, 0, 0)}});
        w->bigScaleMarker = QColor(0, 0, 0, 0);
        w->needleCenterBg = {{0, QColor(QStringLiteral("#FFFFFF"))}};
        w->bigNeedleCenterSize = 5;
        w->units = QStringLiteral("RPM");
        w->initialScaleFontsize = 18;
        w->initialValueFontsize = 56;
        w->setMaxValue(200);
        w->setScalaCount(1);
        w->scalaSubdivCount = 1;
        w->setEnableBigScaleGrid(false);
        w->altUnitsDisplay = true;
        w->valueDecimals = 1;
        w->fitValueInCenter = true;
        if (cadenceMode_) {
            // the cadence zones' band (like the power gauge's), a numbered tick every 10 RPM, and the chainring shrunk
            // to sit inside the band; 0 to 140 RPM (the needle stops at 140, the number in the middle doesn't)
            w->setMaxValue(140);
            w->setScalaCount(14);
            w->initialScaleFontsize = 13;
            w->gaugeColorInnerRadiusFactor = 0.95;
            w->setEnableBarGraph(false);
            cadenceGaugeDisplay_->chainring()->scale = w->gaugeColorInnerRadiusFactor;
            // (colored once there's a target)
            w->setCenterRings(QList<QColor>{QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#ffffff"))});
            hudLayout_->addWidget(cadenceGaugeDisplay_, 4, 7, 16, 15);
        } else if (state.workoutMode) {
            cadenceGaugeDisplay_->setMaximumSize(220, 165);
            cadenceGaugeDisplay_->setMinimumSize(200, 150);
            hudLayout_->addWidget(cadenceGaugeDisplay_, 16, 6, 4, 3, Qt::AlignBottom);
        } else {
            cadenceGaugeDisplay_->setMaximumSize(400, 300);
            cadenceGaugeDisplay_->setMinimumSize(400, 300);
            hudLayout_->addWidget(cadenceGaugeDisplay_, 10, 0, 9, 11, Qt::AlignBottom);
        }
        w->rescale();
    }
    // (the cadence gauge's band, now it's there)
    gaugeChooser();
    // the gauge showing the Ride At target's arc
    targetGauge_ = cadenceMode_ ? cadenceGaugeDisplay_ : wattGaugeDisplay_;
    rideAtLabel_ = centeredLabel();
    theme::setSheet(rideAtLabel_, kRideAtStyle.arg(kRideAtPoints));
    // wraps rather than run into the gauges either side (fitWorkoutMessage); its minimum height set to a line: a
    // word wrapped label's would be at its narrowest possible width, so the screen would seem to need far more room
    rideAtLabel_->setWordWrap(true);
    rideAtLabel_->setMinimumHeight(rideAtLabel_->fontMetrics().lineSpacing());
    rideAtLabel_->setMinimumWidth(770);
    // the workout file's timed messages under it, two lines kept for them so Ride At doesn't jump
    workoutMessageLabel_ = new QLabel;
    theme::setSheet(workoutMessageLabel_, kMessageStyle.arg(kMessagePoints));
    workoutMessageLabel_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    workoutMessageLabel_->setWordWrap(true);
    workoutMessageLabel_->setMinimumHeight(2 * workoutMessageLabel_->fontMetrics().lineSpacing());
    // their widths (and the margins fitWorkoutMessage gives them) mustn't widen the grid's columns; they fit themselves
    for (QLabel* label : {rideAtLabel_, workoutMessageLabel_}) {
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout::setOwnFit(label);
    }
    messageFitTimer_ = new QTimer(this);
    messageFitTimer_->setSingleShot(true);
    messageFitTimer_->setInterval(0);
    connect(messageFitTimer_, &QTimer::timeout, this, &HudScreen::fitWorkoutMessage);
    rideAtWidget_ = new QWidget;
    theme::setSheet(rideAtWidget_, QStringLiteral("background-color: rgba(0, 0, 0, 0);"));
    auto* rideAt = new QVBoxLayout(rideAtWidget_);
    rideAt->setContentsMargins(0, 0, 0, 0);
    rideAt->setSpacing(0);
    rideAt->addStretch();
    rideAt->addWidget(rideAtLabel_);
    rideAt->addWidget(workoutMessageLabel_);
    // stacked at the bottom of the power gauge's rows, over the gauge's empty lower part
    hudLayout_->addWidget(rideAtWidget_, 16, 7, 4, 15);
    // (the gauges either side can move without the whole screen resizing)
    rideAtWidget_->installEventFilter(this);
    if (state.antHRM) {
        // a pink resting heart, red and bigger on each beat at the heart rate
        hrGaugeDisplay_ = new Gauge(QStringLiteral("HeartSmaller.svg"),
                                    Gauge::Beat{QStringLiteral("Heart.svg"), QColor(QStringLiteral("#FF8FAB")),
                                                QColor(QStringLiteral("#E8102A"))});
        AnalogGauge* w = hrGaugeDisplay_->gauge();
        w->setNeedleColor(QColor(kGaugeRed));
        w->setMouseTracking(false);
        w->needleCenterBg = {{0, QColor(QStringLiteral("#FFFFFF"))}};
        w->setScalePolygonColors(QGradientStops{{0, QColor(0, 0, 0, 0)}});
        w->setOuterCircleBg({{0, QColor(0, 0, 0, 0)}});
        // (smaller is a bigger center)
        w->bigNeedleCenterSize = 20;
        w->units = QStringLiteral("BPM");
        w->setMinValue(40);
        w->setMaxValue(200);
        w->setEnableBarGraph(false);
        w->setScalaCount(2);
        w->scalaSubdivCount = 5;
        if (state.workoutMode) {
            hudLayout_->addWidget(hrGaugeDisplay_, 15, 21, 6, 4);
        } else {
            hudLayout_->addWidget(hrGaugeDisplay_, 11, 23, 9, 9);
        }
    }
}

void HudScreen::buildGraph() {
    const AppState& state = appState();
    auto* graphLayout = new QGridLayout;
    progressGraph_ = new WorkoutGraph;
    progressGraph_->vb()->setMouseEnabled(false, false);
    progressGraph_->powerVBox()->setMouseEnabled(false, false);
    progressGraph_->hrCadenceVBox()->setMouseEnabled(false, false);
    // the graph just follows the workout: no mouse (it goes to the screen behind it) or keyboard
    progressGraph_->setAttribute(Qt::WA_TransparentForMouseEvents);
    progressGraph_->setFocusPolicy(Qt::NoFocus);
    progressGraph_->viewport()->setFocusPolicy(Qt::NoFocus);
    progressGraph_->updateViews();
    connect(progressGraph_->vb(), &plot::ViewBox::resized, progressGraph_, &WorkoutGraph::updateViews);
    progressGraph_->setXRange(0, state.graphSecondsShown);
    progressGraph_->setYRange(0, 1000, 0.0);
    progressGraph_->setMaximumHeight(200);
    progressGraph_->plotScreen()->setTitle(std::nullopt);
    progressGraph_->plotScreen()->showAxis(plot::AxisItem::Left, false);
    progressGraph_->plotScreen()->showAxis(plot::AxisItem::Right, false);
    // the time ticks at 0.8x the graph's font size, "Time (minutes)" at 16px (set before the theme first styles it)
    QFont tickFont(progressGraph_->font());
    if (tickFont.pointSizeF() > 0) {
        tickFont.setPointSizeF(tickFont.pointSizeF() * 0.8);
    } else {
        tickFont.setPixelSize(std::max(1, static_cast<int>(py::round(tickFont.pixelSize() * 0.8))));
    }
    progressGraph_->axis(plot::AxisItem::Bottom)->setTickFont(tickFont);
    plot::AxisItem::Style styles = progressGraph_->axisStyles();
    styles[1].second = QStringLiteral("16px");
    progressGraph_->plotScreen()->setLabel(plot::AxisItem::Bottom, QStringLiteral("Time (minutes)"), styles);
    // the workout's target bars; Musical Cadence Mode's targets are cadences, drawn on the cadence axis
    plot::ViewBox* targetVBox = cadenceMode_ ? progressGraph_->hrCadenceVBox() : progressGraph_->powerVBox();
    workoutPowerPlot_ = new plot::SolidColoredColumns;
    targetVBox->addItem(workoutPowerPlot_);
    workoutPowerCompletedPlot_ = new plot::SolidColoredColumns;
    targetVBox->addItem(workoutPowerCompletedPlot_);
    powerProducedPlot_ = new plot::CurveItem;
    progressGraph_->powerVBox()->addItem(powerProducedPlot_);
    if (state.antHRM) {
        hrPlot_ = new plot::CurveItem;
        progressGraph_->hrCadenceVBox()->addItem(hrPlot_);
    }
    if (state.showsCadence()) {
        cadencePlot_ = new plot::CurveItem;
        progressGraph_->hrCadenceVBox()->addItem(cadencePlot_);
    }
    // see-through, so the window's shaded background shows
    progressGraph_->setBackground(std::nullopt);
    progressGraph_->viewport()->setAutoFillBackground(false);
    theme::setSheet(progressGraph_, QStringLiteral("QGraphicsView { border: 0px; background: transparent; }"));
    // a subtle dark grid and axis labels, to read against the shaded background (black: a theme recolors them)
    for (const auto side : {plot::AxisItem::Bottom, plot::AxisItem::Left}) {
        progressGraph_->axis(side)->setPen(plot::mkPen(QColor(0, 0, 0)));
        progressGraph_->axis(side)->setTextPen(plot::mkPen(QColor(0, 0, 0)));
    }
    progressGraph_->plotScreen()->showGrid(true, true, 0.25);
    // a thin light blue line with caret ends at the time now, between what's been ridden and what's to come; above
    // the workout bars, under the curves
    nowLine_ = new plot::InfiniteLine(0, 90);
    nowLine_->setPen(theme::themedPen(plot::mkPen(QColor(QStringLiteral("#bdd6de")), 1.5)));
    nowLine_->addMarker(QStringLiteral("v"), 0.0, 14);
    nowLine_->addMarker(QStringLiteral("^"), 1.0, 14);
    nowLine_->setZValue(5);
    progressGraph_->powerVBox()->addItem(nowLine_);
    // a small solid diamond on each sensor curve's latest reading, and a star over its highest on the graph (riding the
    // curve to the next one) with a bubble beside it
    QList<std::pair<plot::ViewBox*, QString>> sources{{progressGraph_->powerVBox(), QStringLiteral("power")}};
    if (state.antHRM) {
        sources.append({progressGraph_->hrCadenceVBox(), QStringLiteral("hr")});
    }
    if (state.showsCadence()) {
        sources.append({progressGraph_->hrCadenceVBox(), QStringLiteral("cadence")});
    }
    const QHash<QString, plot::CurveItem*> curves = {
        {QStringLiteral("power"), powerProducedPlot_}, {QStringLiteral("hr"), hrPlot_}, {QStringLiteral("cadence"), cadencePlot_}};
    for (const auto& [vbox, name] : sources) {
        const QString color = sensorColor(name);
        // (outlined in its line's halo color, to show over the bars as the line does)
        auto* marker = new plot::ScatterItem(QStringLiteral("d"), kCurrentMarkerSize);
        marker->setBrush(QColor(color));
        marker->setPen(zones::markerOutline(color));
        marker->setZValue(20);
        vbox->addItem(marker);
        currentMarkers_.append({marker, name});
        WorkoutGraph* graph = progressGraph_;
        auto* star = new MaxStar(curves.value(name), vbox, [graph] { return graph->vb()->xRange(); });
        progressGraph_->scene()->addItem(star);
        maxStars_.append({star, name});
        auto* bubble = new MaxBubble(star, kMaxBubbleTexts.value(name));
        progressGraph_->scene()->addItem(bubble);
        maxBubbles_.append(bubble);
    }
    connect(progressGraph_->vb(), &plot::ViewBox::resized, this, &HudScreen::fitGraphTops);
    // (LayoutViewer lists them with the colors a theme can change)
    theme::setPaintedColors(this, {MaxStar::kColor, MaxStar::kEdgeColor, theme::kPlotLabelBg});
    graphLayout->addWidget(progressGraph_);
    hudLayout_->addLayout(graphLayout, 20, 0, 4, 30);
}

void HudScreen::buildTimers() {
    const AppState& state = appState();
    // the selected devices' low battery warnings; keeps its space when hidden, so nothing shifts mid-workout
    batteryWarningLabel_ = centeredLabel();
    batteryWarningLabel_->setWordWrap(true);
    // (half the screen's usual label font, to leave room for the spiral)
    theme::setSheet(batteryWarningLabel_, QStringLiteral("font: 900 8pt 'Orbitron'; color:") + kGaugeRed + u';');
    QSizePolicy batteryPolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    batteryPolicy.setRetainSizeWhenHidden(true);
    batteryWarningLabel_->setSizePolicy(batteryPolicy);
    batteryWarningLabel_->hide();
    if (!state.workoutMode) {
        return;
    }
    auto* duration = new QGridLayout;
    // the workout and the block being ridden, drawn with the power and cadence ridden; centered over the timer
    // boxes below it, whatever shape its space is
    spiralWorkoutProgress_ = new PowerSpiral;
    spiralWorkoutProgress_->clear(state.workoutDurationSeconds, session::ringHistory());
    // (all the spare room between the block info and the slider)
    spiralWorkoutProgress_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // each block's grade shows in the middle of the spiral as it ends
    gradeOverlay_ = new GradeOverlay(spiralWorkoutProgress_);
    duration->addWidget(spiralWorkoutProgress_, 0, 0, 10, 1);
    duration->addWidget(batteryWarningLabel_, 10, 0, 1, 1);
    // rounded timer boxes (Qt draws square corners when the radius is more than half the height)
    constexpr int timerBoxHeight = 34;
    timerRadius_ = timerBoxHeight / 2 - 2;
    workoutTimeHeader_ = centeredLabel(QStringLiteral("Workout Time:"));
    workoutTimeHeader_->setMinimumHeight(timerBoxHeight);
    duration->addWidget(workoutTimeHeader_, 11, 0, 1, 1);
    workoutTime_ = centeredLabel(QStringLiteral("0s / ") + minimizedDuration(state.workoutDurationSeconds));
    workoutTime_->setMinimumHeight(timerBoxHeight);
    duration->addWidget(workoutTime_, 12, 0, 1, 1);
    timeProgressBar_ = new SmoothProgressBar;
    timeProgressBar_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    timeProgressBar_->setRange(0, 100 * state.workoutDurationSeconds);
    timeProgressBar_->setValue(0);
    timeProgressBar_->setTextVisible(false);
    timeProgressBar_->setFixedHeight(timerBoxHeight);
    duration->addWidget(timeProgressBar_, 13, 0, 1, 1);
    remainingWorkoutTime_ = centeredLabel(QStringLiteral("Done in: ") + minimizedDuration(state.workoutDurationSeconds));
    remainingWorkoutTime_->setMinimumHeight(timerBoxHeight);
    duration->addWidget(remainingWorkoutTime_, 14, 0, 1, 1);
    // down through row 14, so the timer boxes sit just above the heart rate gauge (the spiral takes the extra room)
    hudLayout_->addLayout(duration, 0, 22, 15, 8);
}

void HudScreen::buildOtherInfo() {
    const AppState& state = appState();
    auto* info = new QGridLayout;
    // free mode has no Workout Time box: its battery warning sits at the top of this group
    int row = 0;
    if (!state.workoutMode) {
        info->addWidget(batteryWarningLabel_, 0, 0, 1, 3);
        row = 1;
    }
    speedLabelTitle_ = new QLabel(QStringLiteral("Speed"));
    speedLabelTitle_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    info->addWidget(speedLabelTitle_, row, 0, 1, 1);
    speedValueLabel_ = centeredLabel(QStringLiteral("0"));
    info->addWidget(speedValueLabel_, row, 1, 1, 1);
    mphLabel_ = new QLabel(Units::current().speedUnit());
    mphLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    info->addWidget(mphLabel_, row, 2, 1, 1);
    distLabel_ = new QLabel(QStringLiteral("Dist."));
    distLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    info->addWidget(distLabel_, row + 1, 0, 1, 1);
    distValueLabel_ = centeredLabel(QStringLiteral("0"));
    info->addWidget(distValueLabel_, row + 1, 1, 1, 1);
    milesLabel_ = new QLabel(Units::current().distanceUnit());
    milesLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    info->addWidget(milesLabel_, row + 1, 2, 1, 1);
    // speed and distance only come from a real speed sensor (the power -> speed fallback is learned from one too)
    for (QLabel* label : {speedLabelTitle_, speedValueLabel_, mphLabel_, distLabel_, distValueLabel_, milesLabel_}) {
        label->setVisible(state.hasSpeedSensor());
    }
    timerLabel_ = centeredLabel(QStringLiteral("Elapsed Time:"));
    info->addWidget(timerLabel_, row + 2, 0, 1, 3);
    timeElapsed_ = centeredLabel(QStringLiteral("0s"));
    info->addWidget(timeElapsed_, row + 3, 0, 1, 3);
    if (state.workoutMode) {
        hudLayout_->addLayout(info, 16, 25, 4, 5);
    } else {
        hudLayout_->addLayout(info, 2, 25, 4, 5);
    }
}

// workout mode's FTP slider; free mode with an FE-C trainer the same slider setting the trainer's watts, and Musical
// Cadence Mode (whose targets don't depend on FTP) the music's volume
void HudScreen::buildSlider() {
    const AppState& state = appState();
    auto* difficulty = new QGridLayout;
    difficultyLabel_ = centeredLabel(cadenceMode_ ? QStringLiteral("Volume") : QStringLiteral("Difficulty"));
    theme::setSheet(difficultyLabel_, kDifficultyText);
    difficulty->addWidget(difficultyLabel_, 0, 0);
    difficultyPercentage_ =
        centeredLabel(state.workoutMode ? WorkoutEngine::difficultyFtpText(0) : QStringLiteral("Watts:"));
    // (difficultyChanged adds its color to this)
    difficultyTextStyle_ = kDifficultyText;
    theme::setSheet(difficultyPercentage_, difficultyTextStyle_);
    difficultyPercentage_->setMinimumWidth(120);
    difficulty->addWidget(difficultyPercentage_, 1, 0);
    int sliderRow = 2;
    if (!state.workoutMode) {
        // free mode's target watts, following the slider and typed in
        trainerWattsText_ = new QLineEdit;
        theme::setSheet(trainerWattsText_, difficultyTextStyle_);
        trainerWattsText_->setAlignment(Qt::AlignCenter);
        trainerWattsText_->setMaxLength(static_cast<int>(QString::number(ant::kTargetPowerMaxWatts).size()));
        trainerWattsText_->setMaximumWidth(120);
        connect(trainerWattsText_, &QLineEdit::editingFinished, this, &HudScreen::trainerWattsEdited);
        difficulty->addWidget(trainerWattsText_, 2, 0, Qt::AlignHCenter);
        sliderRow = 3;
    }
    difficultySlider_ = new QSlider;
    difficultySlider_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    if (cadenceMode_) {
        difficultySlider_->setRange(0, 100);
        difficultySlider_->setValue(musicVolumePercent_);
    } else if (state.workoutMode) {
        difficultySlider_->setRange(-10, 10);
        difficultySlider_->setValue(0);
    } else {
        const int maxWatts = std::min(state.maxWatts, ant::kTargetPowerMaxWatts);
        difficultySlider_->setRange(ant::kTargetPowerMinWatts, maxWatts);
        // free mode starts at half of FTP
        difficultySlider_->setValue(std::min(static_cast<int>(py::round(state.ftp * .5)), maxWatts));
        // the wheel/Up/Down stop at FTP and the starting watts as well as every 5 W, so either can be got back to
        sliderStops_ = {state.ftp, difficultySlider_->value()};
    }
    difficultySlider_->setSingleStep(1);
    if (cadenceMode_) {
        difficultySlider_->setPageStep(10);
        connect(difficultySlider_, &QSlider::valueChanged, this, &HudScreen::volumeChanged);
    } else if (state.workoutMode) {
        connect(difficultySlider_, &QSlider::valueChanged, this, &HudScreen::difficultyChanged);
    } else {
        connect(difficultySlider_, &QSlider::valueChanged, this, &HudScreen::trainerWattsChanged);
    }
    // (no vertical margin on the handle: a negative one let it hang past the groove's ends)
    theme::setSheet(difficultySlider_,
                    QStringLiteral("\n                                                    QSlider::groove:vertical { "
                                   "background:#bdd6de; width:10px; position:absolute; left:20px; right:20px}\n"
                                   "                                                    QSlider::handle:vertical { "
                                   "background:%1; width:40px; height:40px; margin:0px -40px; border-style:solid; "
                                   "border-color:#9fc5d1; border-width:1px; border-radius:15px}\n"
                                   "                                                    "
                                   "QSlider::handle:vertical:disabled { background:transparent}\n"
                                   "                                                ")
                        .arg(kGaugeRed));
    // workout mode enables it on START; free mode has no START so it's live right away, as is the volume
    difficultySlider_->setEnabled(!state.workoutMode || cadenceMode_);
    if (cadenceMode_) {
        volumeChanged(difficultySlider_->value());
    }
    difficulty->addWidget(difficultySlider_, sliderRow, 0, 15 - sliderRow, 1);
    hudLayout_->addLayout(difficulty, 0, 30, 15, 2);
    if (!state.workoutMode) {
        trainerWattsChanged(difficultySlider_->value());
    }
    // the mouse wheel anywhere on the screen (but the scrolling lists) and Up/Down move the slider a step at a time
    difficultySlider_->installEventFilter(this);
    auto* up = new QShortcut(QKeySequence(Qt::Key_Up), this);
    connect(up, &QShortcut::activated, this, [this] { nudgeSlider(1); });
    auto* down = new QShortcut(QKeySequence(Qt::Key_Down), this);
    connect(down, &QShortcut::activated, this, [this] { nudgeSlider(-1); });
    keys::add(difficultySlider_, QList<int>{}, {QStringLiteral("↑ ↓")});
}

void HudScreen::buildButtons() {
    const AppState& state = appState();
    if (state.workoutMode) {
        auto* buttons = new QGridLayout;
        pauseButton_ = new QPushButton(QStringLiteral("PAUSE"));
        pauseButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
        pauseButton_->setEnabled(false);
        connect(pauseButton_, &QPushButton::clicked, this, &HudScreen::pauseRecording);
        buttons->addWidget(pauseButton_, 0, 0);
        startEndButton_ = new QPushButton(QStringLiteral("START"));
        startEndButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
        connect(startEndButton_, &QPushButton::clicked, this, [this] {
            if (endAvailable()) {
                confirmEndWorkout();
            } else {
                startWorkout();
            }
        });
        buttons->addWidget(startEndButton_, 1, 0);
        hudLayout_->addLayout(buttons, 15, 30, 9, 2);
        startButtonBlink_ = true;
        // Space starts the workout, then pauses/resumes it; Esc (the window's) is END
        keys::Options startKey;
        startKey.label = QStringLiteral("Spacebar");
        startKey.when = [this] { return !engine_->started(); };
        keys::add(startEndButton_, {Qt::Key_Space}, startKey);
        keys::Options pauseKey;
        pauseKey.label = QStringLiteral("Spacebar");
        pauseKey.when = [this] { return engine_->started(); };
        pauseKey.action = [this] { spacePause(); };
        keys::add(pauseButton_, {Qt::Key_Space}, pauseKey);
        keys::Options escKey;
        escKey.label = QStringLiteral("Esc");
        escKey.when = [this] { return endAvailable(); };
        keys::add(startEndButton_, QList<int>{}, escKey);
        layout::setSymbol(pauseButton_);
    } else {
        startEndButton_ = new QPushButton(QStringLiteral("END"));
        startEndButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
        connect(startEndButton_, &QPushButton::clicked, this, &HudScreen::confirmEndWorkout);
        hudLayout_->addWidget(startEndButton_, 20, 30, 4, 2);
        keys::add(startEndButton_, QList<int>{}, {QStringLiteral("Esc")});
    }
    layout::setSymbol(startEndButton_);
}

// ---- Readings ----

void HudScreen::startReadings() {
    if (readings_) {
        readings_->start();
    }
}

void HudScreen::stopReadings() {
    if (readings_) {
        readings_->stop();
    }
}

void HudScreen::detachFromColors() {
    disconnect(&zones::notifier(), nullptr, this, nullptr);
}

void HudScreen::stopTimers() {
    timer_->stop();
    if (smoothBarTimer_) {
        smoothBarTimer_->stop();
    }
    starTimer_->stop();
}

void HudScreen::showPower(int watts) {
    wattGaugeDisplay_->gauge()->setValue(watts);
    // the ring behind the reading in the power's zone color (power zones even in Musical Cadence Mode), see-through
    wattGaugeDisplay_->gauge()->setOuterCircleBg(
        {{0.25, QColor(0, 0, 0, 0)}, {0.18, QColor(QStringLiteral("#44") + zones::gaugeRing(watts, zones::powerRanges()))}});
}

void HudScreen::showHeartRate(int bpm) {
    if (hrGaugeDisplay_) {
        hrGaugeDisplay_->gauge()->setValue(bpm);
    }
}

void HudScreen::showCadence(double rpm, int decimals) {
    if (cadenceGaugeDisplay_) {
        // (the power meter's own cadence is whole RPM only)
        cadenceGaugeDisplay_->gauge()->valueDecimals = decimals;
        cadenceGaugeDisplay_->gauge()->setValue(rpm);
    }
}

void HudScreen::setSpeed(double mph, bool reading) {
    if (reading) {
        speedReadAt_ = appState().secondsPassed;
    }
    speedMph_ = mph;
    layout::setText(speedValueLabel_, py::str(py::round(Units::current().speed(speedMph_), 1)));
}

void HudScreen::setDistance(double miles) {
    distanceMiles_ = miles;
    layout::setText(distValueLabel_, py::str(py::round(Units::current().distanceFromMiles(distanceMiles_), 2)));
}

void HudScreen::unitsChanged() {
    layout::setText(mphLabel_, Units::current().speedUnit());
    layout::setText(milesLabel_, Units::current().distanceUnit());
    progressGraph_->setSpeedLabel();
    setSpeed(speedMph_, false);
    setDistance(distanceMiles_);
}

void HudScreen::updateBatteryWarning() {
    const QStringList warnings = bridge_ ? bridge_->lowBatteryWarnings() : QStringList();
    layout::setText(batteryWarningLabel_, warnings.join(u'\n'));
    batteryWarningLabel_->setVisible(!warnings.isEmpty());
}

}  // namespace awo

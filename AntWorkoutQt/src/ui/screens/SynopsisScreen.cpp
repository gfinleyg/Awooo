#include "SynopsisScreen.hpp"

#include <QEvent>
#include <QFrame>
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "AntBridge.hpp"
#include "AppState.hpp"
#include "FtpTest.hpp"
#include "GradeWidgets.hpp"
#include "KeyShortcuts.hpp"
#include "MaxStar.hpp"
#include "PlotItems.hpp"
#include "PyCompat.hpp"
#include "SetupScreen.hpp"
#include "SolidColoredColumns.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "WorkoutGraph.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

namespace awo {

const QList<int> SynopsisScreen::kBestPowerSeconds = {10, 30, 60, 120, 300, 600, 1200, 1800, 3600};
const QString SynopsisScreen::kSelectionColor = QStringLiteral("#e65100");
const QColor SynopsisScreen::kDividerColor(0, 0, 0, 200);

namespace {

// the best power lines, in kBestPowerSeconds' order
const QStringList kBestText = {
    QStringLiteral(" Watts over 10s"), QStringLiteral(" Watts over 30s"), QStringLiteral(" Watts over 1m"),
    QStringLiteral(" Watts over 2m"),  QStringLiteral(" Watts over 5m"),  QStringLiteral(" Watts over 10m"),
    QStringLiteral(" Watts over 20m"), QStringLiteral(" Watts over 30m"), QStringLiteral(" Watts over 1h!")};

const QString kStatsFontSize = QStringLiteral("14pt");

QString gradeBadgeCss(const QString& grade, int points) {
    const QString color = grading::gradeColor(grade);
    return QStringLiteral("background-color:%1; border: 2px solid #808080; border-radius: 12px; font: 900 %2pt "
                          "'Orbitron'; color: %3; qproperty-alignment: AlignCenter;")
        .arg(color)
        .arg(points)
        .arg(theme::textOn(color));
}

std::pair<QPen, QPen> sensorPens(const QString& sensor) {
    return zones::sensorPens(sensor);
}

// the readings in view: (times, values) from start to end
std::pair<QList<double>, QList<double>> inView(const std::pair<QList<double>, QList<double>>& vals, double start,
                                               double end) {
    const auto i0 = std::lower_bound(vals.first.begin(), vals.first.end(), start) - vals.first.begin();
    const auto i1 = std::upper_bound(vals.first.begin(), vals.first.end(), end) - vals.first.begin();
    return {vals.first.mid(i0, i1 - i0), vals.second.mid(i0, i1 - i0)};
}

double monotonicSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int roundInt(double value) {
    return static_cast<int>(py::round(value));
}

}  // namespace

SynopsisScreen::SynopsisScreen(ant::AntBridge* bridge, SetupScreen* setup, Clock clock, QWidget* parent)
    : QWidget(parent), bridge_(bridge), setup_(setup), clock_(clock ? std::move(clock) : Clock(monotonicSeconds)) {
    // the session's rides, this one among them: the graph shows them one after the other, each ride's readings
    // from its own start in the session, a divider where each after the first starts
    rides_ = session::rides();
    const Ride& lastRide = rides_.back();
    for (qsizetype i = 1; i < rides_.size(); ++i) {
        rideStarts_.append(rides_[i].start);
    }
    // a curve for each sensor any of the rides had (cadence from a dedicated sensor, or the power meter's own while
    // there wasn't one)
    for (const Ride& ride : rides_) {
        showHr_ = showHr_ || ride.hrm;
        showSpeed_ = showSpeed_ || ride.speed;
        showCadence_ = showCadence_ || ride.cadenceSensor || !ride.data.fallbackCadence.isEmpty();
    }
    // best power is over unbroken readings (see findBestPowerRange), but a 20m or 1h FTP test's effort keeps its
    // average through a sensor dropout, as the test's own average does: those rides' (start, end)
    for (const Ride& ride : rides_) {
        const ftptest::Test* test = ftptest::find(ride.ftpTestMode);
        if (test && test->kind == ftptest::Test::Kind::Effort) {
            keepAverageSpans_.append({ride.start, ride.start + ride.seconds});
        }
    }

    auto* gridLayout = new QGridLayout(this);
    graph_ = new WorkoutGraph;
    graph_->updateViews();
    connect(graph_->vb(), &plot::ViewBox::resized, graph_, &WorkoutGraph::updateViews);
    // axis titles at the max labels' size (see WorkoutGraph::applyTheme)
    graph_->axisTitlePoints = PlotLabel::kFontPoints;
    // axis titles and tick values start at the system default font's size; their font (family, size % and weight)
    // follows the theme's Text font row
    const QFont defaultFont = theme::defaultFont();
    for (const auto orientation : {plot::AxisItem::Left, plot::AxisItem::Right, plot::AxisItem::Bottom}) {
        plot::AxisItem* axis = graph_->axis(orientation);
        axis->setTickFont(defaultFont);
        axis->label()->setFont(defaultFont);
        plot::AxisItem::Style style = axis->labelStyle();
        bool found = false;
        for (auto& [key, value] : style) {
            if (key == u"font-family") {
                value = defaultFont.family();
                found = true;
            }
        }
        if (!found) {
            style.append({QStringLiteral("font-family"), defaultFont.family()});
        }
        axis->setLabel(axis->labelText(), axis->labelUnits(), style);
    }
    // a see-through plot in a rounded frame, so the plot area gets the same rounded corners as the stats panel
    graph_->setBackground(std::nullopt);
    graph_->viewport()->setAutoFillBackground(false);
    theme::setSheet(graph_, QStringLiteral("QGraphicsView { border: 0px; background: transparent; }"));
    graphFrame_ = new QFrame;
    graphFrame_->setObjectName(QStringLiteral("synopsisGraphFrame"));
    theme::setSheet(graphFrame_, QStringLiteral("#synopsisGraphFrame { background-color:#bdd6de; border-radius: 15px; }"));
    auto* frameLayout = new QGridLayout(graphFrame_);
    frameLayout->setContentsMargins(6, 6, 6, 6);
    frameLayout->addWidget(graph_);

    connect(&zones::notifier(), &zones::Notifier::changed, this, &SynopsisScreen::refreshFtpColors);
    // speed is kept in the distance unit it was shown in (converted if that changes); power to weight follows the
    // weight set on the setup screen
    speedUnit_ = Units::current().distance;
    connect(&Units::current(), &Units::changed, this, &SynopsisScreen::unitsChanged);
    connect(&theme::notifier(), &theme::Notifier::changed, this, &SynopsisScreen::retheme);

    // where the session ends: the last ride's end (a sensor silent that long before it shows zero)
    endTime_ = lastRide.start + lastRide.secondsPassed;
    processSynopsisData();

    // the whole session fills the graph, and is as far out as it zooms; zooming and panning only move time
    double rideSeconds = 0;
    for (const Ride& ride : rides_) {
        rideSeconds += ride.seconds;
    }
    totalSeconds_ = std::max(rideSeconds, kMinZoomSeconds);
    for (plot::ViewBox* vbox : {graph_->vb(), graph_->powerVBox(), graph_->hrCadenceVBox()}) {
        vbox->setMouseEnabled(true, false);
        plot::ViewBox::Limits limits;
        limits.xMin = 0;
        limits.xMax = totalSeconds_;
        limits.minXRange = kMinZoomSeconds;
        limits.maxXRange = totalSeconds_;
        vbox->setLimits(limits);
    }
    graph_->vb()->setXRange(0, totalSeconds_, 0.0);
    setTickSpacing(totalSeconds_);

    // the y ranges (a Musical Cadence Mode ride's targets are cadences, on the cadence axis)
    QList<double> powerTargets, cadenceTargets;
    for (const TargetBar& bar : targetBars_) {
        (bar.vbox == graph_->powerVBox() ? powerTargets : cadenceTargets) += bar.ys;
    }
    const auto [maxProducedPower, maxProducedPowerIndex] = maxOrZeroAndIndex(processedPowerVals_.second);
    double powerMax = std::max(maxOrZero(powerTargets), maxProducedPower);
    if (powerMax == 0) {
        powerMax = 400;
    }
    // (and again, a little higher if need be, to make room for the max stars, see fitStarRoom)
    powerTop_ = powerMax + 100;
    graph_->powerVBox()->setYRange(0, powerTop_, 0.0);
    const auto [maxHr, maxHrIndex] = maxOrZeroAndIndex(processedHrVals_.second);
    const auto [maxCadence, maxCadenceIndex] = maxOrZeroAndIndex(processedCadenceVals_.second);
    const auto [maxSpeed, maxSpeedIndex] = maxOrZeroAndIndex(processedSpeedVals_.second);
    double hrCadenceSpeedMax = std::max({maxHr, maxCadence, maxSpeed, maxOrZero(cadenceTargets)});
    if (hrCadenceSpeedMax == 0) {
        hrCadenceSpeedMax = 190;
    }
    hrCadenceTop_ = hrCadenceSpeedMax + 20;
    graph_->hrCadenceVBox()->setYRange(0, hrCadenceTop_, 0.0);

    // the stats in their own panel beside the graph, so they never overlap the plot or get clipped
    stats_ = new QLabel(statsText(0, totalSeconds_));
    stats_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    // (Segoe UI Black rather than Orbitron)
    theme::setSheet(stats_, QStringLiteral("background-color:#bdd6de; padding:8px; font: ") + kStatsFontSize +
                                QStringLiteral(" 'Segoe UI Black';"));
    for (const TargetBar& bar : targetBars_) {
        bar.vbox->addItem(bar.plot);
    }
    graph_->powerVBox()->addItem(powerPlot_);
    if (showHr_) {
        graph_->hrCadenceVBox()->addItem(hrPlot_);
    }
    if (showCadence_) {
        graph_->hrCadenceVBox()->addItem(cadencePlot_);
    }
    if (showSpeed_) {
        graph_->hrCadenceVBox()->addItem(speedPlot_);
    }
    // a divider where each ride after the first starts, over the power bars (under a highlighted curve)
    for (const double start : rideStarts_) {
        auto* divider = new plot::InfiniteLine(start, 90);
        QPen pen = plot::mkPen(kDividerColor, 2);
        pen.setStyle(Qt::DashLine);
        divider->setPen(theme::themedPen(pen));
        themedPens_.append({divider, pen});
        divider->setZValue(5);
        graph_->powerVBox()->addItem(divider);
    }

    // sensor curves can be clicked to highlight the whole line; clicking it again or empty plot space clears it
    sensorCurves_ = {powerPlot_};
    if (showHr_) {
        sensorCurves_.append(hrPlot_);
    }
    if (showCadence_) {
        sensorCurves_.append(cadencePlot_);
    }
    if (showSpeed_) {
        sensorCurves_.append(speedPlot_);
    }
    for (plot::CurveItem* curve : sensorCurves_) {
        curvePens_.insert(curve, {curve->pen(), curve->shadowPen()});
        curve->setClickable(true, 10);
        connect(curve, &plot::CurveItem::clicked, this, [this](plot::CurveItem* clicked) { selectCurve(clicked); });
    }
    for (plot::ViewBox* vbox : {graph_->vb(), graph_->powerVBox(), graph_->hrCadenceVBox()}) {
        connect(vbox, &plot::ViewBox::clicked, this,
                [this](QPointF, Qt::MouseButton button, bool taken) { plotClicked(button, taken); });
    }

    // everything drawn that the max labels mustn't cover; curves as drawn, dropout zeros included
    for (const TargetBar& bar : targetBars_) {
        plotSeries_.append({true, bar.vbox, bar.xs, bar.ys});
    }
    plotSeries_.append({false, graph_->powerVBox(), powerPlot_->xData(), powerPlot_->yData()});
    if (showHr_) {
        plotSeries_.append({false, graph_->hrCadenceVBox(), hrPlot_->xData(), hrPlot_->yData()});
    }
    if (showCadence_) {
        plotSeries_.append({false, graph_->hrCadenceVBox(), cadencePlot_->xData(), cadencePlot_->yData()});
    }
    if (showSpeed_) {
        // (redrawn in another unit, see unitsChanged)
        speedSeriesIndex_ = static_cast<int>(plotSeries_.size());
        plotSeries_.append({false, graph_->hrCadenceVBox(), speedPlot_->xData(), speedPlot_->yData()});
    }

    // along the top (before the max labels, which keep clear of them): the rides' headings with more than one, and
    // under them Musical Cadence Mode's songs heard
    if (rides_.size() > 1) {
        QList<BandStrip::Band> bands;
        for (const Ride& ride : rides_) {
            bands.append({ride.start, ride.start + ride.seconds, session::heading(ride)});
        }
        rideStrip_ = new BandStrip(bands, BandStrip::kRideColors, graph_->vb(), [this](int index) {
            const BandStrip::Band& band = rideStrip_->bands()[index];
            zoomTo(band.start, band.end);
        });
        addStrip(rideStrip_);
    }
    QList<BandStrip::Band> songs;
    for (const Ride& ride : rides_) {
        for (const SongSpan& song : ride.songs) {
            if (song.end > song.start) {
                songs.append({song.start + graphOffset(ride), song.end + graphOffset(ride), song.title});
            }
        }
    }
    if (!songs.isEmpty()) {
        songStrip_ = new BandStrip(songs, BandStrip::kSongColors, graph_->vb(), [this](int index) {
            const BandStrip::Band& band = songStrip_->bands()[index];
            zoomTo(band.start, band.end);
        });
        addStrip(songStrip_);
    }

    // the max markers; their labels are placed by layoutMaxLabels so they never overlap or leave the plot, and their
    // texts are formats of the max, so a zoomed in selection can show its own
    addMaxMarker(graph_->powerVBox(), processedPowerVals_, maxProducedPowerIndex, maxProducedPower,
                 [](double watts) {
                     return QStringLiteral("Max Power: %1 W").arg(py::fixed(watts, 0)) +
                            Units::current().powerToWeightText(watts);
                 },
                 powerPlot_);
    if (showHr_) {
        addMaxMarker(graph_->hrCadenceVBox(), processedHrVals_, maxHrIndex, maxHr,
                     [](double bpm) { return QStringLiteral("Max Heart Rate: %1 bpm").arg(py::fixed(bpm, 0)); }, hrPlot_);
    }
    if (showCadence_) {
        // a decimal unless the device only ever gave whole number cadence
        const bool whole = std::all_of(processedCadenceVals_.second.begin(), processedCadenceVals_.second.end(),
                                       [](double c) { return c == std::floor(c); });
        const int decimals = whole ? 0 : 1;
        addMaxMarker(graph_->hrCadenceVBox(), processedCadenceVals_, maxCadenceIndex, maxCadence,
                     [decimals](double rpm) { return QStringLiteral("Max Cadence: %1 RPM").arg(py::fixed(rpm, decimals)); },
                     cadencePlot_);
    }
    if (showSpeed_) {
        addMaxMarker(graph_->hrCadenceVBox(), processedSpeedVals_, maxSpeedIndex, maxSpeed,
                     [](double speed) {
                         return QStringLiteral("Max Speed: %1 %2").arg(py::fixed(speed, 2), Units::current().speedUnit());
                     },
                     speedPlot_);
    }
    // the labels placed again whenever the plot area moves, resizes or rescales
    connect(graph_->vb(), &plot::ViewBox::resized, this, &SynopsisScreen::layoutMaxLabels);
    connect(graph_->powerVBox(), &plot::ViewBox::rangeChanged, this, &SynopsisScreen::layoutMaxLabels);
    connect(graph_->hrCadenceVBox(), &plot::ViewBox::rangeChanged, this, &SynopsisScreen::layoutMaxLabels);

    // low battery warnings at the bottom of the stats column, in the same panel
    batteryWarning_ = new QLabel;
    batteryWarning_->setAlignment(Qt::AlignBottom | Qt::AlignLeft);
    theme::setSheet(batteryWarning_, QStringLiteral("padding:8px; color:") + kGaugeRed + QStringLiteral(";"));
    batteryWarning_->hide();
    statsLayout_ = new QVBoxLayout;
    statsLayout_->setContentsMargins(0, 0, 0, 0);
    statsLayout_->setSpacing(0);
    // the graded rides' grade heads the stats column; an FTP test isn't graded, its new FTP goes there too when
    // it's the last ride
    ftpTestMode_ = lastRide.ftpTestMode;
    grading::Grades all;
    for (int index = 0; index < rides_.size(); ++index) {
        if (rides_[index].graded) {
            gradedRides_.append(index);
            for (const auto& [key, record] : rides_[index].blockGrades.items()) {
                blockGrades_.append({GradeRegionKey{index, key}, &record});
                // (the session's average: keyed by ride too, so two rides' same blocks both count)
                all.set(grading::GradeKey{key.index + 100000 * index, key.sub, key.rep}, record);
            }
        }
    }
    grading_ = !gradedRides_.isEmpty();
    if (grading_) {
        const std::optional<double> average = grading::workoutAverage(all);
        const QString grade = grading::letter(average);
        auto* box = new QFrame;
        box->setObjectName(QStringLiteral("workoutGradeBox"));
        theme::setSheet(box, QStringLiteral("#workoutGradeBox { background-color:#bdd6de; border-radius: 15px; }"));
        auto* boxLayout = new QHBoxLayout(box);
        boxLayout->setContentsMargins(10, 8, 10, 8);
        boxLayout->setSpacing(14);
        workoutGradeBadge_ = new QLabel(grade);
        workoutGradeBadge_->setFixedSize(96, 76);
        theme::setSheet(workoutGradeBadge_, gradeBadgeCss(grade, 26));
        boxLayout->addWidget(workoutGradeBadge_);
        const QString title = gradedRides_.size() > 1 ? QStringLiteral("Session Grade") : QStringLiteral("Workout Grade");
        workoutGradeText_ = new QLabel(title + QStringLiteral("\n") +
                                       (average ? py::fixed(*average, 1) + QStringLiteral("%")
                                                : QStringLiteral("No blocks graded")));
        theme::setSheet(workoutGradeText_, QStringLiteral("font: 16pt 'Segoe UI Black';"));
        boxLayout->addWidget(workoutGradeText_, 1);
        statsLayout_->addWidget(box);
        statsLayout_->addSpacing(6);
    }
    if (!ftpTestMode_.isEmpty()) {
        setupFtpTestResult();
    }
    statsLayout_->addWidget(stats_, 1);
    statsLayout_->addWidget(batteryWarning_);

    // the time span shown, editable to zoom in or out; kept in step with the graph by updateSelection. A rounded box
    // each, From above To
    startTimeEdit_ = new QTimeEdit;
    endTimeEdit_ = new QTimeEdit;
    for (const auto& [text, edit] : {std::pair{QStringLiteral("From"), startTimeEdit_},
                                     std::pair{QStringLiteral("To"), endTimeEdit_}}) {
        edit->setDisplayFormat(QStringLiteral("H:mm:ss"));
        edit->setTimeRange(QTime(0, 0), QTime(0, 0).addSecs(static_cast<int>(std::ceil(totalSeconds_))));
        connect(edit, &QTimeEdit::timeChanged, this, [this, edit = edit] { timeEditChanged(edit); });
        // once done typing, what the graph actually settled on (its limits can clamp it)
        connect(edit, &QTimeEdit::editingFinished, this, &SynopsisScreen::syncTimeEdits);
        auto* timeBox = new QFrame;
        timeBox->setObjectName(QStringLiteral("timeBox"));
        theme::setSheet(timeBox,
                        QStringLiteral("#timeBox { background-color:#bdd6de; border-radius: 15px; }"
                                       "QTimeEdit { border-style:solid; border-color:black; border-width:2px; "
                                       "border-radius: 8px; padding: 2px 4px; background-color: qlineargradient(x1:0 "
                                       "y1:0, x2:1 y2:1, stop:0 #bdd6de stop:1 #9fc5d1); font: ") +
                            kStatsFontSize + QStringLiteral(" 'Segoe UI Black'; }"));
        auto* timeBoxLayout = new QHBoxLayout(timeBox);
        timeBoxLayout->setContentsMargins(8, 8, 8, 8);
        auto* label = new QLabel(text);
        timeLabels_.append(label);
        timeBoxLayout->addWidget(label);
        timeBoxLayout->addWidget(edit, 1);
        statsLayout_->addSpacing(6);
        statsLayout_->addWidget(timeBox);
    }
    // every block's grade, over the synopsis
    if (grading_) {
        blockGradesButton_ = new QPushButton(QStringLiteral("Block Grades"));
        connect(blockGradesButton_, &QPushButton::clicked, this, [this] { showBlockGrades(true); });
        statsLayout_->addSpacing(6);
        statsLayout_->addWidget(blockGradesButton_);
    }
    // an FTP test's new FTP becomes the FTP from here
    if (!ftpTestMode_.isEmpty()) {
        applyFtpButton_ = new QPushButton(QStringLiteral("Apply FTP"));
        connect(applyFtpButton_, &QPushButton::clicked, this, &SynopsisScreen::applyFtp);
        // greyed out with no different FTP to apply
        applyFtpButton_->setEnabled(newFtp_ && *newFtp_ != previousFtp_);
        statsLayout_->addSpacing(6);
        statsLayout_->addWidget(applyFtpButton_);
    }
    // the whole session's max heart rate becomes the Max HR from here, whatever is zoomed in
    workoutMaxHr_ = roundInt(maxHr);
    setMaxHrButton_ = new QPushButton(QStringLiteral("Set Max HR"));
    connect(setMaxHrButton_, &QPushButton::clicked, this, &SynopsisScreen::setMaxHr);
    // greyed out with no heart rate recorded, one the setup screen wouldn't take, or the Max HR already set
    setMaxHrButton_->setEnabled(80 <= workoutMaxHr_ && workoutMaxHr_ < 250 && workoutMaxHr_ != appState().maxHr);
    statsLayout_->addSpacing(6);
    statsLayout_->addWidget(setMaxHrButton_);
    // the same label width in both boxes so the two time fields line up; measured again whenever their font
    // changes, since the stylesheet font only applies once they're polished
    for (QLabel* label : timeLabels_) {
        label->installEventFilter(this);
    }
    matchTimeLabelWidths();
    if (bridge_) {
        connect(bridge_, &ant::AntBridge::batteryChanged, this, &SynopsisScreen::updateBatteryWarning);
    }
    updateBatteryWarning();

    gridLayout->addWidget(graphFrame_, 0, 0, 14, 2);
    gridLayout->addLayout(statsLayout_, 0, 2, 14, 1);
    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 1);
    // Return to Setup and Exit split the full width, under the stats column too
    auto* bottomButtons = new QHBoxLayout;
    restartButton_ = new QPushButton(QStringLiteral("Return to Setup"));
    connect(restartButton_, &QPushButton::clicked, this, &SynopsisScreen::returnToSetupRequested);
    bottomButtons->addWidget(restartButton_);
    exitButton_ = new QPushButton(QStringLiteral("Exit"));
    connect(exitButton_, &QPushButton::clicked, this, &SynopsisScreen::exitRequested);
    bottomButtons->addWidget(exitButton_);
    gridLayout->addLayout(bottomButtons, 15, 0, 1, 3);

    // clicking a graded block on the graph opens its grade (one at a time)
    if (grading_) {
        setupGradeRegions();
    }

    // keys (Ctrl shows them), none while the Block Grades sheet covers the screen. Left/Right step through the
    // graded blocks; Esc closes an open grade or the sheet first (escapePressed)
    const auto noSheet = [this] { return !blockGradesShown(); };
    keys::add(restartButton_, {Qt::Key_R}, {.label = QStringLiteral("R"), .when = noSheet});
    keys::add(exitButton_, {Qt::Key_E}, {.label = QStringLiteral("E"), .when = noSheet});
    if (grading_) {
        keys::add(blockGradesButton_, {Qt::Key_B}, {.label = QStringLiteral("B"), .when = noSheet});
        const auto stepping = [this, noSheet] { return !gradeRegions_.isEmpty() && noSheet(); };
        keys::add(graphFrame_, {Qt::Key_Right},
                  {.label = QStringLiteral("← →"), .action = [this] { stepGrade(1); }, .when = stepping,
                   .place = keys::Place::Inside, .repeat = true});
        keys::add(graphFrame_, {Qt::Key_Left},
                  {.action = [this] { stepGrade(-1); }, .when = stepping, .repeat = true});
    }
    if (applyFtpButton_) {
        keys::add(applyFtpButton_, {Qt::Key_A}, {.label = QStringLiteral("A"), .when = noSheet});
    }
    keys::add(setMaxHrButton_, {Qt::Key_M}, {.label = QStringLiteral("M"), .when = noSheet});
    // Enter in a time box applies it and leaves it, so Left/Right step through the blocks again
    for (QTimeEdit* edit : {startTimeEdit_, endTimeEdit_}) {
        keys::add(edit, keys::kEnter, {.action = [this] { window()->setFocus(); }, .field = edit});
    }

    // zooming/panning refreshes the selection's stats, max labels and time boxes once it settles
    selectionTimer_ = new QTimer(this);
    selectionTimer_->setSingleShot(true);
    selectionTimer_->setInterval(100);
    connect(selectionTimer_, &QTimer::timeout, this, &SynopsisScreen::updateSelection);
    // the max markers' stars: riding to a new max, or twinkling at rest
    starTimer_ = new QTimer(this);
    // (LayoutViewer lists them with the colors a theme can change)
    theme::setPaintedColors(this, {MaxStar::kColor, MaxStar::kEdgeColor});
    starTimer_->setInterval(MaxStar::kStepMs);
    connect(starTimer_, &QTimer::timeout, this, &SynopsisScreen::animateStars);
    starTimer_->start();
    connect(graph_->vb(), &plot::ViewBox::xRangeChanged, selectionTimer_, qOverload<>(&QTimer::start));
    updateSelection();
    QTimer::singleShot(1000, this, &SynopsisScreen::fixSize);
}

SynopsisScreen::~SynopsisScreen() = default;

QFrame* SynopsisScreen::gradePopup() const {
    return gradePopup_;
}

// ---- The readings ----

void SynopsisScreen::processSynopsisData() {
    // each ride's, from where it starts in the session
    for (int index = 0; index < rides_.size(); ++index) {
        processRide(index);
    }
    // a sensor dropout mid-ride interleaves the two cadence sources: back in time order
    if (!processedCadenceVals_.first.isEmpty()) {
        QList<std::pair<double, double>> pairs;
        for (qsizetype i = 0; i < processedCadenceVals_.first.size(); ++i) {
            pairs.append({processedCadenceVals_.first[i], processedCadenceVals_.second[i]});
        }
        std::sort(pairs.begin(), pairs.end());
        processedCadenceVals_ = {};
        for (const auto& [t, v] : pairs) {
            processedCadenceVals_.first.append(t);
            processedCadenceVals_.second.append(v);
        }
    }
    // the curves get long dropouts as zero; the processed lists stay real readings only, for the stats and maxes
    const auto curve = [this](const std::pair<QList<double>, QList<double>>& vals, const QString& sensor) {
        auto* item = new plot::CurveItem;
        const auto [xs, ys] = withDropoutsAsZero(vals.first, vals.second, endTime_);
        const auto [pen, halo] = sensorPens(sensor);
        item->setData(xs, ys, pen, halo);
        return item;
    };
    hrPlot_ = curve(processedHrVals_, ftpcolors::kSensorHr);
    powerPlot_ = curve(processedPowerVals_, ftpcolors::kSensorPower);
    cadencePlot_ = curve(processedCadenceVals_, ftpcolors::kSensorCadence);
    speedPlot_ = curve(processedSpeedVals_, ftpcolors::kSensorSpeed);
    // (the ones not shown are never added to the graph: the screen keeps them)
    for (plot::CurveItem* item : {hrPlot_, powerPlot_, cadencePlot_, speedPlot_}) {
        item->setParent(this);
    }

    // the best powers: the durations get longer, so it stops at the first one the recorded power doesn't span (the
    // stats pair them with their lines by position)
    for (const int targetTime : kBestPowerSeconds) {
        const std::optional<double> best = findBestPowerRange(targetTime);
        if (!best) {
            break;
        }
        bestPowerRange_.append(py::round(*best, 2));
    }
}

void SynopsisScreen::processRide(int index) {
    Ride& ride = rides_[index];
    const double offset = ride.start;
    const double startSeconds = ride.startTimestamp.toMSecsSinceEpoch() / 1000.0;
    const WorkoutData& data = ride.data;
    const qsizetype powerFrom = processedPowerVals_.first.size();
    const qsizetype cadenceFrom = processedCadenceVals_.first.size();

    // the target bars, each target from when it was asked for (the times have one more: the first's start)
    const QList<double>& targets = ride.powerRequestWatts;
    const QList<QDateTime>& targetTimes = ride.powerRequestTimes;
    if (!targets.isEmpty() && !targetTimes.isEmpty()) {
        QList<double> xs = {offset + ride.startTimestamp.msecsTo(targetTimes.front()) / 1000.0};
        QList<double> ys;
        for (qsizetype i = 0; i < targets.size() && i + 1 < targetTimes.size(); ++i) {
            const double delta = ride.startTimestamp.msecsTo(targetTimes[i + 1]) / 1000.0;
            if (delta > 0) {
                xs.append(offset + delta);
                ys.append(static_cast<int>(targets[i]));
            }
        }
        if (!ys.isEmpty()) {
            auto* plot = new plot::SolidColoredColumns;
            const ftpcolors::ZoneRanges ranges = ride.targetRanges;
            plot->setData(xs, ys, ranges.limits, [ranges](double value, const QList<double>&) {
                return zones::graphColor(value, ranges, ftpcolors::GraphState::Past);
            });
            targetBars_.append({index, plot, targetVBox(ride), xs, ys});
        }
    }

    // the sensor history recorded during the ride, replayed (see WorkoutData)
    if (ride.hrm) {
        int lastNonZero = 0, held = 0;
        for (qsizetype i = 0; i < data.heartRate.size(); ++i) {
            const int hr = data.heartRate.column()[i];
            // (the live gauge's hold-last-value smoothing: a 0 is only held over a few readings)
            int value = hr;
            if (hr == 0) {
                if (held < kHoldHrReadings) {
                    value = lastNonZero;
                    ++held;
                }
            } else {
                held = 0;
                lastNonZero = hr;
            }
            const double delta = data.heartRate.times()[i] - startSeconds;
            if (delta > 0) {
                processedHrVals_.first.append(offset + delta);
                processedHrVals_.second.append(value);
            }
        }
    }
    {
        int lastNonZero = 0, held = 0;
        for (qsizetype i = 0; i < data.power.size(); ++i) {
            const int watts = data.power.column()[i];
            // (as for heart rate: coasting's 0s show as 0 once they've gone on for more than a few readings)
            int value = watts;
            if (watts == 0) {
                if (held < kHoldPowerReadings) {
                    value = lastNonZero;
                    ++held;
                }
            } else {
                held = 0;
                lastNonZero = watts;
            }
            const double delta = data.power.times()[i] - startSeconds;
            if (delta > 0) {
                processedPowerVals_.first.append(offset + delta);
                processedPowerVals_.second.append(value);
            }
        }
    }

    // (each ride's counters start over, on its own wheel)
    speedLast_.reset();
    cadenceLast_.reset();
    wheelCircumference_ = ride.wheelCircumference;
    lastNonZeroSpeed_ = 0;
    lastNonZeroSpeedIndex_ = 0;
    lastNonZeroCadence_ = 0;
    lastNonZeroCadenceIndex_ = 0;
    if (ride.speedCadence) {
        // combined (121) sensor: both counters in one row
        for (qsizetype i = 0; i < data.speedCadence.size(); ++i) {
            const double delta = data.speedCadence.times()[i] - startSeconds;
            if (delta > 0) {
                processSpeedReading(offset + delta, data.speedCadence.column(0)[i], data.speedCadence.column(1)[i]);
                // odd error values, only ever seen on the combined sensor's cadence time
                const int cadenceTime = data.speedCadence.column(3)[i];
                if (cadenceTime != 513 && cadenceTime != 2305) {
                    processCadenceReading(offset + delta, data.speedCadence.column(2)[i], cadenceTime);
                }
            }
        }
        // discrete cadence (122) / speed (123) sensors
        for (qsizetype i = 0; i < data.cadence.size(); ++i) {
            const double delta = data.cadence.times()[i] - startSeconds;
            if (delta > 0) {
                processCadenceReading(offset + delta, data.cadence.column(0)[i], data.cadence.column(1)[i]);
            }
        }
        for (qsizetype i = 0; i < data.speed.size(); ++i) {
            const double delta = data.speed.times()[i] - startSeconds;
            if (delta > 0) {
                processSpeedReading(offset + delta, data.speed.column(0)[i], data.speed.column(1)[i]);
            }
        }
    }
    // the power meter's cadence, used while there was no dedicated cadence sensor reading (and all of it in Musical
    // Cadence Mode without a speed/cadence sensor)
    for (qsizetype i = 0; i < data.fallbackCadence.size(); ++i) {
        const double delta = data.fallbackCadence.times()[i] - startSeconds;
        if (delta > 0) {
            addCadenceValue(offset + delta, data.fallbackCadence.column()[i]);
        }
    }

    // a ride with no ring of its own (a free ride) gets one from its readings, for the next rides' spiral
    if (!ride.ring) {
        std::pair<QList<double>, QList<double>> power;
        for (qsizetype i = powerFrom; i < processedPowerVals_.first.size(); ++i) {
            power.first.append(processedPowerVals_.first[i] - offset);
            power.second.append(processedPowerVals_.second[i]);
        }
        QList<std::pair<double, double>> sorted;
        for (qsizetype i = cadenceFrom; i < processedCadenceVals_.first.size(); ++i) {
            sorted.append({processedCadenceVals_.first[i], processedCadenceVals_.second[i]});
        }
        std::sort(sorted.begin(), sorted.end());
        std::pair<QList<double>, QList<double>> cadence;
        for (const auto& [t, c] : sorted) {
            cadence.first.append(t - offset);
            cadence.second.append(c);
        }
        ride.ring = session::ringFromReadings(ride.seconds, power, cadence, ride.speedCadence);
        // (kept in the session too)
        if (index < session::rides().size()) {
            session::rides()[index].ring = ride.ring;
        }
    }
}

void SynopsisScreen::processSpeedReading(double seconds, int revolutions, int eventTime) {
    if (!speedLast_) {
        speedLast_ = {revolutions, eventTime};
    }
    // both counters are 16 bit and wrap (event times every 64 s): the difference masked, as live
    const int deltaRevolutions = (revolutions - speedLast_->first) & 0xFFFF;
    const int deltaTime = (eventTime - speedLast_->second) & 0xFFFF;
    // (before rejecting anything, as live, so one bad reading or a counter reset can't poison every delta after it)
    speedLast_ = {revolutions, eventTime};
    if (deltaTime <= 0) {
        return;
    }
    // MPH: event times are in 1/1024 s, 2.23694 MPH a m/s
    double speed = (wheelCircumference_ * deltaRevolutions * 2290.62656) / deltaTime;
    // crazy values out, and their revolutions from the distance, as live
    if (speed > 60) {
        return;
    }
    // the distance from accepted readings only (meters: each ride on its own wheel)
    const double meters = wheelCircumference_ * deltaRevolutions;
    totalMeters_ += meters;
    speedDistance_.first.append(seconds);
    speedDistance_.second.append(meters);
    if (speed < 5) {
        // a possible misread: the last value kept a few readings (the live gauge's smoothing)
        if (lastNonZeroSpeedIndex_ < kHoldSpeedReadings) {
            speed = lastNonZeroSpeed_;
            ++lastNonZeroSpeedIndex_;
        }
    } else {
        lastNonZeroSpeed_ = speed;
        lastNonZeroSpeedIndex_ = 0;
    }
    processedSpeedVals_.first.append(seconds);
    processedSpeedVals_.second.append(Units::current().speed(speed));
}

void SynopsisScreen::processCadenceReading(double seconds, int revolutions, int eventTime) {
    if (!cadenceLast_) {
        cadenceLast_ = {revolutions, eventTime};
    }
    const int deltaRevolutions = (revolutions - cadenceLast_->first) & 0xFFFF;
    const int deltaTime = (eventTime - cadenceLast_->second) & 0xFFFF;
    cadenceLast_ = {revolutions, eventTime};
    if (deltaTime > 0) {
        // revolutions a minute: event times in 1/1024 s, 60 s a minute (addCadenceValue drops anything over 200)
        addCadenceValue(seconds, (61440.0 * deltaRevolutions) / deltaTime);
    }
}

void SynopsisScreen::addCadenceValue(double seconds, double cadence) {
    if (cadence > 200) {
        return;
    }
    if (cadence < 10) {
        if (lastNonZeroCadenceIndex_ < kHoldCadenceReadings) {
            cadence = lastNonZeroCadence_;
            ++lastNonZeroCadenceIndex_;
        }
    } else {
        lastNonZeroCadence_ = cadence;
        lastNonZeroCadenceIndex_ = 0;
    }
    processedCadenceVals_.first.append(seconds);
    processedCadenceVals_.second.append(cadence);
}

// The highest average power over any stretch of the session at least targetTime long, or none when the recorded
// power doesn't span it (a sensor connected late, dropped out, a short ride). The whole session's power unless given
// (times, watts). A stretch is unbroken readings of one ride: the next ride, or a dropout (readings further apart
// than kDropoutZeroSeconds, where the graph drops to zero too) ends one, except in a 20m or 1h FTP test's ride
// (keepAverageSpans_), where the power is taken to have stayed at the stretch's average through a dropout
std::optional<double> SynopsisScreen::findBestPowerRange(double targetTime, const QList<double>* timesIn,
                                                         const QList<double>* wattsIn) const {
    const QList<double>& times = timesIn ? *timesIn : processedPowerVals_.first;
    const QList<double>& watts = wattsIn ? *wattsIn : processedPowerVals_.second;
    std::optional<double> best;
    double total = 0;
    qsizetype startIndex = 0;
    qsizetype nextStart = times.isEmpty()
                              ? 0
                              : std::upper_bound(rideStarts_.begin(), rideStarts_.end(), times.front()) - rideStarts_.begin();
    for (qsizetype endIndex = 0; endIndex < times.size(); ++endIndex) {
        // (the first reading of the next ride)
        bool newRide = false;
        while (nextStart < rideStarts_.size() && rideStarts_[nextStart] <= times[endIndex]) {
            ++nextStart;
            newRide = true;
        }
        bool kept = false;
        for (const auto& [start, end] : keepAverageSpans_) {
            kept = kept || (start <= times[endIndex] && times[endIndex] < end);
        }
        const bool dropout = endIndex > 0 && times[endIndex] - times[endIndex - 1] > kDropoutZeroSeconds && !kept;
        if (endIndex > startIndex && (newRide || dropout)) {
            // the next stretch starts with this reading
            startIndex = endIndex;
            total = 0;
        }
        total += watts[endIndex];
        // readings off the front while the window would still span targetTime without them
        while (endIndex > startIndex && times[endIndex] - times[startIndex + 1] >= targetTime) {
            total -= watts[startIndex];
            ++startIndex;
        }
        if (times[endIndex] - times[startIndex] >= targetTime) {
            const double average = total / static_cast<double>(endIndex - startIndex + 1);
            if (!best || average > *best) {
                best = average;
            }
        }
    }
    return best;
}

// ---- The FTP test ----

// Previous FTP, the new one the test works out to, and the difference, heading the stats column. No new FTP if the
// effort had no power readings or wasn't ridden to the end, or the ramp was under kRampBestSeconds
void SynopsisScreen::setupFtpTestResult() {
    const Ride& lastRide = rides_.back();
    previousFtp_ = lastRide.ftp;
    const ftptest::Result result = lastRide.ftpTestResult.value_or(ftptest::Result{});
    QString reason = QStringLiteral("No power recorded");
    if (ftptest::isRamp(ftpTestMode_) && !result.average) {
        reason = QStringLiteral("Ramp under %1s").arg(ftptest::kRampBestSeconds);
    } else if (result.average && !result.completed) {
        reason = QStringLiteral("Effort not completed");
    } else if (result.average) {
        const int ftp = ftptest::ftpFromAverage(ftpTestMode_, *result.average);
        // (the range the setup screen's FTP box takes)
        if (20 <= ftp && ftp < 450) {
            newFtp_ = ftp;
        } else {
            reason = QStringLiteral("%1 is outside 20-449").arg(ftp);
        }
    }
    ftpTestReason_ = reason;
    auto* box = new QFrame;
    box->setObjectName(QStringLiteral("ftpTestBox"));
    theme::setSheet(box, QStringLiteral("#ftpTestBox { background-color:#bdd6de; border-radius: 15px; }"));
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(14, 8, 14, 8);
    ftpTestText_ = new QLabel(ftpTestTextNow());
    theme::setSheet(ftpTestText_, QStringLiteral("font: 16pt 'Segoe UI Black';"));
    boxLayout->addWidget(ftpTestText_);
    statsLayout_->addWidget(box);
    statsLayout_->addSpacing(6);
}

QString SynopsisScreen::ftpTestTextNow() const {
    // (a ramp test can be ridden with no FTP set)
    const QString previous = previousFtp_ ? QString::number(previousFtp_) : QStringLiteral("not set");
    QStringList lines = {QStringLiteral("Previous FTP: ") + previous};
    if (newFtp_) {
        lines.append(QStringLiteral("New FTP: %1").arg(*newFtp_) + Units::current().powerToWeightText(*newFtp_));
        lines.append(previousFtp_ ? QStringLiteral("Difference: %1%2")
                                        .arg(*newFtp_ - previousFtp_ >= 0 ? QStringLiteral("+") : QString())
                                        .arg(*newFtp_ - previousFtp_)
                                  : QStringLiteral("Difference: --"));
    } else {
        lines.append(QStringLiteral("New FTP: --"));
        lines.append(ftpTestReason_);
    }
    return lines.join(u'\n');
}

void SynopsisScreen::applyFtp() {
    // as if typed in on the setup screen, which saves it and rebuilds the test for the new FTP
    if (setup_ && newFtp_) {
        setup_->applyFtp(*newFtp_);
    }
    previousFtp_ = newFtp_.value_or(previousFtp_);
    applyFtpButton_->setEnabled(false);
}

void SynopsisScreen::setMaxHr() {
    // as if typed in on the setup screen, which saves it
    if (setup_) {
        setup_->applyMaxHr(workoutMaxHr_);
    }
    setMaxHrButton_->setEnabled(false);
}

// the ⚙ menu's Measurements (or the weight) changed: speed converted to the new unit, the stats and labels again
void SynopsisScreen::unitsChanged() {
    const Units& units = Units::current();
    if (showSpeed_ && units.distance != speedUnit_) {
        const double factor = units.distance == u"km" ? Units::kKmPerMile : 1 / Units::kKmPerMile;
        speedUnit_ = units.distance;
        for (double& speed : processedSpeedVals_.second) {
            speed *= factor;
        }
        const auto [xs, ys] = withDropoutsAsZero(processedSpeedVals_.first, processedSpeedVals_.second, endTime_);
        speedPlot_->setData(xs, ys);
        plotSeries_[speedSeriesIndex_] = {false, graph_->hrCadenceVBox(), xs, ys};
        for (MaxMarker& marker : maxMarkers_) {
            if (marker.vals == &processedSpeedVals_) {
                marker.fullY *= factor;
                marker.y *= factor;
            }
        }
    }
    graph_->setSpeedLabel();
    if (ftpTestText_) {
        ftpTestText_->setText(ftpTestTextNow());
    }
    updateSelection();
}

void SynopsisScreen::refreshFtpColors() {
    // the bars' colors worked out again, each ride's in its own zones
    for (const TargetBar& bar : targetBars_) {
        bar.plot->recolor();
    }
}

void SynopsisScreen::retheme() {
    for (const auto& [item, pen] : themedPens_) {
        if (auto* line = dynamic_cast<plot::InfiniteLine*>(item)) {
            line->setPen(theme::themedPen(pen));
        } else if (auto* graphicsLine = dynamic_cast<QGraphicsLineItem*>(item)) {
            graphicsLine->setPen(theme::themedPen(pen));
        }
    }
    for (MaxMarker& marker : maxMarkers_) {
        marker.label->retheme();
    }
    QTimer::singleShot(0, this, &SynopsisScreen::layoutMaxLabels);
}

void SynopsisScreen::updateBatteryWarning() {
    const QStringList warnings = bridge_ ? bridge_->lowBatteryWarnings() : QStringList();
    batteryWarning_->setText(warnings.join(u'\n'));
    batteryWarning_->setVisible(!warnings.isEmpty());
}

void SynopsisScreen::fixSize() {
    // (a second after it's built: if it's been left already it mustn't show itself again over the screen that
    // replaced it)
    if (!isVisible()) {
        return;
    }
    if (!keepWindowSize_) {
        window()->showMaximized();
    }
    QTimer::singleShot(0, this, &SynopsisScreen::layoutMaxLabels);
}

plot::ViewBox* SynopsisScreen::targetVBox(const Ride& ride) const {
    return cadenceRide(ride) ? graph_->hrCadenceVBox() : graph_->powerVBox();
}

// the title of the ride's song heard longest from start to end (its secondsPassed), or none
std::optional<QString> SynopsisScreen::songDuring(const Ride& ride, double start, double end) {
    double best = 0;
    std::optional<QString> title;
    for (const SongSpan& song : ride.songs) {
        const double overlap = std::min(end, song.end) - std::max(start, song.start);
        // (max() of (overlap, title) pairs: the later title wins a tie)
        if (!title || overlap > best || (overlap == best && song.title > *title)) {
            best = overlap;
            title = song.title;
        }
    }
    return best > 0 ? title : std::nullopt;
}

// ---- The strips along the top ----

void SynopsisScreen::addStrip(BandStrip* strip) {
    strips_.append(strip);
    graph_->scene()->addItem(strip);
    if (strips_.size() == 1) {
        connect(graph_->vb(), &plot::ViewBox::resized, this, &SynopsisScreen::layoutStrips);
        connect(graph_->vb(), &plot::ViewBox::xRangeChanged, this, &SynopsisScreen::layoutStrips);
    }
    layoutStrips();
}

// each strip under the ones above it (their heights follow the max labels' size)
void SynopsisScreen::layoutStrips() {
    double top = 0;
    for (BandStrip* strip : strips_) {
        strip->top = top;
        strip->layout();
        top += strip->height();
    }
}

void SynopsisScreen::zoomTo(double start, double end) {
    start = std::max(start, 0.0);
    end = std::min(end, totalSeconds_);
    double span = std::max(end - start, kMinZoomSeconds);
    double newStart = std::max(0.0, std::min(start - (span - (end - start)) / 2, totalSeconds_ - span));
    const plot::Range view = viewRange();
    if (std::abs(view.min - newStart) < 0.5 && std::abs(view.max - (newStart + span)) < 0.5) {
        newStart = 0.0;
        span = totalSeconds_;
    }
    graph_->vb()->setXRange(newStart, newStart + span, 0.0);
}

// ---- Block grades ----

// A clickable region over each graded block's span, on its ride's target axis, as tall as its highest target (free
// ride and low power blocks get a minimum height so they can still be clicked)
void SynopsisScreen::setupGradeRegions() {
    for (const auto& [key, record] : blockGrades_) {
        if (record->end <= record->start) {
            continue;
        }
        const Ride& ride = rides_[key.ride];
        plot::ViewBox* vbox = targetVBox(ride);
        const double minHeight = 0.12 * vbox->yRange().max;
        // (grades are timed in the ride's secondsPassed)
        const QRectF rect(record->start + graphOffset(ride), 0, record->end - record->start,
                          std::max(static_cast<double>(record->top), minHeight));
        auto* region = new GradeRegion(rect, key, vbox, [this](GradeRegion* clicked) { gradeRegionClicked(clicked); });
        vbox->addItem(region);
        // clicks go to the highest item first: below everything, so the curves and labels keep theirs
        region->setZValue(-1000);
        gradeRegions_.append(region);
        if (!gradeHighlights_.contains(vbox)) {
            auto* highlight = new QGraphicsRectItem;
            QPen pen{QColor(kSelectionColor), 3};
            pen.setCosmetic(true);
            highlight->setPen(pen);
            highlight->hide();
            vbox->addItem(highlight);
            connect(vbox, &plot::ViewBox::rangeChanged, this, &SynopsisScreen::placeGradePopup);
            gradeHighlights_.insert(vbox, highlight);
        }
    }
    graphFrame_->installEventFilter(this);
}

void SynopsisScreen::gradeRegionClicked(GradeRegion* region) {
    // clicking the open block again closes it
    if (region == gradePopupRegion_) {
        closeGradePopup();
    } else {
        openGradePopup(region);
    }
}

void SynopsisScreen::openGradePopup(GradeRegion* region) {
    closeGradePopup();
    gradePopupRegion_ = region;
    QGraphicsRectItem* highlight = gradeHighlights_.value(region->vbox());
    highlight->setRect(region->rect());
    highlight->show();
    const grading::Grade* record = nullptr;
    for (const auto& [key, grade] : blockGrades_) {
        if (key.ride == region->key().ride && key.key == region->key().key) {
            record = grade;
        }
    }
    const QString grade = grading::letter(record->pct);
    QString name = record->label;
    const Ride& ride = rides_[region->key().ride];
    const grading::GradeKey& key = region->key().key;
    if (key.index < ride.workoutList.size() && ride.workoutList[key.index].type == WorkoutEntry::Type::Interval) {
        name += QStringLiteral("  ·  rep %1 of %2").arg(key.rep + 1).arg(ride.workoutList[key.index].repeats);
    }

    const double scale = theme::uiScale();
    auto* popup = new QFrame(graphFrame_);
    popup->setObjectName(QStringLiteral("gradePopup"));
    theme::setSheet(popup, QStringLiteral("#gradePopup { background-color:#bdd6de; border: 2px solid black; "
                                          "border-radius: 14px; }QLabel { font: 13pt 'Segoe UI Black'; color: black; "
                                          "background-color: transparent; }"));
    popup->setFixedWidth(roundInt(440 * scale));
    auto* layout = new QVBoxLayout(popup);
    const int margin = roundInt(12 * scale);
    layout->setContentsMargins(margin, margin, margin, margin);
    auto* header = new QHBoxLayout;
    header->setSpacing(roundInt(10 * scale));
    auto* badge = new QLabel(grade);
    badge->setFixedSize(roundInt(74 * scale), roundInt(58 * scale));
    theme::setSheet(badge, gradeBadgeCss(grade, 20));
    header->addWidget(badge);
    const QString pctText = record->pct ? py::fixed(*record->pct, 1) + QStringLiteral("%") : QStringLiteral("Not graded");
    // rich text sizes aren't scaled with the stylesheet's, so scaled here
    auto* title = new QLabel(QStringLiteral("%1<br><span style='font-size:%2pt'>%3</span>")
                                 .arg(name)
                                 .arg(roundInt(20 * scale))
                                 .arg(pctText));
    title->setWordWrap(true);
    header->addWidget(title, 1);
    auto* closeButton = new QPushButton(QStringLiteral("✕"));
    theme::setSheet(closeButton, QStringLiteral("QPushButton { background: transparent; border: none; padding: 0px; "
                                                "font: 18pt 'Segoe UI Black'; color: black; }"));
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, this, &SynopsisScreen::closeGradePopup);
    keys::add(closeButton, {}, {.label = QStringLiteral("Esc")});
    header->addWidget(closeButton, 0, Qt::AlignTop);
    layout->addLayout(header);
    auto* details = new QLabel(gradeDetailsHtml(*record, ride));
    details->setTextFormat(Qt::RichText);
    details->setWordWrap(true);
    layout->addWidget(details);
    gradePopup_ = popup;
    popup->adjustSize();
    placeGradePopup();
}

QString SynopsisScreen::gradeDetailsHtml(const grading::Grade& record, const Ride& ride) const {
    const auto pct = [](double value) { return py::fixed(value, 1) + QStringLiteral("%"); };
    const auto points = [](double value, const QString& sign) {
        return value >= 0.05 ? sign + py::fixed(value, 1) : QStringLiteral("0.0");
    };
    QList<std::pair<QString, QString>> rows;
    const double overAvg = record.overAvg.value_or(0), surge = record.surge.value_or(0);
    if (!record.pct) {
        rows.append({QStringLiteral("No power readings"), QStringLiteral("the whole block")});
    } else if (record.free) {
        rows.append({QStringLiteral("Pedaling (over %1 W)").arg(grading::kFreeRideMinWatts),
                     pct(record.pedaling.value_or(0))});
    } else {
        rows.append({QStringLiteral("Average vs target"), pct(record.avg.value_or(0))});
        if (record.low) {
            rows.append({QStringLiteral("Low (3 s average)"), pct(*record.low)});
            rows.append({QStringLiteral("High (3 s average)"), pct(record.high.value_or(0))});
        }
        rows.append({QStringLiteral("Avg over target"), points(overAvg, QStringLiteral("−"))});
        if (record.low) {
            rows.append({QStringLiteral("Surges"), points(surge, QStringLiteral("−"))});
        }
        if (record.penalty.value_or(0) < overAvg + surge - 0.05) {
            rows.append({QStringLiteral("Over penalty capped at"), points(record.penalty.value_or(0), QStringLiteral("−"))});
        }
        rows.append({QStringLiteral("Intensity bonus"), points(record.bonus.value_or(0), QStringLiteral("+"))});
    }
    if (record.endedEarly) {
        // only the part ridden before END was graded
        rows.append({QStringLiteral("Workout ended at"),
                     QStringLiteral("%1 of %2").arg(minimizedDuration(py::round(record.ridden)),
                                                    minimizedDuration(py::round(record.duration)))});
    }
    rows.append({QStringLiteral("Graded time"), QStringLiteral("%1 of %2").arg(minimizedDuration(py::round(record.graded)),
                                                                                minimizedDuration(py::round(record.ridden)))});
    QString html;
    // Musical Cadence Mode: the block's song
    if (cadenceRide(ride)) {
        if (const std::optional<QString> song = songDuring(ride, record.start, record.end)) {
            html += QStringLiteral("<div>♪ %1</div>").arg(song->toHtmlEscaped());
        }
    }
    html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='1'>");
    for (const auto& [label, value] : rows) {
        html += QStringLiteral("<tr><td>%1</td><td align='right'>%2</td></tr>").arg(label, value);
    }
    html += QStringLiteral("</table>");
    QStringList notes;
    if (record.grace >= 0.5) {
        notes.append(QStringLiteral("%1s grace (block start/end, after a pause)").arg(roundInt(record.grace)));
    }
    if (record.dropped >= 0.5) {
        notes.append(QStringLiteral("%1s %2 dropout left out")
                         .arg(roundInt(record.dropped))
                         .arg(cadenceRide(ride) ? QStringLiteral("cadence") : QStringLiteral("power")));
    }
    if (!notes.isEmpty()) {
        html += QStringLiteral("<div style='font-size:%1pt'>").arg(std::max(1, roundInt(10 * theme::uiScale()))) +
                notes.join(QStringLiteral("  ·  ")) + QStringLiteral("</div>");
    }
    return html;
}

// the open grade just above its block on the graph (below it if there's no room), inside the graph's frame; hidden
// while the block is scrolled out of view
void SynopsisScreen::placeGradePopup() {
    QFrame* popup = gradePopup_;
    GradeRegion* region = gradePopupRegion_;
    if (popup == nullptr || region == nullptr) {
        return;
    }
    const plot::Range view = viewRange();
    const QRectF rect = region->rect();
    if (rect.right() < view.min || rect.left() > view.max) {
        popup->hide();
        return;
    }
    const double middle = (std::max(rect.left(), view.min) + std::min(rect.right(), view.max)) / 2;
    const QPointF scenePoint = region->vbox()->mapViewToScene(QPointF(middle, rect.bottom()));
    const QPoint point = graph_->mapTo(graphFrame_, graph_->mapFromScene(scenePoint));
    const QRect frame = graphFrame_->rect();
    const int width = popup->width();
    const int height = popup->sizeHint().height();
    const int gap = roundInt(12 * theme::uiScale());
    const int left = std::min(std::max(point.x() - width / 2, gap / 2), frame.width() - width - gap / 2);
    int top = point.y() - height - gap;
    if (top < gap / 2) {
        top = std::min(point.y() + gap, frame.height() - height - gap / 2);
    }
    popup->setGeometry(left, std::max(gap / 2, top), width, height);
    popup->show();
    popup->raise();
}

void SynopsisScreen::closeGradePopup() {
    if (gradePopup_) {
        gradePopup_->hide();
        gradePopup_->deleteLater();
    }
    gradePopup_ = nullptr;
    gradePopupRegion_ = nullptr;
    for (QGraphicsRectItem* highlight : std::as_const(gradeHighlights_)) {
        highlight->hide();
    }
}

void SynopsisScreen::stepGrade(int step) {
    QList<GradeRegion*> regions = gradeRegions_;
    std::stable_sort(regions.begin(), regions.end(),
                     [](GradeRegion* a, GradeRegion* b) { return a->rect().left() < b->rect().left(); });
    qsizetype index = regions.indexOf(gradePopupRegion_);
    if (index >= 0) {
        index += step;
        if (index < 0 || index >= regions.size()) {
            return;
        }
    } else {
        index = step > 0 ? 0 : regions.size() - 1;
    }
    GradeRegion* region = regions[index];
    scrollTo(region->rect());
    openGradePopup(region);
}

// zoomed in with the block out of view: the same span, centered on it (its start, if it's longer than that)
void SynopsisScreen::scrollTo(const QRectF& rect) {
    const plot::Range view = viewRange();
    if (rect.left() >= view.min && rect.right() <= view.max) {
        return;
    }
    const double span = view.span();
    double newStart = rect.width() >= span ? rect.left() : rect.center().x() - span / 2;
    newStart = std::max(0.0, std::min(newStart, totalSeconds_ - span));
    graph_->vb()->setXRange(newStart, newStart + span, 0.0);
}

bool SynopsisScreen::blockGradesShown() const {
    return blockGradesOverlay_ != nullptr && blockGradesOverlay_->isVisible();
}

bool SynopsisScreen::escapePressed() {
    if (!isVisible()) {
        return false;
    }
    if (blockGradesShown()) {
        showBlockGrades(false);
        return true;
    }
    if (gradePopup_) {
        closeGradePopup();
        return true;
    }
    return false;
}

// every block's grade over the synopsis (Block Grades), the list the workout screen shows once a workout's complete
void SynopsisScreen::showBlockGrades(bool show) {
    if (!show) {
        if (blockGradesOverlay_) {
            blockGradesOverlay_->hide();
        }
        return;
    }
    if (!blockGradesOverlay_) {
        auto* overlay = new QFrame(this);
        overlay->setObjectName(QStringLiteral("blockGradesOverlay"));
        theme::setSheet(overlay, QStringLiteral("#blockGradesOverlay { background-color: rgba(20, 20, 50, 140); }"));
        overlay->installEventFilter(this);
        auto* sheet = new QFrame(overlay);
        sheet->setObjectName(QStringLiteral("blockGradesSheet"));
        theme::setSheet(sheet, QStringLiteral("#blockGradesSheet { border: 2px solid #fffffe; border-radius: 18px; "
                                              "background-color: qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e "
                                              "stop:1 #9fc5d1); }"));
        auto* layout = new QVBoxLayout(sheet);
        const int margin = roundInt(16 * theme::uiScale());
        layout->setContentsMargins(margin, margin, margin, margin);
        layout->setSpacing(roundInt(8 * theme::uiScale()));
        auto* header = new QLabel(QStringLiteral("Block Grades"));
        theme::setSheet(header, QStringLiteral("font: 900 22pt 'Orbitron'; color: #fffffe; background-color: transparent;"));
        layout->addWidget(header);
        blockGradeList_ = new GradeList;
        layout->addWidget(blockGradeList_, 1);
        auto* closeButton = new QPushButton(QStringLiteral("CLOSE"));
        connect(closeButton, &QPushButton::clicked, this, [this] { showBlockGrades(false); });
        keys::add(closeButton, Qt::Key_B, QStringLiteral("B"));
        layout->addWidget(closeButton);
        blockGradesOverlay_ = overlay;
        blockGradesSheet_ = sheet;
    }
    QList<const Ride*> rides;
    for (const Ride& ride : rides_) {
        rides.append(&ride);
    }
    blockGradeList_->refresh(rides);
    layoutBlockGrades();
    blockGradesOverlay_->show();
    blockGradesOverlay_->raise();
}

void SynopsisScreen::layoutBlockGrades() {
    if (!blockGradesOverlay_) {
        return;
    }
    blockGradesOverlay_->setGeometry(rect());
    const int width = std::min(roundInt(640 * theme::uiScale()), this->width() - 40);
    const int height = roundInt(this->height() * 0.86);
    blockGradesSheet_->setGeometry((this->width() - width) / 2, (this->height() - height) / 2, width, height);
}

void SynopsisScreen::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutBlockGrades();
}

bool SynopsisScreen::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FontChange && timeLabels_.contains(qobject_cast<QLabel*>(watched))) {
        // after the font change is done, not from inside it
        QTimer::singleShot(0, this, &SynopsisScreen::matchTimeLabelWidths);
    }
    if (watched == graphFrame_ && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, &SynopsisScreen::placeGradePopup);
    }
    if (watched == blockGradesOverlay_ && event->type() == QEvent::MouseButtonPress) {
        // a click outside the grades closes them
        const auto* press = static_cast<QMouseEvent*>(event);
        if (!blockGradesSheet_->geometry().contains(press->position().toPoint())) {
            showBlockGrades(false);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SynopsisScreen::matchTimeLabelWidths() {
    int width = 0;
    for (QLabel* label : timeLabels_) {
        label->setMinimumWidth(0);
        label->setMaximumWidth(QWIDGETSIZE_MAX);
        width = std::max(width, label->sizeHint().width());
    }
    for (QLabel* label : timeLabels_) {
        label->setFixedWidth(width);
    }
}

// ---- The max markers ----

void SynopsisScreen::addMaxMarker(plot::ViewBox* vbox, const std::pair<QList<double>, QList<double>>& vals,
                                  qsizetype index, double value, std::function<QString(double)> format,
                                  plot::CurveItem* curve) {
    // no readings for the sensor: no marker. A star marks the max (see MaxStar)
    if (vals.first.isEmpty()) {
        return;
    }
    const double x = vals.first[index];
    QGraphicsScene* scene = graph_->scene();
    auto* line = new QGraphicsLineItem;
    const QPen linePen(QColor(0, 0, 0, 160), 1);
    line->setPen(theme::themedPen(linePen));
    themedPens_.append({line, linePen});
    line->setZValue(999);
    scene->addItem(line);
    auto* label = new PlotLabel(format(value));
    QFont font(QStringLiteral("Orbitron"), static_cast<int>(PlotLabel::kFontPoints));
    font.setBold(true);
    label->setLabelFont(font);
    label->setZValue(1000);
    // clicking the label highlights its curve, as clicking the curve itself does
    label->onClick = [this, curve] { selectCurve(curve); };
    scene->addItem(label);
    auto* star = new MaxStar(curve, vbox, [this] { return viewRange(); });
    star->goTo(x, value, clock_());
    scene->addItem(star);
    maxMarkers_.append({label, line, star, vbox, &vals, std::move(format), x, value, x, value, true});
}

plot::Range SynopsisScreen::viewRange() const {
    return graph_->vb()->xRange();
}

bool SynopsisScreen::isZoomed(double start, double end) const {
    return end - start < totalSeconds_ - 0.5;
}

void SynopsisScreen::setTickSpacing(double span) {
    // major = minor spacing, coarser the more time is shown; under a minute the axis shows seconds too
    for (const auto& [longerThan, spacing] : {std::pair{7200, 900}, std::pair{3600, 600}, std::pair{1800, 300},
                                              std::pair{300, 60}, std::pair{120, 30}, std::pair{0, 10}}) {
        if (span > longerThan) {
            graph_->axis(plot::AxisItem::Bottom)->setTickSpacing(spacing, spacing);
            return;
        }
    }
}

void SynopsisScreen::updateSelection() {
    const plot::Range view = viewRange();
    setTickSpacing(view.span());
    stats_->setText(statsText(view.min, view.max));
    updateMaxMarkers(view.min, view.max);
    syncTimeEdits();
    layoutMaxLabels();
}

// the graph's time span, except in a box being typed in
void SynopsisScreen::syncTimeEdits() {
    const plot::Range view = viewRange();
    for (const auto& [edit, seconds] : {std::pair{startTimeEdit_, view.min}, std::pair{endTimeEdit_, view.max}}) {
        if (!edit->hasFocus()) {
            const QSignalBlocker blocker(edit);
            edit->setTime(QTime(0, 0).addSecs(roundInt(seconds)));
        }
    }
}

void SynopsisScreen::timeEditChanged(QTimeEdit* edited) {
    double start = QTime(0, 0).secsTo(startTimeEdit_->time());
    double end = QTime(0, 0).secsTo(endTimeEdit_->time());
    // at least the narrowest zoom, moving whichever box wasn't edited
    if (end - start < kMinZoomSeconds) {
        if (edited == startTimeEdit_) {
            end = std::min(start + kMinZoomSeconds, totalSeconds_);
            start = end - kMinZoomSeconds;
        } else {
            start = std::max(end - kMinZoomSeconds, 0.0);
            end = start + kMinZoomSeconds;
        }
    }
    graph_->vb()->setXRange(start, end, 0.0);
}

// a max scrolled out of view is replaced by the max of the readings in view
void SynopsisScreen::updateMaxMarkers(double start, double end) {
    const bool zoomed = isZoomed(start, end);
    for (MaxMarker& marker : maxMarkers_) {
        const QList<double>& times = marker.vals->first;
        const QList<double>& values = marker.vals->second;
        const bool selection = zoomed && !(start <= marker.fullX && marker.fullX <= end);
        bool shown = true;
        if (selection) {
            const qsizetype i0 = std::lower_bound(times.begin(), times.end(), start) - times.begin();
            const qsizetype i1 = std::upper_bound(times.begin(), times.end(), end) - times.begin();
            shown = i0 < i1;
            if (shown) {
                // (max() over the range: the first of equal ones)
                qsizetype best = i0;
                for (qsizetype i = i0; i < i1; ++i) {
                    if (values[i] > values[best]) {
                        best = i;
                    }
                }
                marker.x = times[best];
                marker.y = values[best];
            }
        } else {
            marker.x = marker.fullX;
            marker.y = marker.fullY;
        }
        marker.shown = shown;
        marker.star->onGraph = shown;
        marker.label->setVisible(shown);
        marker.line->setVisible(shown);
        if (!shown) {
            continue;
        }
        if (selection) {
            marker.label->setText(QStringLiteral("(Selection) ") + marker.format(marker.y));
            marker.label->setBorderColor(QColor(kSelectionColor), 2.5);
        } else {
            marker.label->setText(marker.format(marker.y));
            marker.label->setBorderColor(PlotLabel::kBorderColor, PlotLabel::kBorderWidth);
        }
        marker.star->setColor(selection ? kSelectionColor : MaxStar::kColor);
        marker.star->goTo(marker.x, marker.y, clock_());
    }
}

// each graph's top high enough for its stars over the session's maxes (a selection's are no higher), under the
// strips along the top
void SynopsisScreen::fitStarRoom() {
    double reserved = 0;
    for (BandStrip* strip : strips_) {
        reserved += strip->height();
    }
    for (const auto& [vbox, top] : {std::pair{graph_->powerVBox(), powerTop_}, std::pair{graph_->hrCadenceVBox(), hrCadenceTop_}}) {
        QList<std::pair<double, double>> rooms;
        for (const MaxMarker& marker : maxMarkers_) {
            if (marker.vbox == vbox) {
                rooms.append({marker.star->headroom(), marker.fullY});
            }
        }
        const double fitted = MaxStar::topWithRoom(vbox, top, rooms, reserved);
        if (std::abs(vbox->yRange().max - fitted) > 1e-6) {
            vbox->setYRange(0, fitted, 0.0);
        }
    }
}

void SynopsisScreen::animateStars() {
    if (!isVisible()) {
        return;
    }
    const double now = clock_();
    for (int i = 0; i < maxMarkers_.size(); ++i) {
        maxMarkers_[i].star->step(now, i);
    }
}

// ---- The stats ----

// the whole session's, or worked out again for the readings in view when zoomed in
QString SynopsisScreen::statsText(double start, double end) const {
    const bool zoomed = isZoomed(start, end);
    const Units& units = Units::current();
    QString text = zoomed ? QStringLiteral("(Selection)\n") : QString();
    if (showSpeed_) {
        double meters = totalMeters_;
        if (zoomed) {
            meters = 0;
            for (const double m : inView(speedDistance_, start, end).second) {
                meters += m;
            }
        }
        const double miles = meters * 0.000621371;
        text += QStringLiteral("Distance: %1 %2\n").arg(py::str(py::round(units.distanceFromMiles(miles), 2)), units.distanceUnit(false));
    }
    double rideSeconds = 0;
    for (const Ride& ride : rides_) {
        rideSeconds += ride.secondsPassed;
    }
    text += QStringLiteral("Total Time: ") + minimizedDuration(zoomed ? py::round(end - start) : rideSeconds) +
            QStringLiteral("\n");
    // averages over the recorded readings, only for sensors that recorded any
    const auto average = [&](const std::pair<QList<double>, QList<double>>& vals) -> std::optional<double> {
        const QList<double> values = inView(vals, start, end).second;
        if (values.isEmpty()) {
            return std::nullopt;
        }
        double total = 0;
        for (const double v : values) {
            total += v;
        }
        return total / static_cast<double>(values.size());
    };
    if (const std::optional<double> power = average(processedPowerVals_)) {
        text += QStringLiteral("Average Power: %1 W").arg(py::fixed(*power, 0)) + units.powerToWeightText(*power) +
                QStringLiteral("\n");
    }
    if (showHr_) {
        if (const std::optional<double> hr = average(processedHrVals_)) {
            text += QStringLiteral("Average HR: %1 bpm\n").arg(py::fixed(*hr, 0));
        }
    }
    if (showSpeed_) {
        if (const std::optional<double> speed = average(processedSpeedVals_)) {
            text += QStringLiteral("Average Speed: %1 %2\n").arg(py::fixed(*speed, 2), units.speedUnit());
        }
    }
    if (showCadence_) {
        if (const std::optional<double> cadence = average(processedCadenceVals_)) {
            text += QStringLiteral("Average Cadence: %1 RPM\n").arg(py::fixed(*cadence, 0));
        }
    }
    // the best powers; a selection has only the durations it spans
    QList<double> best = bestPowerRange_;
    if (zoomed) {
        best.clear();
        const auto [times, watts] = inView(processedPowerVals_, start, end);
        for (const int targetTime : kBestPowerSeconds) {
            const std::optional<double> value = findBestPowerRange(targetTime, &times, &watts);
            if (!value) {
                break;
            }
            best.append(py::round(*value, 2));
        }
    }
    text += QStringLiteral("Best sustained power values:\n");
    for (int i = 0; i < best.size(); ++i) {
        text += QStringLiteral("   ") + py::str(py::round(best[i], 2)) + kBestText[i] + units.powerToWeightText(best[i]) +
                QStringLiteral("\n");
    }
    while (text.endsWith(u'\n')) {
        text.chop(1);
    }
    return text;
}

// ---- Highlighting a curve ----

void SynopsisScreen::selectCurve(plot::CurveItem* curve) {
    // a toggle: clicking the highlighted line again clears it
    plot::CurveItem* previous = selectedCurve_;
    if (previous) {
        previous->setPen(curvePens_[previous].first);
        previous->setShadowPen(curvePens_[previous].second);
        previous->setZValue(0);
        selectedCurve_ = nullptr;
    }
    if (curve && curve != previous) {
        QPen pen = curvePens_[curve].first;
        pen.setWidthF(4);
        curve->setPen(pen);
        curve->setShadowPen(plot::mkPen(ftpcolors::haloColor(pen.color().name(), 0xff), 8));
        curve->setZValue(10);
        selectedCurve_ = curve;
    }
}

void SynopsisScreen::plotClicked(Qt::MouseButton button, bool taken) {
    // a left click no curve took clears the highlight, and closes an open block grade (a click on a block opens its
    // own, see GradeRegion)
    if (!taken && button == Qt::LeftButton) {
        selectCurve(nullptr);
        closeGradePopup();
    }
}

// ---- Placing the max labels ----

// whether a scene rect would cover any curve or target column
bool SynopsisScreen::rectHitsData(const QRectF& rect) const {
    const QRectF r = rect.adjusted(-3, -3, 3, 3);
    for (const PlotSeries& series : plotSeries_) {
        const QList<double>& xs = series.xs;
        const QList<double>& ys = series.ys;
        if (xs.size() < 2 || ys.isEmpty()) {
            continue;
        }
        const QPointF topLeft = series.vbox->mapSceneToView(r.topLeft());
        const QPointF bottomRight = series.vbox->mapSceneToView(r.bottomRight());
        const double xl = topLeft.x(), xr = bottomRight.x();
        const double yTop = topLeft.y(), yBot = bottomRight.y();
        if (series.columns) {
            // column i spans xs[i]..xs[i+1] and is filled from 0 up to ys[i]
            qsizetype i = std::max<qsizetype>((std::upper_bound(xs.begin(), xs.end(), xl) - xs.begin()) - 1, 0);
            while (i < ys.size() && xs[i] < xr) {
                if (xs[i + 1] > xl && ys[i] >= yBot && yTop >= 0) {
                    return true;
                }
                ++i;
            }
        } else {
            // each segment crossing the rect's x range, clipped to that range
            const qsizetype i0 = std::max<qsizetype>((std::lower_bound(xs.begin(), xs.end(), xl) - xs.begin()) - 1, 0);
            const qsizetype i1 = std::min<qsizetype>(std::upper_bound(xs.begin(), xs.end(), xr) - xs.begin(), xs.size() - 1);
            for (qsizetype i = i0; i < i1; ++i) {
                const double x0 = xs[i], y0 = ys[i], x1 = xs[i + 1], y1 = ys[i + 1];
                if (x1 < xl || x0 > xr) {
                    continue;
                }
                double lo, hi;
                if (x1 == x0) {
                    lo = std::min(y0, y1);
                    hi = std::max(y0, y1);
                } else {
                    const double ya = y0 + (y1 - y0) * (std::max(x0, xl) - x0) / (x1 - x0);
                    const double yb = y0 + (y1 - y0) * (std::min(x1, xr) - x0) / (x1 - x0);
                    lo = std::min(ya, yb);
                    hi = std::max(ya, yb);
                }
                if (hi >= yBot && lo <= yTop) {
                    return true;
                }
            }
        }
    }
    return false;
}

// each max label in the free spot nearest its marker: inside the plot area, not over another label, and not covering
// any curve or target column
void SynopsisScreen::layoutMaxLabels() {
    // (the strips along the top first: a new theme can change their size with the labels')
    layoutStrips();
    fitStarRoom();
    QRectF area = graph_->vb()->sceneBoundingRect();
    double reserved = 0;
    for (BandStrip* strip : strips_) {
        reserved += strip->height();
    }
    area.setTop(area.top() + reserved);
    if (area.width() < 20 || area.height() < 20) {
        return;
    }
    const double gap = 12;
    // the stars' resting spots too, so no label covers one
    QList<QRectF> placed;
    for (const MaxMarker& marker : maxMarkers_) {
        if (marker.shown) {
            placed.append(marker.star->restRect(marker.vbox->mapViewToScene(QPointF(marker.x, marker.y))));
        }
    }
    for (MaxMarker& marker : maxMarkers_) {
        if (!marker.shown) {
            continue;
        }
        // (the star marking it, resting over it)
        const QPointF pt = marker.star->restRect(marker.vbox->mapViewToScene(QPointF(marker.x, marker.y))).center();
        const QRectF br = marker.label->boundingRect();
        const double w = br.width(), h = br.height();
        const double maxLeft = std::max(area.left(), area.right() - w);
        const double maxTop = std::max(area.top(), area.bottom() - h);
        const auto clampLeft = [&](double l) { return std::min(std::max(l, area.left()), maxLeft); };
        const auto clampTop = [&](double t) { return std::min(std::max(t, area.top()), maxTop); };
        // the spots hugging the marker plus a grid over the whole plot area (sets: each position once, sorted)
        const double xStep = std::max(w / 4, 10.0), yStep = std::max(h / 2, 6.0);
        QList<double> lefts = {clampLeft(pt.x() + gap), clampLeft(pt.x() - gap - w)};
        for (int i = 0; i < static_cast<int>((maxLeft - area.left()) / xStep) + 1; ++i) {
            lefts.append(area.left() + i * xStep);
        }
        lefts.append(maxLeft);
        QList<double> tops = {clampTop(pt.y() - gap - h), clampTop(pt.y() + gap)};
        for (int i = 0; i < static_cast<int>((maxTop - area.top()) / yStep) + 1; ++i) {
            tops.append(area.top() + i * yStep);
        }
        tops.append(maxTop);
        for (QList<double>* list : {&lefts, &tops}) {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end()), list->end());
        }
        const auto distance = [&](const QRectF& c) {
            const double dx = c.center().x() - pt.x(), dy = c.center().y() - pt.y();
            return dx * dx + dy * dy;
        };
        QList<QRectF> candidates;
        for (const double l : lefts) {
            for (const double t : tops) {
                candidates.append(QRectF(l, t, w, h));
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&](const QRectF& a, const QRectF& b) { return distance(a) < distance(b); });
        QList<QRectF> notOnLabels;
        for (const QRectF& c : candidates) {
            if (std::none_of(placed.begin(), placed.end(), [&](const QRectF& p) { return c.intersects(p); })) {
                notOnLabels.append(c);
            }
        }
        std::optional<QRectF> rect;
        for (const QRectF& c : notOnLabels) {
            if (!rectHitsData(c)) {
                rect = c;
                break;
            }
        }
        if (!rect) {
            // too crowded for a fully clear spot: at least clear of the other labels
            rect = notOnLabels.isEmpty() ? candidates.front() : notOnLabels.front();
        }
        placed.append(rect->adjusted(-2, -2, 2, 2));
        marker.label->setPos(rect->left() - br.left(), rect->top() - br.top());
        // the leader line from the marker to the nearest edge of its label
        const QPointF nearest(std::min(std::max(pt.x(), rect->left()), rect->right()),
                              std::min(std::max(pt.y(), rect->top()), rect->bottom()));
        marker.line->setLine(QLineF(pt, nearest));
        marker.line->setVisible(area.contains(pt));
    }
}

}  // namespace awo

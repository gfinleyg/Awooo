// Renders the C++ app's custom widgets to PNG files, set up as the screens set them up and fed the same made-up readings
// as render_python.py, so the two can be compared side by side (compare.py).
//
//     render_widgets.exe OUT_DIR [SCENE ...]
//
// Scenes are listed in kScenes below; all of them by default.

#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>

#include "AnalogGauge.hpp"
#include "AntBridge.hpp"
#include "AppLayout.hpp"
#include "CadenceWorkout.hpp"
#include "FtpColors.hpp"
#include "FtpColorsEditor.hpp"
#include "MusicSelectDialog.hpp"
#include "AppState.hpp"
#include "CalibratePowerWindow.hpp"
#include "ColorPicker.hpp"
#include "DeviceDialogs.hpp"
#include "WorkoutSelectDialog.hpp"
#include "FakeAnt.hpp"
#include "SetupScreen.hpp"
#include "WorkoutHelpers.hpp"
#include "GradeWidgets.hpp"
#include "HudScreen.hpp"
#include "Session.hpp"
#include "SimulatedRider.hpp"
#include "SynopsisScreen.hpp"
#include "WorkoutData.hpp"
#include "LongVLabels.hpp"
#include "PowerSpiral.hpp"
#include "SmoothProgressBar.hpp"
#include "AppStyle.hpp"
#include "MaxStar.hpp"
#include "Performance.hpp"
#include "PyCompat.hpp"
#include "PlotItems.hpp"
#include "SolidColoredColumns.hpp"
#include "PreviewRide.hpp"
#include "Theme.hpp"
#include "ThemeSelectDialog.hpp"
#include "ViewerDisplay.hpp"
#include "WorkoutGraph.hpp"
#include "WorkoutHelpers.hpp"
#include "ZoneColors.hpp"

using namespace awo;

namespace {

// ---- The made-up ride (the same formulas as render_python.py) ----

double ridePower(double t) {
    return 150 + 60 * std::sin(t / 20) + 20 * std::sin(t / 3);
}

double rideHr(double t) {
    return 120 + 20 * std::sin(t / 50);
}

double rideCadence(double t) {
    return 85 + 5 * std::sin(t / 7);
}

struct Series {
    QList<double> times, values;
};

Series rideSeries(const std::function<double(double)>& fn, double seconds, std::optional<std::pair<double, double>> gap = {}) {
    Series series;
    for (int i = 0; i < static_cast<int>(seconds * 4) + 1; ++i) {
        const double t = i / 4.0;
        if (gap && gap->first < t && t < gap->second) {
            continue;
        }
        series.times.append(t);
        series.values.append(py::round(fn(t)));
    }
    return series;
}

void settle() {
    for (int i = 0; i < 3; ++i) {
        QApplication::processEvents();
    }
}

QWidget* shown(QWidget* widget, int width, int height) {
    // (themed as the screens' fitting themes everything it builds, see layout::applyLayout)
    theme::apply();
    widget->setAttribute(Qt::WA_DontShowOnScreen);
    widget->resize(width, height);
    widget->show();
    settle();
    return widget;
}

// ---- Scenes ----

// The HUD's progress graph, 300 s into a workout
QWidget* sceneHudGraph() {
    constexpr double kMarkerSize = 11;
    constexpr double kMarkerReach = kMarkerSize / 2 + 0.5;
    AppState& state = appState();
    auto* graph = new WorkoutGraph;
    graph->vb()->setMouseEnabled(false, false);
    graph->powerVBox()->setMouseEnabled(false, false);
    graph->hrCadenceVBox()->setMouseEnabled(false, false);
    graph->updateViews();
    QObject::connect(graph->vb(), &plot::ViewBox::resized, graph, &WorkoutGraph::updateViews);
    graph->setXRange(0, state.graphSecondsShown);
    graph->setYRange(0, 1000, 0.0);
    graph->setMaximumHeight(200);
    graph->plotScreen()->setTitle(std::nullopt);
    graph->plotScreen()->showAxis(plot::AxisItem::Left, false);
    graph->plotScreen()->showAxis(plot::AxisItem::Right, false);
    QFont tickFont(graph->font());
    if (tickFont.pointSizeF() > 0) {
        tickFont.setPointSizeF(tickFont.pointSizeF() * 0.8);
    } else {
        tickFont.setPixelSize(std::max(1, static_cast<int>(py::round(tickFont.pixelSize() * 0.8))));
    }
    graph->axis(plot::AxisItem::Bottom)->setTickFont(tickFont);
    plot::AxisItem::Style styles = graph->axisStyles();
    styles[1].second = QStringLiteral("16px");
    graph->plotScreen()->setLabel(plot::AxisItem::Bottom, QStringLiteral("Time (minutes)"), styles);
    auto* upcoming = new plot::SolidColoredColumns;
    graph->powerVBox()->addItem(upcoming);
    auto* completed = new plot::SolidColoredColumns;
    graph->powerVBox()->addItem(completed);
    auto* powerPlot = new plot::CurveItem;
    graph->powerVBox()->addItem(powerPlot);
    auto* hrPlot = new plot::CurveItem;
    graph->hrCadenceVBox()->addItem(hrPlot);
    auto* cadencePlot = new plot::CurveItem;
    graph->hrCadenceVBox()->addItem(cadencePlot);
    graph->setBackground(std::nullopt);
    graph->viewport()->setAutoFillBackground(false);
    theme::setSheet(graph, QStringLiteral("QGraphicsView { border: 0px; background: transparent; }"));
    for (const auto side : {plot::AxisItem::Bottom, plot::AxisItem::Left}) {
        graph->axis(side)->setPen(plot::mkPen(QColor(0, 0, 0)));
        graph->axis(side)->setTextPen(plot::mkPen(QColor(0, 0, 0)));
    }
    graph->plotScreen()->showGrid(true, true, 0.25);
    auto* nowLine = new plot::InfiniteLine(0, 90);
    nowLine->setPen(theme::themedPen(plot::mkPen(QColor(QStringLiteral("#bdd6de")), 1.5)));
    nowLine->addMarker(QStringLiteral("v"), 0.0, 14);
    nowLine->addMarker(QStringLiteral("^"), 1.0, 14);
    nowLine->setZValue(5);
    graph->powerVBox()->addItem(nowLine);
    struct Source {
        plot::ViewBox* vbox;
        QString color;
        plot::CurveItem* curve;
        QString format;
    };
    const QList<Source> sources = {
        {graph->powerVBox(), ftpcolors::kSensorPower, powerPlot, QStringLiteral("Max Pwr: %1 W")},
        {graph->hrCadenceVBox(), ftpcolors::kSensorHr, hrPlot, QStringLiteral("Max HR: %1 bpm")},
        {graph->hrCadenceVBox(), ftpcolors::kSensorCadence, cadencePlot, QStringLiteral("Max Cad: %1 rpm")},
    };
    QList<plot::ScatterItem*> markers;
    QList<MaxStar*> stars;
    QList<MaxBubble*> bubbles;
    for (const Source& source : sources) {
        auto* marker = new plot::ScatterItem(QStringLiteral("d"), kMarkerSize);
        marker->setBrush(QColor(source.color));
        marker->setPen(zones::markerOutline(source.color));
        marker->setZValue(20);
        source.vbox->addItem(marker);
        markers.append(marker);
        auto* star = new MaxStar(source.curve, source.vbox, [graph] { return graph->vb()->xRange(); });
        graph->scene()->addItem(star);
        stars.append(star);
        auto* bubble = new MaxBubble(star, source.format);
        graph->scene()->addItem(bubble);
        bubbles.append(bubble);
    }
    auto* holder = new QWidget;
    theme::setSheet(holder, QStringLiteral("background: #bdd6de;"));
    auto* layout = new QVBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(graph);
    shown(holder, 1800, 200);

    const double now = 300;
    state.secondsPassed = now;
    const double diff = std::max(0.0, now - (state.graphSecondsShown - state.graphBreakpoint));
    graph->setXRange(diff, diff + state.graphSecondsShown);
    nowLine->setValue(now);
    const QList<double> blocks = {0, 120, 300, 420, 600, 720};
    const QList<double> watts = {120, 200, 150, 260, 180};
    const QList<Series> series = {rideSeries(ridePower, now, std::pair(200.0, 210.0)), rideSeries(rideHr, now),
                                  rideSeries(rideCadence, now)};
    for (qsizetype i = 0; i < sources.size(); ++i) {
        const auto [xs, ys] = withDropoutsAsZero(series[i].times, series[i].values, now);
        const auto [pen, halo] = zones::sensorPens(sources[i].color);
        sources[i].curve->setData(xs, ys, pen, halo);
        markers[i]->setData({xs.back()}, {ys.back()});
        const QList<double>& values = series[i].values;
        qsizetype index = 0;
        for (qsizetype j = 0; j < values.size(); ++j) {
            if (values[j] >= values[index]) {
                index = j;
            }
        }
        stars[i]->goTo(series[i].times[index], values[index], 0.0);
        stars[i]->markerReach = stars[i]->x == series[i].times.back() ? kMarkerReach : 0.0;
    }
    const double powerMax =
        std::max(*std::max_element(watts.begin(), watts.end()),
                 *std::max_element(series[0].values.begin(), series[0].values.end())) + 100;
    const double hrCadenceMax =
        std::max(*std::max_element(series[1].values.begin(), series[1].values.end()),
                 *std::max_element(series[2].values.begin(), series[2].values.end())) + 20;
    const ftpcolors::ZoneRanges ranges = state.ftpRanges;
    auto colorer = [ranges](ftpcolors::GraphState graphState) {
        return [ranges, graphState](double value, const QList<double>&) {
            return zones::graphColor(value, ranges, graphState);
        };
    };
    upcoming->setData(blocks.mid(2), watts.mid(2), ranges.limits, colorer(ftpcolors::GraphState::Upcoming));
    completed->setData(blocks.mid(0, 3), watts.mid(0, 2), ranges.limits, colorer(ftpcolors::GraphState::Past));
    settle();
    for (plot::ViewBox* vbox : {graph->powerVBox(), graph->hrCadenceVBox()}) {
        QList<std::pair<double, double>> rooms;
        for (MaxBubble* bubble : bubbles) {
            if (bubble->star()->vbox() == vbox) {
                rooms.append({bubble->headroom(), *bubble->star()->y});
            }
        }
        vbox->setYRange(0, MaxStar::topWithRoom(vbox, vbox == graph->powerVBox() ? powerMax : hrCadenceMax, rooms), 0.0);
    }
    settle();
    // the stars at rest (a first goTo puts them straight there) at a time their twinkle is at its middle size, and
    // the bubbles beside them grown in
    for (int phase = 0; phase < stars.size(); ++phase) {
        stars[phase]->step(((3 + phase) * std::numbers::pi - phase * 2.1) / 3.6, phase);
    }
    for (const double when : {10.0, 10.0 + MaxBubble::kAppearSeconds}) {
        QList<QRectF> avoid;
        for (MaxStar* star : stars) {
            if (star->isVisible()) {
                avoid.append(star->sceneBoundingRect());
            }
        }
        for (MaxBubble* bubble : bubbles) {
            bubble->step(when, avoid);
        }
    }
    settle();
    if (qEnvironmentVariableIsSet("AWO_PARITY_DEBUG")) {
        for (plot::ViewBox* view : {graph->powerVBox(), graph->hrCadenceVBox(), graph->vb()}) {
            std::fprintf(stderr, "x %.6f %.6f y %.6f %.6f geometry %.2f %.2f %.2f %.2f\n", view->xRange().min, view->xRange().max,
                  view->yRange().min, view->yRange().max, view->sceneBoundingRect().x(), view->sceneBoundingRect().y(),
                  view->sceneBoundingRect().width(), view->sceneBoundingRect().height());
        }
    }
    return holder;
}

// ---- The gauges, as the HUD sets them up ----

const QColor kGaugeRed(QStringLiteral("#8B0000"));

QWidget* gaugeShown(Gauge* gauge) {
    return shown(gauge, 400, 400);
}

QGradientStops arcColors(const std::pair<QString, QString>& colors) {
    return {{0, QColor(QStringLiteral("#88") + colors.first)}, {.5, QColor(QStringLiteral("#88") + colors.second)}};
}

// The HUD's power gauge at 230 W, a 250 W target arc, rings for the last few blocks
QWidget* scenePowerGauge() {
    auto* gauge = new Gauge;
    AnalogGauge* w = gauge->gauge();
    w->setNeedleColor(kGaugeRed);
    w->setGaugeTheme(3);
    w->setOuterCircleBg({{0.25, QColor(0, 0, 0, 0)}, {0.18, QColor(255, 255, 255, 255)}});
    w->setEnableBarGraph(false);
    w->units = QStringLiteral("Watts");
    w->bigNeedleCenterSize = 5;
    w->altUnitsDisplay = true;
    w->fitValueInCenter = true;
    w->centerRingsPale = {0.1, 0.4};
    w->centerRingCount = 3;
    w->centerRingMinPx = 6;
    w->setValueColors(zones::zoneStops(zones::powerRanges()));
    w->setValue(230);
    w->setOuterCircleBg({{0.25, QColor(0, 0, 0, 0)}, {0.18, QColor(QStringLiteral("#") + zones::gaugeRing(230))}});
    QList<QColor> rings;
    for (const double power : {230, 180, 140, 260, 90}) {
        rings.append(zones::zoneColor(power));
    }
    w->setCenterRings(rings);
    w->setArc(250, arcColors(zones::gaugeArc(250)));
    return gaugeShown(gauge);
}

void cadenceGaugeLook(AnalogGauge* w) {
    w->setNeedleColor(kGaugeRed);
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
}

// The HUD's cadence gauge (workout mode) at 87.4 RPM
QWidget* sceneCadenceGauge() {
    auto* gauge = new Gauge(QStringLiteral("Chainring.svg"), std::nullopt, true);
    gauge->chainring()->spinTimer()->stop();
    cadenceGaugeLook(gauge->gauge());
    gauge->gauge()->rescale();
    gauge->gauge()->setValue(87.4);
    return gaugeShown(gauge);
}

// Musical Cadence Mode's big cadence gauge at 92.3 RPM, a 95 RPM target, rings on a half beat
QWidget* sceneCadenceModeGauge() {
    auto* gauge = new Gauge(QStringLiteral("Chainring.svg"), std::nullopt, true);
    gauge->chainring()->spinTimer()->stop();
    AnalogGauge* w = gauge->gauge();
    cadenceGaugeLook(w);
    w->setMaxValue(140);
    w->setScalaCount(14);
    w->initialScaleFontsize = 13;
    w->gaugeColorInnerRadiusFactor = 0.95;
    w->setEnableBarGraph(false);
    gauge->chainring()->scale = w->gaugeColorInnerRadiusFactor;
    const ftpcolors::ZoneRanges cadenceRanges{{60, 75, 90, 105}, true};
    w->setValueColors(zones::zoneStops(cadenceRanges));
    const QColor color(QStringLiteral("#") + zones::gaugeRing(95, cadenceRanges));
    w->setCenterRings(QList<QColor>{color.darker(130), color}, 0.5);
    w->setArc(95, arcColors(zones::gaugeArc(95, cadenceRanges)));
    w->rescale();
    w->setValue(92.3);
    return gaugeShown(gauge);
}

// The HUD's heart rate gauge at 132 BPM, the heart at rest
QWidget* sceneHrGauge() {
    auto* gauge = new Gauge(QStringLiteral("HeartSmaller.svg"),
                            Gauge::Beat{QStringLiteral("Heart.svg"), QColor(QStringLiteral("#FF8FAB")),
                                        QColor(QStringLiteral("#E8102A"))});
    gauge->heart()->beatTimer()->stop();
    AnalogGauge* w = gauge->gauge();
    w->setNeedleColor(kGaugeRed);
    w->needleCenterBg = {{0, QColor(QStringLiteral("#FFFFFF"))}};
    w->setScalePolygonColors(QGradientStops{{0, QColor(0, 0, 0, 0)}});
    w->setOuterCircleBg({{0, QColor(0, 0, 0, 0)}});
    w->bigNeedleCenterSize = 20;
    w->units = QStringLiteral("BPM");
    w->setMinValue(40);
    w->setMaxValue(200);
    w->setEnableBarGraph(false);
    w->setScalaCount(2);
    w->scalaSubdivCount = 5;
    w->setValue(132);
    return gaugeShown(gauge);
}

// ---- The workout screen's other widgets ----

// The workout screen's spiral 200 s into a 10 minute workout after a 5 minute ride (at a higher FTP), 80 s into its
// second 2 minute block, with a stretch without cadence
PowerSpiral* makePowerSpiral() {
    const ftpcolors::ZoneRanges now = zones::powerRanges();
    ftpcolors::ZoneRanges then;
    for (const double limit : now.limits) {
        then.limits.append(limit * 1.2);
    }
    session::RingHistory ride{300, {}, then};
    for (int t = 0; t < 300; ++t) {
        ride.samples.append({t + 0.5, 250 + 50 * std::sin(t / 30.0), 90 + 10 * std::sin(t / 9.0)});
    }
    auto* spiral = new PowerSpiral;
    spiral->clear(600, {ride});
    spiral->setBlock(120);
    for (int i = 0; i <= 400; ++i) {
        const double t = i * 0.5;
        if (t == 120) {
            spiral->setBlock(120);
        }
        const std::optional<double> cadence =
            100 <= t && t < 110 ? std::nullopt : std::optional(85 + 15 * std::sin(t / 11));
        spiral->addSample(t, t < 120 ? t : t - 120, 150 + 80 * std::sin(t / 15), cadence);
    }
    spiral->setProgress(200, 600, 80, 120);
    return spiral;
}

QWidget* scenePowerSpiral() {
    return shown(makePowerSpiral(), 400, 400);
}

// A block's grade over the spiral, before it fades
QWidget* sceneGradeOverlay() {
    PowerSpiral* spiral = makePowerSpiral();
    auto* overlay = new GradeOverlay(spiral);
    overlay->showGrade(QStringLiteral("A+"));
    for (QTimer* timer : overlay->findChildren<QTimer*>()) {
        timer->stop();
    }
    return shown(spiral, 400, 400);
}

QList<GradeList::Row> gradeRows() {
    using Kind = GradeList::Row::Kind;
    auto row = [](Kind kind, const QString& label, std::optional<double> pct, const QString& grade,
                  const QString& note = {}, bool indent = false) {
        GradeList::Row r;
        r.kind = kind;
        r.label = label;
        r.pct = pct;
        r.grade = grade;
        r.note = note;
        r.indent = indent;
        return r;
    };
    return {row(Kind::Ride, QStringLiteral("Workout: Test Ride"), 91.2, QStringLiteral("A"), QStringLiteral("45m")),
            row(Kind::Block, QStringLiteral("5:00 @ 150 Watts"), 95, QStringLiteral("A+")),
            row(Kind::Interval, QStringLiteral("3x"), std::nullopt, {}),
            row(Kind::Sub, QStringLiteral("1:00 @ 300 Watts"), 80, QStringLiteral("B"),
                QStringLiteral("S · A · B+")),
            row(Kind::Sub, QStringLiteral("2:00 @ 120 Watts"), 99, QStringLiteral("S")),
            row(Kind::Song, QStringLiteral("Artist - Title"), 70, QStringLiteral("C+")),
            row(Kind::Block, QStringLiteral("3:00 @ 200-250 Watts"), 60, QStringLiteral("D"),
                QStringLiteral("ended early"), true),
            row(Kind::Block, QStringLiteral("Free Ride"), 88, QStringLiteral("A-"))};
}

// The grade list's kinds of rows, and the average
QWidget* sceneGradeList() {
    auto* grades = new GradeList;
    grades->setRows(gradeRows(), 85.3);
    return shown(grades, 600, 500);
}

// The grade list scrolled, with "more..." above and below
QWidget* sceneGradeListScrolled() {
    auto* grades = new GradeList;
    grades->setRows(gradeRows() + gradeRows() + gradeRows(), 85.3);
    grades->scrollTo(4);
    return shown(grades, 600, 400);
}

// The workout screen's timer bar part of the way along, between pixels
QWidget* sceneSmoothBar() {
    auto* bar = new SmoothProgressBar;
    bar->setRange(0, 100 * 600);
    bar->setTextVisible(false);
    bar->setFraction(0.4373);
    bar->setBarColors(QColor(QStringLiteral("#1f15e1")), QColor(QStringLiteral("#bdd6de")));
    return shown(bar, 301, 34);
}

// The coming up list, longer than fits: scrolled one down, "more..." above and below
QWidget* sceneLongLabels() {
    const QStringList lines = {"5:00 @ 150 Watts", "1:00 @ 300 Watts", "2:00 @ 120 Watts", "3:00 @ 200-250 Watts",
                               "Free Ride",        "4:00 @ 180 Watts", "0:30 @ 400 Watts", "6:00 @ 90 Watts",
                               "2:00 @ 260 Watts", "1:00 @ 140 Watts"};
    auto* labels = new LongVLabels(appState().ftpRanges);
    labels->setFullText(lines.join(u'\n') + u'\n');
    shown(labels, 500, 300);
    labels->scrollTo(1);
    settle();
    return labels;
}

// ---- The screens ----

// a bridge that has found two power meters and a heart rate monitor, and nothing more
class StubBridge : public ant::AntBridge {
public:
    explicit StubBridge(awotest::FakeAnt& fake)
        : AntBridge(
              [transport = fake.stick](antnative::Stick::MessageHandler sink) {
                  transport->sink = std::move(sink);
                  return transport;
              },
              fake.clock, [](double) {}) {}
    QStringList foundDevices() const override { return {"11 100", "11 101", "120 200"}; }
    bool isInitialized() const override { return true; }
    double scanSettleRemaining() const override { return 0; }
    std::optional<QString> takeError() override { return std::nullopt; }
    std::optional<QString> waitingForStick() const override { return std::nullopt; }
    QSet<ant::Role> selectedRoles() const override { return {}; }
    bool isConnectRequested() const override { return false; }
    bool isReady() const override { return false; }
    QStringList lowBatteryWarnings() const override { return {}; }
    std::optional<ant::DeviceReading> selectedDevice(ant::Role) const override { return std::nullopt; }
    void requestConnect(const ant::Selection&) override {}
};

QString firstShippedWorkout() {
    QDir folder(QStringLiteral(AWO_PYTHON_APP_DIR "/awoWorkouts"));
    QStringList names = folder.entryList(QDir::Files);
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
    return folder.filePath(names.front());
}

// The setup screen with the saved settings, two power meters and a heart rate monitor found (the heart rate monitor
// picked by itself), the first shipped workout loaded, laid out at 1920x1116
QWidget* sceneSetupScreen() {
    static awotest::FakeAnt fake;
    static StubBridge bridge(fake);
    QFile settingsFile(QStringLiteral("savedSettingsANT.json"));
    settingsFile.open(QIODevice::WriteOnly);
    settingsFile.write(QJsonDocument(QJsonObject{{"wheelCircumference", 2.1}, {"ftp", 250}, {"maxHr", 185}}).toJson());
    settingsFile.close();
    auto* setup = new SetupScreen(&bridge);
    // (its timers stopped: the check is run once here)
    for (QTimer* timer : setup->findChildren<QTimer*>()) {
        timer->stop();
    }
    setup->pickWorkoutFile(firstShippedWorkout());
    setup->initAntScanConnect();
    auto* holder = new QWidget;
    holder->setObjectName(QStringLiteral("holder"));
    theme::setSheet(holder, QStringLiteral("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 "
                                           "#28275e stop:1 #9fc5d1);}"));
    auto* layout = new QVBoxLayout(holder);
    layout->addWidget(setup);
    layout::applyLayout(holder, {{QStringLiteral("Init"), setup}}, 1.0);
    shown(holder, 1920, 1116);
    return holder;
}

// a popup at the size it gives itself
QWidget* shownAsIs(QWidget* widget) {
    theme::apply();
    widget->setAttribute(Qt::WA_DontShowOnScreen);
    widget->show();
    settle();
    return widget;
}

void writeSceneSettings() {
    QFile file(QStringLiteral("savedSettingsANT.json"));
    file.open(QIODevice::WriteOnly);
    file.write(QJsonDocument(QJsonObject{{"wheelCircumference", 2.1}, {"ftp", 250}, {"maxHr", 185}}).toJson());
}

// Picking a speed/cadence source: a combined meter, a cadence meter and two speed meters found, the cadence meter
// picked before
QWidget* sceneSelectDevices() {
    const QStringList found = {"121 400", "122 300", "123 301", "123 302"};
    selectedDevices::write({"122 300"});
    auto* window = new SelectDevicesWindow(found, SelectDevicesWindow::Kind::SpeedCadence, found);
    for (QTimer* timer : window->findChildren<QTimer*>()) {
        timer->stop();
    }
    shownAsIs(window);
    selectedDevices::clear();
    return window;
}

class ConnectBridge : public StubBridge {
public:
    using StubBridge::StubBridge;
    QStringList foundDevices() const override { return {"11 100", "121 400"}; }
};

// The connect prompt: a power meter picked and found, a heart rate monitor picked and not, a speed/cadence meter
// found and not picked
QWidget* sceneConnectDevices() {
    static awotest::FakeAnt fake;
    static ConnectBridge bridge(fake);
    selectedDevices::write({"11 100", "120 200"});
    auto* window = new ConnectDevicesWindow(&bridge);
    shownAsIs(window);
    selectedDevices::clear();
    return window;
}

// Calibrate Power with no power meter connected
QWidget* sceneCalibratePower() {
    static awotest::FakeAnt fake;
    static StubBridge bridge(fake);
    writeSceneSettings();
    auto* window = new CalibratePowerWindow(&bridge);
    for (QTimer* timer : window->findChildren<QTimer*>()) {
        timer->stop();
    }
    return shownAsIs(window);
}

// The workout picker listing the first three shipped workouts, the second picked
QWidget* sceneWorkoutPicker() {
    writeSceneSettings();
    QDir folder(QStringLiteral(AWO_PYTHON_APP_DIR "/awoWorkouts"));
    QStringList names = folder.entryList(QDir::Files);
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
    QJsonArray files;
    for (const QString& name : names.mid(0, 3)) {
        files.append(QDir::cleanPath(folder.filePath(name)));
    }
    QFile list(kWorkoutListFile);
    list.open(QIODevice::WriteOnly);
    list.write(QJsonDocument(QJsonObject{{"files", files}, {"last", files[1]}}).toJson(QJsonDocument::Compact));
    list.close();
    // (as the setup screen leaves it: no workout loaded yet)
    appState().workoutFilename.clear();
    return shownAsIs(new WorkoutSelectDialog);
}

// The graph's stars at rest (a first goTo puts them straight there) at a time their twinkle is at its middle size,
// and the bubbles beside them grown in
void restStars(const QList<std::pair<MaxStar*, QString>>& stars, const QList<MaxBubble*>& bubbles) {
    for (int phase = 0; phase < stars.size(); ++phase) {
        stars[phase].first->step(((3 + phase) * std::numbers::pi - phase * 2.1) / 3.6, phase);
    }
    for (const double when : {10.0, 10.0 + MaxBubble::kAppearSeconds}) {
        QList<QRectF> avoid;
        for (const auto& [star, _] : stars) {
            if (star->isVisible()) {
                avoid.append(star->sceneBoundingRect());
            }
        }
        for (MaxBubble* bubble : bubbles) {
            bubble->step(when, avoid);
        }
    }
}

// The workout screen 600 s into the first shipped workout, ridden by the simulated rider (seed 3) a second a tick,
// laid out at 1920x1116
QWidget* sceneHudScreen() {
    static awotest::FakeAnt fake;
    static StubBridge bridge(fake);
    writeSceneSettings();
    auto* setup = new SetupScreen(&bridge);
    for (QTimer* timer : setup->findChildren<QTimer*>()) {
        timer->stop();
    }
    AppState& state = appState();
    state.secondsPassed = 0;
    state.workoutPowerRequestsSeconds.clear();
    state.workoutPowerRequestsWatts.clear();
    state.workoutPowerRequestsSysTime.clear();
    state.savedPowerVals = {};
    state.savedHRVals = {};
    state.savedCadenceVals = {};
    state.totalRevolutions = 0;
    state.sessionRides.clear();
    state.rideStart.reset();
    state.rideRecorded = false;
    workoutData().clear();
    state.ftp = 250;
    state.maxHr = 185;
    state.wheelCircumference = 2.105;
    state.workoutMode = true;
    state.cadenceMode = false;
    state.ftpTestMode.clear();
    state.antHRM = true;
    state.antSpeedCadence = true;
    state.antCadence = false;
    state.antSpeed = false;
    state.ftpRanges = zones::powerRanges();
    const QString path = firstShippedWorkout();
    setup->loadWorkoutFile(path);
    state.workoutFilename = path;
    state.workoutFilenameShort = path.split(u'/').last();
    state.startTimestamp = QDateTime::currentDateTime();
    delete setup;
    auto* hud = new HudScreen(&bridge);
    SimulatedRider rider(3);
    hud->runSimulatedRide(600, rider);
    hud->stopTimers();
    for (QTimer* timer : hud->gradeOverlay()->findChildren<QTimer*>()) {
        timer->stop();
    }
    hud->cadenceGauge()->chainring()->spinTimer()->stop();
    hud->heartRateGauge()->heart()->beatTimer()->stop();
    auto* holder = new QWidget;
    holder->setObjectName(QStringLiteral("holder"));
    theme::setSheet(holder, QStringLiteral("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 "
                                           "#28275e stop:1 #9fc5d1);}"));
    auto* layout = new QVBoxLayout(holder);
    layout->addWidget(hud);
    layout::applyLayout(holder, {{QStringLiteral("Workout"), hud}}, 1.0);
    shown(holder, 1920, 1116);
    restStars(hud->maxStars(), hud->maxBubbles());
    settle();
    return holder;
}

// The synopsis of the first shipped workout ridden start to finish by the simulated rider (seed 11) a second a tick,
// its readings recorded as LayoutViewer records them, laid out at 1920x1116
QWidget* sceneSynopsisScreen() {
    static awotest::FakeAnt fake;
    static StubBridge bridge(fake);
    writeSceneSettings();
    auto* setup = new SetupScreen(&bridge);
    for (QTimer* timer : setup->findChildren<QTimer*>()) {
        timer->stop();
    }
    AppState& state = appState();
    state.secondsPassed = 0;
    state.workoutPowerRequestsSeconds.clear();
    state.workoutPowerRequestsWatts.clear();
    state.workoutPowerRequestsSysTime.clear();
    state.savedPowerVals = {};
    state.savedHRVals = {};
    state.savedCadenceVals = {};
    state.totalRevolutions = 0;
    state.sessionRides.clear();
    state.rideStart.reset();
    state.rideRecorded = false;
    workoutData().clear();
    state.ftp = 250;
    state.maxHr = 185;
    state.wheelCircumference = 2.105;
    state.workoutMode = true;
    state.cadenceMode = false;
    state.ftpTestMode.clear();
    state.antHRM = true;
    state.antSpeedCadence = true;
    state.antCadence = false;
    state.antSpeed = false;
    state.ftpRanges = zones::powerRanges();
    const QString path = firstShippedWorkout();
    setup->loadWorkoutFile(path);
    state.workoutFilename = path;
    state.workoutFilenameShort = path.split(u'/').last();
    const QDateTime startTimestamp(QDate(2026, 10, 26), QTime(8, 0, 0));
    state.startTimestamp = startTimestamp;
    const double startSeconds = startTimestamp.toMSecsSinceEpoch() / 1000.0;
    double revolutions = 0;
    const auto record = [&](double t, const RiderReading& reading) {
        const double ts = startSeconds + t;
        WorkoutData& data = workoutData();
        data.power.appendAt(ts, {static_cast<double>(reading.power)});
        data.heartRate.appendAt(ts, {static_cast<double>(reading.heartRate)});
        data.fallbackCadence.appendAt(ts, {py::round(reading.cadence)});
        // speed sensor counters: cumulative revolutions and the 1/1024 s time of the last one
        const double revsPerSecond = reading.speedMph / 2.23694 / state.wheelCircumference;
        revolutions += revsPerSecond;
        const double lastRevTime = t - (revolutions - std::trunc(revolutions)) / std::max(revsPerSecond, 0.01);
        data.speed.appendAt(ts, {static_cast<double>(static_cast<long long>(revolutions) & 0xFFFF),
                                 static_cast<double>(static_cast<long long>(lastRevTime * 1024) & 0xFFFF)});
    };
    {
        HudScreen hud(&bridge, nullptr, [] { return 1000.0; });
        SimulatedRider rider(11);
        hud.runSimulatedRide(state.workoutDurationSeconds + 1, rider, record);
        hud.stopTimers();
    }
    state.startTimestamp = startTimestamp;
    state.workoutPowerRequestsSysTime.clear();
    for (const double seconds : state.workoutPowerRequestsSeconds) {
        state.workoutPowerRequestsSysTime.append(startTimestamp.addMSecs(static_cast<qint64>(py::round(seconds * 1000))));
    }
    session::addCurrentRide();
    auto* synopsis = new SynopsisScreen(&bridge, setup, [] { return 0.0; });
    for (QTimer* timer : synopsis->findChildren<QTimer*>(Qt::FindDirectChildrenOnly)) {
        timer->stop();
    }
    auto* holder = new QWidget;
    setup->setParent(holder);
    setup->hide();
    holder->setObjectName(QStringLiteral("holder"));
    theme::setSheet(holder, QStringLiteral("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 "
                                           "#28275e stop:1 #9fc5d1);}"));
    auto* layout = new QVBoxLayout(holder);
    layout->addWidget(synopsis);
    layout::applyLayout(holder, {{QStringLiteral("Synopsis"), synopsis}}, 1.0);
    shown(holder, 1920, 1116);
    synopsis->layoutMaxLabels();
    for (int phase = 0; phase < synopsis->maxMarkers().size(); ++phase) {
        synopsis->maxMarkers()[phase].star->step(((3 + phase) * std::numbers::pi - phase * 2.1) / 3.6, phase);
    }
    settle();
    return holder;
}

// LayoutViewer's made up songs (VIEWER_SONGS, viewer_music_workout): each its blocks (seconds, rpm)
const QList<std::pair<QString, QList<std::pair<int, int>>>> kViewerSongs = {
    {QStringLiteral("Viewer Band - Warm Up Groove"), {{30, 0}, {90, 82}, {60, 88}}},
    {QStringLiteral("The Layouts - Tempo Change"), {{70, 96}, {50, 72}, {60, 104}}},
    {QStringLiteral("Sample Artist - Cool Down"), {{80, 90}, {20, 0}, {60, 78}}}};

music::CadenceRide viewerRide() {
    music::CadenceRide ride;
    int start = 0;
    for (int index = 0; index < kViewerSongs.size(); ++index) {
        const auto& [title, blocks] = kViewerSongs[index];
        int duration = 0;
        for (const auto& [seconds, rpm] : blocks) {
            duration += seconds;
        }
        ride.tracks.append({QStringLiteral("viewer-song-%1.mp3").arg(index + 1), title, double(start), double(duration)});
        ride.songStarts.insert(static_cast<int>(ride.blocks.size()), index);
        int blockStart = start;
        for (const auto& [seconds, rpm] : blocks) {
            ride.blocks.append({seconds, rpm});
            for (int k = 0; rpm && k < seconds * rpm / 60; ++k) {
                ride.pulses.append(blockStart + k * 60.0 / rpm);
            }
            blockStart += seconds;
        }
        start += duration;
    }
    return ride;
}

// The song picker with LayoutViewer's made up songs (ViewerMusicPicker): three checked, one not, one unreadable, a
// playlist entry not found
QWidget* sceneMusicPicker() {
    writeSceneSettings();
    auto* picker = new MusicSelectDialog;
    picker->clearSongs();
    QList<std::pair<QString, std::optional<QString>>> songs;
    QSet<QString> checked;
    for (const auto& [title, blocks] : kViewerSongs) {
        songs.append({QStringLiteral("C:/Music/%1.mp3").arg(title), title});
        checked.insert(QStringLiteral("C:/Music/%1.mp3").arg(title));
    }
    songs.append({QStringLiteral("C:/Music/Unchecked Song.mp3"), QStringLiteral("Some Artist - Unchecked Song")});
    songs.append({QStringLiteral("C:/Music/Broken File.mp3"), QStringLiteral("Broken File")});
    // (analyzed as the viewer makes them up: the lengths in the order added)
    for (const auto& [path, title] : songs) {
        music::Analysis analysis;
        if (path.contains(QStringLiteral("Broken"))) {
            analysis.error = QStringLiteral("can't be read");
        } else {
            analysis.duration = 180.0 + 37 * (picker->analyses.size() % 4);
        }
        picker->analyses.insert(path, analysis);
    }
    picker->addToList(songs, checked);
    picker->missing = {QStringLiteral("C:/Music/Not There.mp3")};
    picker->updateSummary();
    return shownAsIs(picker);
}

// Musical Cadence Mode's workout screen 40% into LayoutViewer's made up songs, ridden by the simulated rider (seed 3)
// a second a tick (silent music), laid out at 1920x1116
QWidget* sceneCadenceScreen() {
    static awotest::FakeAnt fake;
    static StubBridge bridge(fake);
    writeSceneSettings();
    AppState& state = appState();
    state.secondsPassed = 0;
    state.workoutPowerRequestsSeconds.clear();
    state.workoutPowerRequestsWatts.clear();
    state.workoutPowerRequestsSysTime.clear();
    state.savedPowerVals = {};
    state.savedHRVals = {};
    state.savedCadenceVals = {};
    state.musicPlayed.clear();
    state.sessionRides.clear();
    state.rideStart.reset();
    state.rideRecorded = false;
    workoutData().clear();
    state.ftp = 250;
    state.maxHr = 185;
    state.wheelCircumference = 2.105;
    state.workoutMode = true;
    state.cadenceMode = true;
    state.ftpTestMode.clear();
    state.antHRM = true;
    state.antSpeedCadence = true;
    state.antCadence = false;
    state.antSpeed = false;
    music::applyRide(viewerRide());
    state.ftpRanges = ftpcolors::cadenceRanges(ftpcolors::kCadenceScaleBottomRpm);
    state.workoutFilename = state.workoutFilenameShort = QStringLiteral("music");
    state.startTimestamp = QDateTime::currentDateTime();
    auto* hud = new HudScreen(&bridge, nullptr, [] { return 1000.0; });
    SimulatedRider rider(3);
    hud->runSimulatedRide(state.workoutDurationSeconds * 0.4, rider);
    hud->stopTimers();
    for (QTimer* timer : hud->gradeOverlay()->findChildren<QTimer*>()) {
        timer->stop();
    }
    for (Gauge* gauge : {hud->cadenceGauge(), hud->heartRateGauge()}) {
        if (gauge && gauge->chainring()) {
            gauge->chainring()->spinTimer()->stop();
        }
        if (gauge && gauge->heart()) {
            gauge->heart()->beatTimer()->stop();
        }
    }
    auto* holder = new QWidget;
    holder->setObjectName(QStringLiteral("holder"));
    theme::setSheet(holder, QStringLiteral("#holder{background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 "
                                           "#28275e stop:1 #9fc5d1);}"));
    auto* layout = new QVBoxLayout(holder);
    layout->addWidget(hud);
    layout::applyLayout(holder, {{QStringLiteral("Workout"), hud}}, 1.0);
    shown(holder, 1920, 1116);
    restStars(hud->maxStars(), hud->maxBubbles());
    settle();
    return holder;
}

// the widget and everything in it in Qt's Fusion style, as ColorPicker.exe runs
void fusion(QWidget* widget) {
    static QStyle* style = QStyleFactory::create(QStringLiteral("Fusion"));
    widget->setStyle(style);
    for (QWidget* child : widget->findChildren<QWidget*>()) {
        child->setStyle(style);
    }
}

// ColorPicker.exe's window at QColor(200, 200, 200, 130)
QWidget* sceneColorPicker() {
    auto* picker = new colorpicker::ColorPicker(QColor(200, 200, 200, 130));
    fusion(picker);
    return shown(picker, 360, 460);
}

// LayoutViewer's FTP Colors tab at 700x460
QWidget* sceneFtpColorsTab() {
    appState().ftp = 250;
    return shown(new FtpColorsTab, 700, 460);
}

// The FTP Colors mockup window at FTP 250, 230 W, at 1000x900
QWidget* sceneFtpMockup() {
    appState().ftp = 250;
    QWidget* window = shown(new FtpMockupWindow, 1000, 900);
    // (the point at buttons each refresh replaced, gone)
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    settle();
    return window;
}

// copies a folder and everything in it
void copyTree(const QString& from, const QString& to) {
    QDir().mkpath(to);
    for (const QFileInfo& entry : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.isDir()) {
            copyTree(entry.absoluteFilePath(), QDir(to).filePath(entry.fileName()));
        } else {
            QFile::copy(entry.absoluteFilePath(), QDir(to).filePath(entry.fileName()));
        }
    }
}

// The theme picker over the shipped themes, the base theme picked, its workout screen preview drawn
QWidget* sceneThemePicker() {
    writeSceneSettings();
    copyTree(QStringLiteral(AWO_PYTHON_APP_DIR "/themes"), QStringLiteral("themes"));
    QDir().mkpath(QStringLiteral("awoWorkouts"));
    QFile::copy(QStringLiteral(AWO_PYTHON_APP_DIR "/") + kPreviewWorkout, kPreviewWorkout);
    appState() = AppState();
    appState().ftp = 250;
    appState().ftpRanges = zones::powerRanges();
    auto* dialog = new ThemeSelectDialog;
    theme::apply();
    dialog->setAttribute(Qt::WA_DontShowOnScreen);
    dialog->show();
    settle();
    dialog->buildPreview();
    settle();
    return dialog;
}

// LayoutViewer's display window in Workout Mode with the preview workout, 40% of the way through, laid out for
// 1920x1152 (the design size less the taskbar) and showing view; what moves by itself stopped where it is
QWidget* viewerScene(const QString& view) {
    // (in a folder of its own, as render_python.py builds each scene)
    auto* folder = new QTemporaryDir;
    const QString here = QDir::currentPath();
    QDir::setCurrent(folder->path());
    writeSceneSettings();
    appState() = AppState();
    auto* display = new viewer::DisplayWindow;
    display->setAttribute(Qt::WA_DontShowOnScreen);
    display->build(QStringLiteral("workout"), QStringLiteral(AWO_PYTHON_APP_DIR "/") + kPreviewWorkout, 40);
    const QSize available(1920, 1152);
    display->maximizedMode = false;
    display->resize(available);
    display->show();
    QMap<QString, QWidget*> screens;
    for (const auto& [name, screen] : display->views()) {
        screens.insert(name, screen);
    }
    layout::applyLayout(display, screens, layout::scaleFor(available), available);
    display->showView(view);
    settle();
    HudScreen* hud = display->workoutHud();
    hud->stopTimers();
    for (QTimer* timer : hud->gradeOverlay()->findChildren<QTimer*>()) {
        timer->stop();
    }
    hud->cadenceGauge()->chainring()->spinTimer()->stop();
    hud->heartRateGauge()->heart()->beatTimer()->stop();
    restStars(hud->maxStars(), hud->maxBubbles());
    auto* synopsis = qobject_cast<SynopsisScreen*>(display->view(QStringLiteral("Synopsis")));
    for (QTimer* timer : synopsis->findChildren<QTimer*>(Qt::FindDirectChildrenOnly)) {
        timer->stop();
    }
    if (view == u"Synopsis") {
        synopsis->layoutMaxLabels();
        for (int phase = 0; phase < synopsis->maxMarkers().size(); ++phase) {
            synopsis->maxMarkers()[phase].star->step(((3 + phase) * std::numbers::pi - phase * 2.1) / 3.6, phase);
        }
    }
    settle();
    QDir::setCurrent(here);
    return display;
}

const QList<std::pair<QString, std::function<QWidget*()>>> kScenes = {
    {QStringLiteral("hud_graph"), sceneHudGraph},
    {QStringLiteral("power_gauge"), scenePowerGauge},
    {QStringLiteral("cadence_gauge"), sceneCadenceGauge},
    {QStringLiteral("cadence_mode_gauge"), sceneCadenceModeGauge},
    {QStringLiteral("hr_gauge"), sceneHrGauge},
    {QStringLiteral("power_spiral"), scenePowerSpiral},
    {QStringLiteral("grade_overlay"), sceneGradeOverlay},
    {QStringLiteral("grade_list"), sceneGradeList},
    {QStringLiteral("grade_list_scrolled"), sceneGradeListScrolled},
    {QStringLiteral("smooth_bar"), sceneSmoothBar},
    {QStringLiteral("long_labels"), sceneLongLabels},
    {QStringLiteral("setup_screen"), sceneSetupScreen},
    {QStringLiteral("select_devices"), sceneSelectDevices},
    {QStringLiteral("connect_devices"), sceneConnectDevices},
    {QStringLiteral("calibrate_power"), sceneCalibratePower},
    {QStringLiteral("workout_picker"), sceneWorkoutPicker},
    {QStringLiteral("hud_screen"), sceneHudScreen},
    {QStringLiteral("synopsis_screen"), sceneSynopsisScreen},
    {QStringLiteral("music_picker"), sceneMusicPicker},
    {QStringLiteral("cadence_screen"), sceneCadenceScreen},
    {QStringLiteral("color_picker"), sceneColorPicker},
    {QStringLiteral("ftp_colors_tab"), sceneFtpColorsTab},
    {QStringLiteral("ftp_mockup"), sceneFtpMockup},
    {QStringLiteral("theme_picker"), sceneThemePicker},
    {QStringLiteral("viewer_init"), [] { return viewerScene(QStringLiteral("Init")); }},
    {QStringLiteral("viewer_workout"), [] { return viewerScene(QStringLiteral("Workout")); }},
    {QStringLiteral("viewer_synopsis"), [] { return viewerScene(QStringLiteral("Synopsis")); }},
    {QStringLiteral("viewer_music_picker"), [] { return viewerScene(QStringLiteral("Music Picker")); }},
    {QStringLiteral("viewer_workout_picker"), [] { return viewerScene(QStringLiteral("Workout Picker")); }},
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QString outDir = argc > 1 ? QDir::current().absoluteFilePath(QString::fromLocal8Bit(argv[1])) : QDir::currentPath();
    // in a folder of its own with the Python app's defaultTheme.json (render_python.py runs in the app's folder), so
    // the settings it changes are its own
    QTemporaryDir workDir;
    QDir::setCurrent(workDir.path());
    QFile::copy(QStringLiteral(AWO_PYTHON_APP_DIR "/defaultTheme.json"), QStringLiteral("defaultTheme.json"));
    // (no needle easing: drawn right at the reading)
    Performance::current().setGaugeFps(0);
    theme::setDefaultFont(QApplication::font());
    theme::setAppStyleSheet(style::kAppStyleSheet);
    theme::clear();
    AppState& state = appState();
    state.ftp = 250;
    state.ftpRanges = zones::powerRanges();

    QStringList names;
    for (int i = 2; i < argc; ++i) {
        names.append(QString::fromLocal8Bit(argv[i]));
    }
    QDir().mkpath(outDir);
    for (const auto& [name, make] : kScenes) {
        if (!names.isEmpty() && !names.contains(name)) {
            continue;
        }
        QWidget* widget = make();
        const QString path = QDir(outDir).filePath(name + QStringLiteral(".png"));
        widget->grab().save(path);
        std::printf("%s\n", qPrintable(QDir::toNativeSeparators(path)));
        delete widget;
    }
    return 0;
}

// Renders the C++ app's custom widgets to PNG files, set up as the screens set them up and fed the same made-up readings
// as render_python.py, so the two can be compared side by side (compare.py).
//
//     render_widgets.exe OUT_DIR [SCENE ...]
//
// Scenes are listed in kScenes below; all of them by default.

#include <QApplication>
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
#include "AppState.hpp"
#include "CalibratePowerWindow.hpp"
#include "DeviceDialogs.hpp"
#include "WorkoutSelectDialog.hpp"
#include "FakeAnt.hpp"
#include "SetupScreen.hpp"
#include "WorkoutHelpers.hpp"
#include "GradeWidgets.hpp"
#include "LongVLabels.hpp"
#include "PowerSpiral.hpp"
#include "SmoothProgressBar.hpp"
#include "AppStyle.hpp"
#include "MaxStar.hpp"
#include "Performance.hpp"
#include "PyCompat.hpp"
#include "PlotItems.hpp"
#include "SolidColoredColumns.hpp"
#include "Theme.hpp"
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

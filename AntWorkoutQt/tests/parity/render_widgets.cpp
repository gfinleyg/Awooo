// Renders the C++ app's custom widgets to PNG files, set up as the screens set them up and fed the same made-up readings
// as render_python.py, so the two can be compared side by side (compare.py).
//
//     render_widgets.exe OUT_DIR [SCENE ...]
//
// Scenes are listed in kScenes below; all of them by default.

#include <QApplication>
#include <QDir>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>

#include "AppState.hpp"
#include "AppStyle.hpp"
#include "MaxStar.hpp"
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

const QList<std::pair<QString, std::function<QWidget*()>>> kScenes = {
    {QStringLiteral("hud_graph"), sceneHudGraph},
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QString outDir = argc > 1 ? QDir::current().absoluteFilePath(QString::fromLocal8Bit(argv[1])) : QDir::currentPath();
    // (in the Python app's folder, as render_python.py runs: its defaultTheme.json)
    QDir::setCurrent(QStringLiteral(AWO_PYTHON_APP_DIR));
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

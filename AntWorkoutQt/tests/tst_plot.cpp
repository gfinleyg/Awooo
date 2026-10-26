// The plot library (src/plot) against pyqtgraph's own results (golden/plot.json): tick spacing, tick values and their
// numbers, the time axis' labels, view ranges with padding and limits; and the workout graph's views and items

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "AxisItem.hpp"
#include "MaxStar.hpp"
#include "PlotItems.hpp"
#include "SolidColoredColumns.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "ViewBox.hpp"
#include "WorkoutGraph.hpp"

using namespace awo;

namespace {

QList<double> numbers(const QJsonValue& value) {
    QList<double> list;
    for (const QJsonValue& item : value.toArray()) {
        list.append(item.toDouble());
    }
    return list;
}

QStringList strings(const QJsonValue& value) {
    QStringList list;
    for (const QJsonValue& item : value.toArray()) {
        list.append(item.toString());
    }
    return list;
}

bool sameNumbers(const QList<double>& a, const QList<double>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (!awotest::sameNumber(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

QString text(const QList<double>& values) {
    QStringList parts;
    for (const double value : values) {
        parts.append(QString::number(value, 'g', 17));
    }
    return parts.join(u", ");
}

}  // namespace

class TestPlot : public QObject {
    Q_OBJECT

private slots:
    void ticksMatchPyqtgraph() {
        plot::AxisItem axis(plot::AxisItem::Bottom);
        const QJsonArray cases = awotest::golden(QStringLiteral("plot.json"))[QStringLiteral("ticks")].toArray();
        QVERIFY(!cases.isEmpty());
        for (const QJsonValue& value : cases) {
            const QJsonObject c = value.toObject();
            const double mn = c[QStringLiteral("min")].toDouble();
            const double mx = c[QStringLiteral("max")].toDouble();
            const double size = c[QStringLiteral("size")].toDouble();
            const QString label = QStringLiteral("%1 - %2 over %3").arg(mn).arg(mx).arg(size);
            const QList<plot::AxisItem::TickLevel> spacing = axis.tickSpacing(mn, mx, size);
            const QJsonArray expectedSpacing = c[QStringLiteral("spacing")].toArray();
            QVERIFY2(spacing.size() == expectedSpacing.size(), qPrintable(label));
            for (qsizetype i = 0; i < spacing.size(); ++i) {
                QVERIFY2(awotest::sameNumber(expectedSpacing[i].toArray()[0], spacing[i].spacing), qPrintable(label));
            }
            const QList<plot::AxisItem::Ticks> ticks = axis.tickValues(mn, mx, size);
            const QJsonArray expectedTicks = c[QStringLiteral("values")].toArray();
            const QJsonArray expectedStrings = c[QStringLiteral("strings")].toArray();
            QVERIFY2(ticks.size() == expectedTicks.size(), qPrintable(label));
            for (qsizetype i = 0; i < ticks.size(); ++i) {
                const QList<double> expected = numbers(expectedTicks[i].toArray()[1]);
                QVERIFY2(sameNumbers(ticks[i].values, expected),
                         qPrintable(label + QStringLiteral(": ") + text(ticks[i].values) + QStringLiteral(" not ") +
                                    text(expected)));
                QCOMPARE(axis.tickStrings(ticks[i].values, 1.0, ticks[i].spacing), strings(expectedStrings[i]));
            }
        }
    }

    void timeAxisLabels() {
        TimeAxisItem axis(plot::AxisItem::Bottom);
        for (const QJsonValue& value : awotest::golden(QStringLiteral("plot.json"))[QStringLiteral("times")].toArray()) {
            const QJsonArray c = value.toArray();
            QCOMPARE(axis.tickStrings(numbers(c[1]), 1.0, c[0].toDouble()), strings(c[2]));
        }
    }

    void viewRangesMatchPyqtgraph() {
        for (const QJsonValue& value : awotest::golden(QStringLiteral("plot.json"))[QStringLiteral("views")].toArray()) {
            const QJsonObject c = value.toObject();
            const QJsonArray size = c[QStringLiteral("size")].toArray();
            plot::ViewBox view;
            view.resize(size[0].toDouble(), size[1].toDouble());
            const QJsonObject limits = c[QStringLiteral("limits")].toObject();
            if (!limits.isEmpty()) {
                plot::ViewBox::Limits set;
                set.xMin = awotest::optionalNumber(limits[QStringLiteral("xMin")]);
                set.xMax = awotest::optionalNumber(limits[QStringLiteral("xMax")]);
                set.minXRange = awotest::optionalNumber(limits[QStringLiteral("minXRange")]);
                view.setLimits(set);
            }
            const QJsonArray range = c[QStringLiteral("range")].toArray();
            const std::optional<double> padding = awotest::optionalNumber(c[QStringLiteral("padding")]);
            view.setXRange(range[0].toDouble(), range[1].toDouble(), padding);
            view.setYRange(range[0].toDouble(), range[1].toDouble(), padding);
            const QString label = QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact));
            const QJsonArray x = c[QStringLiteral("x")].toArray();
            const QJsonArray y = c[QStringLiteral("y")].toArray();
            QVERIFY2(awotest::sameNumber(x[0], view.xRange().min) && awotest::sameNumber(x[1], view.xRange().max),
                     qPrintable(label + QStringLiteral(" x: %1 %2").arg(view.xRange().min).arg(view.xRange().max)));
            QVERIFY2(awotest::sameNumber(y[0], view.yRange().min) && awotest::sameNumber(y[1], view.yRange().max),
                     qPrintable(label + QStringLiteral(" y: %1 %2").arg(view.yRange().min).arg(view.yRange().max)));
            const QJsonArray suggested = c[QStringLiteral("suggested")].toArray();
            QVERIFY(awotest::sameNumber(suggested[0], view.suggestPadding(plot::ViewBox::XAxis)));
            QVERIFY(awotest::sameNumber(suggested[1], view.suggestPadding(plot::ViewBox::YAxis)));
        }
    }

    void viewMapsDataUpAndAcross() {
        plot::ViewBox view;
        view.resize(200, 100);
        view.setXRange(0, 20, 0.0);
        view.setYRange(0, 10, 0.0);
        QCOMPARE(view.mapFromView({0, 0}), QPointF(0, 100));
        QCOMPARE(view.mapFromView({20, 10}), QPointF(200, 0));
        QCOMPARE(view.mapToView({100, 50}), QPointF(10, 5));
        QCOMPARE(view.viewPixelSize(), QPointF(0.1, 0.1));
    }

    void linkedViewsFollow() {
        WorkoutGraph graph;
        graph.resize(800, 300);
        graph.show();
        QVERIFY(QTest::qWaitForWindowExposed(&graph));
        graph.updateViews();
        graph.setXRange(100, 400, 0.0);
        // the extra views are half a pixel wider (pyqtgraph's ViewBox bounding rect), so their range reaches a little
        // further right, from the same start
        QCOMPARE(graph.powerVBox()->xRange().min, 100.0);
        QVERIFY(graph.powerVBox()->xRange().max >= 400.0 && graph.powerVBox()->xRange().max < 401.0);
        QCOMPARE(graph.hrCadenceVBox()->xRange().min, 100.0);
        // the axes follow their views
        QCOMPARE(graph.axis(plot::AxisItem::Left)->range(), graph.powerVBox()->yRange());
        QCOMPARE(graph.axis(plot::AxisItem::Bottom)->range(), graph.vb()->xRange());
    }

    // pyqtgraph's links go both ways: a linked view changed itself (dragged, zoomed) takes the view it's linked to
    // along, and that one the rest
    void linksFollowBack() {
        WorkoutGraph graph;
        graph.resize(800, 300);
        graph.show();
        QVERIFY(QTest::qWaitForWindowExposed(&graph));
        graph.updateViews();
        graph.setXRange(100, 400, 0.0);
        graph.hrCadenceVBox()->setXRange(200, 260, 0.0);
        QVERIFY(std::abs(graph.vb()->xRange().min - 200) < 1e-9);
        QVERIFY(graph.vb()->xRange().max > 259 && graph.vb()->xRange().max <= 260);
        QCOMPARE(graph.powerVBox()->xRange().min, graph.vb()->xRange().min);
        // (not back again into the one changed)
        QCOMPARE(graph.hrCadenceVBox()->xRange(), (plot::Range{200, 260}));
    }

    // a click is offered to the clickable items under it, through the views over them, topmost first
    void clicksPassDownThroughViews() {
        WorkoutGraph graph;
        graph.resize(800, 300);
        graph.show();
        QVERIFY(QTest::qWaitForWindowExposed(&graph));
        graph.updateViews();
        graph.setXRange(0, 100, 0.0);
        graph.powerVBox()->setYRange(0, 100, 0.0);
        auto* curve = new plot::CurveItem;
        curve->setData({0, 100}, {50, 50}, plot::mkPen(Qt::red), std::nullopt);
        curve->setClickable(true, 10);
        // (in the power view, under the heart rate/cadence view)
        graph.powerVBox()->addItem(curve);
        QSignalSpy curveClicks(curve, &plot::CurveItem::clicked);
        // (whichever view is on top under the mouse took the press)
        QList<bool> viewClicks;
        for (plot::ViewBox* view : {graph.vb(), graph.powerVBox(), graph.hrCadenceVBox()}) {
            QObject::connect(view, &plot::ViewBox::clicked, &graph,
                             [&viewClicks](QPointF, Qt::MouseButton, bool taken) { viewClicks.append(taken); });
        }
        const QPoint onLine = graph.mapFromScene(graph.powerVBox()->mapViewToScene(QPointF(50, 50)));
        QTest::mouseClick(graph.viewport(), Qt::LeftButton, {}, onLine);
        QCOMPARE(curveClicks.count(), 1);
        QCOMPARE(viewClicks, QList<bool>{true});
        // off it: the view's click, nothing took it
        const QPoint offLine = graph.mapFromScene(graph.powerVBox()->mapViewToScene(QPointF(50, 90)));
        QTest::mouseClick(graph.viewport(), Qt::LeftButton, {}, offLine);
        QCOMPARE(curveClicks.count(), 1);
        QCOMPARE(viewClicks, (QList<bool>{true, false}));
    }

    void mouseZoomAndDragHoldToLimits() {
        plot::ViewBox view;
        view.resize(1000, 100);
        view.setXRange(0, 100, 0.0);
        plot::ViewBox::Limits limits;
        limits.xMin = 0;
        limits.xMax = 100;
        limits.minXRange = 10;
        view.setLimits(limits);
        view.setMouseEnabled(true, false);
        QCOMPARE(view.xRange(), (plot::Range{0, 100}));
        // never under the shortest range, never past the limits
        view.setXRange(50, 51, 0.0);
        QCOMPARE(view.xRange().span(), 10.0);
        view.setXRange(95, 110, 0.0);
        QCOMPARE(view.xRange().max, 100.0);
    }

    void columnsColoredByRange() {
        plot::ViewBox view;
        view.resize(100, 100);
        auto* columns = new plot::SolidColoredColumns;
        view.addItem(columns);
        const QList<QColor> colors{Qt::red, Qt::green, Qt::blue};
        columns->setData({0, 10, 20, 30}, {50, 150, 250}, {100, 200}, colors);
        QCOMPARE(columns->boundingRect(), QRectF(0, 0, 30, 250));
        int calls = 0;
        columns->setData({0, 10, 20}, {100, 101}, {100}, [&calls](double value, const QList<double>& ranges) {
            ++calls;
            return value <= ranges[0] ? QColor(Qt::red) : QColor(Qt::blue);
        });
        QImage image(100, 100, QImage::Format_ARGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        columns->paint(&painter, nullptr, nullptr);
        painter.end();
        QCOMPARE(calls, 2);
    }

    void curveBoundsReachItsPens() {
        plot::ViewBox view;
        view.resize(100, 100);
        view.setXRange(0, 100, 0.0);
        view.setYRange(0, 100, 0.0);
        auto* curve = new plot::CurveItem;
        view.addItem(curve);
        curve->setData({10, 20, 30}, {5, 50, 25}, plot::mkPen(Qt::blue, 2), plot::mkPen(Qt::white, 4));
        const QRectF bounds = curve->boundingRect();
        // the data, and 3 px (half the halo, and 1) past it
        QCOMPARE(bounds, QRectF(QPointF(7, 2), QPointF(33, 53)));
    }

    void starsMakeRoomAtTheTop() {
        QCOMPARE(MaxStar::ease(0.0), 0.0);
        QCOMPARE(MaxStar::ease(0.5), 0.5);
        QCOMPARE(MaxStar::ease(1.0), 1.0);
        QCOMPARE(MaxStar::lift(0.2, 0.0), 0.2);
        QCOMPARE(MaxStar::lift(0.2, 1.0), 1.0);
        plot::ViewBox view;
        view.resize(100, 100);
        view.setYRange(0, 100, 0.0);
        // a max at the top needs the headroom over it: the range grows so it fits under the top
        QCOMPARE(MaxStar::topWithRoom(&view, 100, {{20, 100}}), 125.0);
        // one well under the top fits already
        QCOMPARE(MaxStar::topWithRoom(&view, 100, {{20, 50}}), 100.0);
    }

    void hourMinuteText() {
        QCOMPARE(hourMinuteFromSecond(0), QStringLiteral("0m"));
        QCOMPARE(hourMinuteFromSecond(3590), QStringLiteral("59m"));
        QCOMPARE(hourMinuteFromSecond(3600), QStringLiteral("1h 0m"));
        QCOMPARE(hourMinuteSecondFromSecond(65), QStringLiteral("1m 05s"));
        QCOMPARE(hourMinuteSecondFromSecond(3665), QStringLiteral("1h 1m 05s"));
    }
};

QTEST_MAIN(TestPlot)
#include "tst_plot.moc"

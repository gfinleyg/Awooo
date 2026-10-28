// The custom painted widgets (src/ui/widgets): the power spiral (tests/test_power_spiral.py: its timelines, where it's
// drawn, what's drawn), the gauge (readings, easing at the Performance setting's rate, the band's colors, center
// rings), the grade overlay and list, the block label column and the smooth progress bar

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QtTest>

#include <cmath>
#include <numbers>

#include "AnalogGauge.hpp"
#include "AppState.hpp"
#include "GradeWidgets.hpp"
#include "LongVLabels.hpp"
#include "Performance.hpp"
#include "PowerSpiral.hpp"
#include "PyCompat.hpp"
#include "SmoothProgressBar.hpp"
#include "TestSupport.hpp"
#include "Theme.hpp"
#include "ZoneColors.hpp"
#include "ZoneLegend.hpp"

using namespace awo;
using namespace awo::spiral;

namespace {

constexpr double kPi = std::numbers::pi;

// ---- The spiral's helpers (test_power_spiral.py's) ----

std::unique_ptr<PowerSpiral> spiralOf(QSize size, double block = 60, double workout = 600) {
    auto spiral = std::make_unique<PowerSpiral>();
    spiral->resize(size);
    spiral->clear(workout);
    spiral->setBlock(block);
    return spiral;
}

QColor at(const QImage& image, const PowerSpiral::Geometry& g, double theta, double r) {
    return image.pixelColor(static_cast<int>(py::round(g.center.x() + r * std::sin(theta))),
                            static_cast<int>(py::round(g.center.y() - r * std::cos(theta))));
}

// the middle of the bin at angle theta
double barMiddle(double theta) {
    const double step = 2 * kPi / kBins;
    return (std::floor(theta / step) + 0.5) * step;
}

// the image's color on the ring's middle (or offset from it), on the bin fraction of the way round
QColor onRing(const PowerSpiral& spiral, const QImage& image, double fraction, double offset = 0.0) {
    const PowerSpiral::Geometry g = spiral.geometryNow();
    return at(image, g, barMiddle(2 * kPi * fraction), g.ringR + offset);
}

// the image's color on the spiral's middle, on the bin fraction of the way along it
QColor onSpiral(const PowerSpiral& spiral, const QImage& image, double fraction) {
    const PowerSpiral::Geometry g = spiral.geometryNow();
    const double theta = barMiddle(2 * kPi * kBlockTurns * fraction);
    return at(image, g, theta, spiral.spiralRadius(g)(theta));
}

// (without the window's background: what isn't drawn stays see-through)
QImage drawn(PowerSpiral& spiral) {
    QImage image(spiral.size(), QImage::Format_ARGB32);
    image.fill(QColor(0, 0, 0, 0));
    spiral.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

// samples from start to seconds, into the workout and (block) the block at the same time
void ride(PowerSpiral& spiral, double seconds, double power, std::optional<double> cadence, double start = 0.0,
          double step = 0.1, bool block = true) {
    for (double t = start; t <= seconds; t += step) {
        spiral.addSample(t, block ? t : 0, power, cadence);
    }
}

bool close(const QColor& a, const QColor& b, int by = 3) {
    return std::abs(a.red() - b.red()) <= by && std::abs(a.green() - b.green()) <= by &&
           std::abs(a.blue() - b.blue()) <= by;
}

QString text(const QColor& color) {
    return color.name(QColor::HexArgb);
}

}  // namespace

class TestWidgets : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        theme::current().reset();
        theme::setUiScale(1.0);
        Performance::current().reset();
        appState() = AppState();
    }

    void cleanup() {
        theme::current().reset();
        dir_.reset();
    }

    // ---- Cadence to height ----

    void cadenceShares() {
        QCOMPARE(cadenceShare(0.0), 0.0);
        QCOMPARE(cadenceShare(39.0), 0.0);
        QCOMPARE(cadenceShare(kCadenceMinRpm), 0.0);
        QCOMPARE(cadenceShare((kCadenceMinRpm + kCadenceMaxRpm) / 2), 0.5);
        QCOMPARE(cadenceShare(kCadenceMaxRpm), 1.0);
        QCOMPARE(cadenceShare(140.0), 1.0);
        // no cadence to show: the lane's outer edge
        QCOMPARE(cadenceShare(std::nullopt), 1.0);
    }

    // ---- A timeline's bins ----

    void timelineBinsAverageAndCarryOn() {
        Timeline timeline(kBins);  // a second a bin
        timeline.add(0.2, 100, 80);
        timeline.add(0.7, 200, 90);
        timeline.add(5.5, 300, std::nullopt);
        const QList<BinValue>& values = timeline.values();
        QCOMPARE(values[0], (BinValue{150, 85}));
        // bins nothing landed in show the bin before
        QCOMPARE(values[1], (BinValue{150, 85}));
        QCOMPARE(values[4], (BinValue{150, 85}));
        // a sample without a cadence keeps the power
        QCOMPARE(values[5], (BinValue{300, std::nullopt}));
        QCOMPARE(values[kBins - 1], (BinValue{300, std::nullopt}));
    }

    void timelineStartsWithItsFirstSample() {
        Timeline timeline(60);
        QCOMPARE(timeline.values(), QList<BinValue>(kBins));
        // (the first sample comes a tick in: the bins before it show it, so the line starts at the start)
        timeline.add(1, 100, 80);
        timeline.add(30, 200, 90);
        const QList<BinValue>& values = timeline.values();
        QCOMPARE(values[0], (BinValue{100, 80}));
        QCOMPARE(values[2], (BinValue{100, 80}));
        QCOMPARE(values[kBins / 2], (BinValue{200, 90}));
    }

    void timelineFractionAndLengthChange() {
        Timeline timeline(100);
        timeline.seconds = 25;
        QCOMPARE(timeline.fraction(), 0.25);
        timeline.seconds = 250;
        QCOMPARE(timeline.fraction(), 1.0);
        // (a ramp test's workout grows): what was ridden moves to where it now falls
        timeline.add(50, 200, 90);
        QCOMPARE(*timeline.values()[kBins / 2].power, 200.0);
        timeline.add(90, 100, 90);
        timeline.setDuration(200);
        QCOMPARE(*timeline.values()[kBins / 4].power, 200.0);
        QCOMPARE(*timeline.values()[kBins * 90 / 200].power, 100.0);
    }

    void timelineSamplePastTheEndLandsInTheLastBin() {
        Timeline timeline(10, kBins * kBlockTurns);
        timeline.add(12, 150, 90);
        QCOMPARE(timeline.values().size(), kBins * kBlockTurns);
        QCOMPARE(timeline.values().back(), (BinValue{150, 90}));
    }

    // ---- Where it's drawn ----

    void ringOutsideSpiralInsideDownToTheHole() {
        auto spiral = spiralOf({300, 400});
        const PowerSpiral::Geometry g = spiral->geometryNow();
        // square, centered: the ring's outer edge touches the narrower side
        QCOMPARE(g.center, QPointF(150, 200));
        QVERIFY(qFuzzyCompare(g.ringR + g.lane / 2, 150.0));
        // every lane the same width, kSpacePx apart: the spiral starts a lane in from the ring
        QVERIFY(qFuzzyCompare(g.pitch - g.lane, kSpacePx));
        QVERIFY(qFuzzyCompare(g.ringR - g.spiralR, g.pitch));
        // its last turn's inner edge on the hole
        QVERIFY(qFuzzyCompare(spiral->spiralRadius(g)(2 * kPi * kBlockTurns) - g.lane / 2, g.rInner));
        const auto [center, radius] = spiral->innerCircle();
        QVERIFY(center == g.center && radius < g.rInner);
    }

    void gradeOverlayUsesTheHole() {
        auto spiral = spiralOf({300, 400});
        auto* overlay = new GradeOverlay(spiral.get());
        QCOMPARE(overlay->innerCircle(), spiral->innerCircle());
    }

    // ---- What's drawn ----

    void ringIsColoredByPowerAndAsThickAsTheCadence() {
        appState().ftp = 200;
        auto spiral = spiralOf({300, 300});
        ride(*spiral, 300, 250, 110);  // the first half spun at full width
        spiral->addSample(330, 0, 250, 20);  // then coasting: only the line, on the lane's inner edge
        spiral->setProgress(360, 600, 0, 60);
        const QImage image = drawn(*spiral);
        const QColor expected = zones::zoneColor(250, zones::powerRanges());
        QVERIFY2(close(onRing(*spiral, image, 0.25), expected), qPrintable(text(onRing(*spiral, image, 0.25))));
        QCOMPARE(onRing(*spiral, image, 0.25).alpha(), 255);
        // nothing but the faint track where the rider coasted, and past where the workout's got to
        QVERIFY(onRing(*spiral, image, 0.57).alpha() < 60);
        QVERIFY(onRing(*spiral, image, 0.8).alpha() < 60);
    }

    // halfway between kCadenceMinRpm and kCadenceMaxRpm: the line across the lane's middle, the power's color under
    // it, nothing but the faint track over it, and no gaps along it
    void filledUnderTheCadenceLineSeeThroughAbove() {
        appState().ftp = 200;
        auto spiral = spiralOf({600, 600});
        ride(*spiral, 300, 150, (kCadenceMinRpm + kCadenceMaxRpm) / 2, 0.0, 0.1, false);
        spiral->setProgress(300, 600, 0, 60);
        const QImage image = drawn(*spiral);
        const double lane = spiral->geometryNow().lane;
        const QColor fill = zones::zoneColor(150, zones::powerRanges());
        for (const double fraction : {0.1, 0.2, 0.2 + 0.5 / kBins, 0.3}) {
            const QColor under = onRing(*spiral, image, fraction, -lane / 4);
            QVERIFY2(under.alpha() == 255 && close(under, fill), qPrintable(QString::number(fraction)));
            QVERIFY2(onRing(*spiral, image, fraction, lane / 4).alpha() < 60, qPrintable(QString::number(fraction)));
        }
        // the line darker than what's under it
        QVERIFY(onRing(*spiral, image, 0.2).lightness() < fill.lightness() - 40);
    }

    void finishedRingMeetsItself() {
        auto spiral = spiralOf({300, 300});
        ride(*spiral, 600, 150, 110, 0.0, 0.1, false);
        spiral->setProgress(600, 600, 0, 60);
        const QImage image = drawn(*spiral);
        // all the way round, right up to North from either side
        for (const double fraction : {0.002, 0.5, 0.998}) {
            QVERIFY2(onRing(*spiral, image, fraction).alpha() == 255, qPrintable(QString::number(fraction)));
        }
    }

    // a 2 minute block and a 45 minute one, both half done, end in the same place
    void spiralIsTheSameLengthWhateverTheBlock() {
        for (const double seconds : {120.0, 2700.0}) {
            auto spiral = spiralOf({300, 300}, seconds, 3600);
            const double step = seconds / 1200;
            for (double t = 0.0; t <= seconds / 2; t += step) {
                spiral->addSample(t, t, 200, 100);
            }
            spiral->setProgress(seconds / 2, 3600, seconds / 2, seconds);
            const QImage image = drawn(*spiral);
            QVERIFY2(onSpiral(*spiral, image, 0.45).alpha() == 255, qPrintable(QString::number(seconds)));
            QVERIFY2(onSpiral(*spiral, image, 0.58).alpha() < 60, qPrintable(QString::number(seconds)));
        }
    }

    void spiralIsColoredByThePowerRiddenAndRunsThroughItsTurns() {
        appState().ftp = 200;
        // (big enough that a bar on the inner turns is a few pixels across)
        auto spiral = spiralOf({600, 600}, 90);
        ride(*spiral, 45, 120, 100);
        ride(*spiral, 90, 300, 100, 45.1);
        spiral->setProgress(90, 600, 90, 90);
        const QImage image = drawn(*spiral);
        const ftpcolors::ZoneRanges ranges = zones::powerRanges();
        QVERIFY(close(onSpiral(*spiral, image, 0.2), zones::zoneColor(120, ranges)));
        QVERIFY(close(onSpiral(*spiral, image, 0.8), zones::zoneColor(300, ranges)));
        // every turn drawn, up to and across where they meet at North
        QList<double> fractions{0.999};
        for (int k = 1; k < kBlockTurns; ++k) {
            fractions += {static_cast<double>(k) / kBlockTurns - 0.003, static_cast<double>(k) / kBlockTurns + 0.003};
        }
        for (const double fraction : fractions) {
            QVERIFY2(onSpiral(*spiral, image, fraction).alpha() == 255, qPrintable(QString::number(fraction)));
        }
    }

    void withoutCadenceTheLineIsFullWidth() {
        auto spiral = spiralOf({300, 300});
        ride(*spiral, 300, 150, std::nullopt);
        spiral->setProgress(300, 600, 0, 60);
        const PowerSpiral::Geometry g = spiral->geometryNow();
        // near the lane's edge too
        QVERIFY(onRing(*spiral, drawn(*spiral), 0.25, g.lane / 2 - 1.5).alpha() > 200);
    }

    void blinks() {
        auto spiral = spiralOf({300, 300}, 100);
        ride(*spiral, 50, 200, 100);
        spiral->setProgress(300, 600, 50, 100);
        spiral->setBlink(PowerSpiral::Blink::Block);
        QImage image = drawn(*spiral);
        QCOMPARE(onRing(*spiral, image, 0.05).alpha(), 255);
        QVERIFY(onSpiral(*spiral, image, 0.25).alpha() < 60);
        spiral->setBlink(PowerSpiral::Blink::Workout);
        image = drawn(*spiral);
        QVERIFY(onRing(*spiral, image, 0.05).alpha() < 60);
        const int alpha = onSpiral(*spiral, image, 0.25).alpha();
        QVERIFY(100 < alpha && alpha < 200);
        spiral->setBlink(PowerSpiral::Blink::Default);
        QCOMPARE(onSpiral(*spiral, drawn(*spiral), 0.25).alpha(), 255);
    }

    // two blocks the same length, the second ridden as far as the first was: it's drawn with its own cadence
    void aNewBlockIsDrawnAsItsOwn() {
        auto spiral = spiralOf({300, 300}, 100);
        ride(*spiral, 50, 200, 110);
        spiral->setProgress(0, 600, 50, 100);
        QCOMPARE(onSpiral(*spiral, drawn(*spiral), 0.25).alpha(), 255);
        spiral->setBlock(100);
        ride(*spiral, 50, 200, 20);  // (coasting: only the line, on the lane's inner edge)
        spiral->setProgress(0, 600, 50, 100);
        QVERIFY(onSpiral(*spiral, drawn(*spiral), 0.25).alpha() < 60);
    }

    void redrawnOnlyWhenAnEndMovesEnoughToSee() {
        auto spiral = spiralOf({300, 300});
        spiral->setProgress(0.0, 7200, 0.0, 60);
        drawn(*spiral);
        const int before = spiral->updatesRequested();
        // a 2 hour workout's end barely moves in a frame
        spiral->setProgress(0.016, 7200, 0.0, 60);
        QCOMPARE(spiral->updatesRequested(), before);
        spiral->setProgress(0.016, 7200, 1.0, 60);
        QVERIFY(spiral->updatesRequested() > before);
    }

    // ---- The session's earlier rides ----

    // the ring starts with the rides before this workout, each in the zones it was ridden in, a tick where each ends;
    // this workout goes on from there
    void ringCarriesOnFromTheSessionsRides() {
        appState().ftp = 200;
        const ftpcolors::ZoneRanges now = zones::powerRanges();
        // ridden when the FTP was 400: 250 W was a low zone then
        ftpcolors::ZoneRanges then;
        for (const double limit : now.limits) {
            then.limits.append(limit * 2);
        }
        session::RingHistory earlier{300, {}, then};
        for (int t = 0; t < 300; ++t) {
            earlier.samples.append({t + 0.5, 250, 110});
        }
        PowerSpiral spiral;
        spiral.resize(300, 300);
        spiral.clear(300, {earlier});
        spiral.setBlock(60);
        // half the ring is the earlier ride, this workout's yet to start
        QCOMPARE(spiral.workout().duration(), 600.0);
        QVERIFY(qFuzzyCompare(spiral.workout().fraction(), 0.5));
        ride(spiral, 290, 250, 110, 0.0, 0.1, false);
        spiral.setProgress(290, 300, 0, 60);
        QVERIFY(qFuzzyCompare(spiral.workout().seconds, 590.0));
        const QImage image = drawn(spiral);
        QVERIFY(close(onRing(spiral, image, 0.25), zones::zoneColor(250, then)));
        QVERIFY(close(onRing(spiral, image, 0.75), zones::zoneColor(250, now)));
        QVERIFY(zones::zoneColor(250, then) != zones::zoneColor(250, now));
        // the tick across the ring where the earlier ride ends, in the line's color
        const PowerSpiral::Geometry g = spiral.geometryNow();
        const QColor tick = at(image, g, kPi, g.ringR);
        QVERIFY(close(tick, QColor(theme::current().mapHex(kLineColor)), 40));

        // what this workout added, for the session's next ride: its own seconds, one sample a second
        const Ring ridden = spiral.ridden();
        QVERIFY(qFuzzyCompare(ridden.seconds, 290.0));
        // (the ride's 0.1s steps add up to just under 290)
        QVERIFY(ridden.samples.size() == 290 || ridden.samples.size() == 291);
        QCOMPARE(ridden.samples[0].seconds, 0.5);
        QVERIFY(qFuzzyCompare(ridden.samples[0].power, 250.0));
        QVERIFY(qFuzzyCompare(*ridden.samples[0].cadence, 110.0));
        for (qsizetype i = 1; i < ridden.samples.size(); ++i) {
            QCOMPARE(ridden.samples[i].seconds - ridden.samples[i - 1].seconds, 1.0);
        }
    }

    void ringWithoutRidesBeforeIsTheWorkout() {
        auto spiral = spiralOf({300, 300}, 60, 600);
        QCOMPARE(spiral->workout().duration(), 600.0);
        QCOMPARE(spiral->historySeconds(), 0.0);
        spiral->setProgress(60, 600, 0, 60);
        QVERIFY(qFuzzyCompare(spiral->workout().fraction(), 0.1));
        QVERIFY(qFuzzyCompare(spiral->ridden().seconds, 60.0));
    }

    // ---- The gauge ----

    void gaugeHoldsTheNeedleWithinTheScale() {
        Performance::current().setGaugeFps(0);
        AnalogGauge gauge;
        gauge.setMinValue(40);
        gauge.setMaxValue(200);
        gauge.setValue(250);
        QCOMPARE(gauge.value(), 200.0);
        QCOMPARE(gauge.reading(), 250.0);
        // a dropout's 0: the needle at the start, the reading 0
        gauge.setValue(0);
        QCOMPARE(gauge.value(), 40.0);
        QCOMPARE(gauge.reading(), 0.0);
        // (easing off: drawn right at the reading)
        QCOMPARE(gauge.displayValue(), 40.0);
    }

    void gaugeNeedleEasesAtTheSettingsRate() {
        Performance::current().setGaugeFps(60);
        AnalogGauge gauge;
        gauge.resize(300, 300);
        gauge.show();
        QVERIFY(QTest::qWaitForWindowExposed(&gauge));
        int frames = 0;
        QTimer* needle = nullptr;
        for (QTimer* timer : gauge.findChildren<QTimer*>()) {
            if (timer->interval() == static_cast<int>(py::round(1000.0 / 60))) {
                needle = timer;
            }
        }
        QVERIFY(needle != nullptr);
        connect(needle, &QTimer::timeout, &gauge, [&frames] { ++frames; });
        QElapsedTimer clock;
        clock.start();
        gauge.setValue(800);
        QVERIFY(gauge.displayValue() < 1.0);
        // most of the way there in a quarter second, at about 60 frames a second, then it stops by itself
        QTRY_VERIFY_WITH_TIMEOUT(gauge.displayValue() == 800.0, 3000);
        const double seconds = clock.nsecsElapsed() / 1e9;
        QVERIFY2(frames >= seconds * 60 * 0.5, qPrintable(QStringLiteral("%1 frames in %2 s").arg(frames).arg(seconds)));
        QVERIFY(!needle->isActive());
        // turned off part way: straight to the reading
        gauge.setValue(100);
        Performance::current().setGaugeFps(0);
        QCOMPARE(gauge.displayValue(), 100.0);
    }

    void gaugeBandColorsSitAtTheirValues() {
        AnalogGauge gauge;
        gauge.setMaxValue(1000);
        // the start of the scale is the gradient's far end, the end of the scale just past its start
        QVERIFY(qFuzzyCompare(gauge.valueGradientPosition(0), 271.0 / 360));
        QVERIFY(qFuzzyCompare(gauge.valueGradientPosition(1000), 1.0 / 360));
        QVERIFY(qFuzzyCompare(gauge.valueGradientPosition(500), 136.0 / 360));
    }

    void gaugeRingsFitItsSize() {
        AnalogGauge gauge;
        gauge.bigNeedleCenterSize = 5;
        gauge.centerRingCount = 3;
        gauge.resize(200, 200);
        gauge.show();
        QVERIFY(QTest::qWaitForWindowExposed(&gauge));
        QCOMPARE(gauge.ringCount(), 3);
        gauge.centerRingMinPx = 6;
        // (a 40 px center, 60% of it ringed at 6 px a ring: 1 + 4)
        QCOMPARE(gauge.ringCount(), 5);
        QSignalSpy changed(&gauge, &AnalogGauge::ringCountChanged);
        gauge.resize(400, 400);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(gauge.ringCount(), 9);
    }

    // (test_power_spiral.py test_cadence_rings_gear_turns_and_beats_with_the_chainring, the gear's half; the beat's
    // is tst_hud's cadenceRingsBeatWithTheChainring) the outermost ring a gear turned with the chainring behind it
    void cadenceRingsGearTurnsWithTheChainring() {
        Gauge gauge(QStringLiteral("Chainring.svg"), std::nullopt, true);
        AnalogGauge* w = gauge.gauge();
        RotatingSvgWidget* chainring = gauge.chainring();
        QCOMPARE(w->centerGearAngle(), std::optional<double>(0.0));
        w->setMaxValue(200);
        w->setValue(60);
        QTRY_VERIFY(chainring->angle() > 0);
        QCOMPARE(w->centerGearAngle(), std::optional<double>(chainring->angle()));
        // stopped: it holds where it got to
        w->setValue(0);
        QTRY_COMPARE(chainring->spinTimer()->interval(), RotatingSvgWidget::kIdleMs);
        QCOMPARE(w->centerGearAngle(), std::optional<double>(chainring->angle()));
    }

    // ---- The grade overlay and list ----

    void gradeOverlayFadesOut() {
        PowerSpiral spiral;
        spiral.resize(300, 300);
        GradeOverlay overlay(&spiral);
        overlay.showGrade(QStringLiteral("A"), 3.0);
        QVERIFY(overlay.isVisibleTo(&spiral));
        overlay.stepAt(1.0);
        QCOMPARE(overlay.opacity(), 1.0);
        // the last second (no more than half of it) fades
        overlay.stepAt(2.5);
        QVERIFY(qFuzzyCompare(overlay.opacity(), 0.5));
        overlay.stepAt(3.0);
        QVERIFY(!overlay.isVisibleTo(&spiral) && overlay.grade().isEmpty());
        // no time to show it before the next block: not shown
        overlay.showGrade(QStringLiteral("A"), 0.0);
        QVERIFY(!overlay.isVisibleTo(&spiral));
        // a value stays until cleared
        overlay.showValue(QStringLiteral("Avg"), QStringLiteral("250 W"));
        QVERIFY(overlay.isVisibleTo(&spiral));
        QCOMPARE(overlay.title(), QStringLiteral("Avg"));
    }

    void gradeListScrollsWithMoreRows() {
        GradeList list;
        list.resize(600, 400);
        QList<GradeList::Row> rows;
        for (int i = 0; i < 20; ++i) {
            GradeList::Row row;
            row.label = QStringLiteral("%1:00 @ 150W").arg(i);
            row.grade = QStringLiteral("A");
            rows.append(row);
        }
        list.setRows(rows, 90);
        // (400 px less the average and its gaps, 46 px a row: 7 rows)
        QList<GradeList::Row> shown = list.shownRows();
        QCOMPARE(shown.size(), 7);
        QCOMPARE(shown.back().kind, GradeList::Row::Kind::More);
        list.scrollTo(3);
        shown = list.shownRows();
        QCOMPARE(shown.front().kind, GradeList::Row::Kind::More);
        QCOMPARE(shown[1].label, rows[3].label);
        // no further than the last row in the bottom slot
        list.scrollTo(100);
        shown = list.shownRows();
        QCOMPARE(list.scrollOffset(), 20 - 7 + 1);
        QCOMPARE(shown.back().label, rows.back().label);
    }

    void gradeListOfTheSessionsRides() {
        Ride graded;
        graded.kind = RideKind::Workout;
        graded.name = QStringLiteral("Test");
        graded.graded = true;
        Ride free;
        free.kind = RideKind::Free;
        GradeList list;
        list.refresh(QList<const Ride*>{&graded, &free});
        QCOMPARE(list.rows().size(), 2);
        QCOMPARE(list.rows()[0].kind, GradeList::Row::Kind::Ride);
        QVERIFY(list.rows()[0].graded && !list.rows()[1].graded);
        QCOMPARE(list.averageText(), QStringLiteral("Workout Average"));
    }

    // ---- The block labels and the progress bar ----

    void longLabelsShowWhatFitsWithMore() {
        LongVLabels labels;
        labels.resize(500, 300);
        labels.show();
        QVERIFY(QTest::qWaitForWindowExposed(&labels));
        QStringList lines;
        for (int i = 0; i < 10; ++i) {
            lines.append(QStringLiteral("%1:00 @ 150W").arg(i + 1));
        }
        labels.setFullText(lines.join(u'\n'));
        labels.renderLines();
        int visible = 0;
        for (QLabel* label : labels.labels()) {
            visible += label->isVisibleTo(&labels);
        }
        QVERIFY(visible >= 3 && visible < 10);
        QCOMPARE(labels.labels()[visible - 1]->text(), QStringLiteral("more..."));
        // an empty list shows nothing
        labels.setFullText({});
        labels.renderLines();
        QVERIFY(!labels.labels()[0]->isVisibleTo(&labels));
    }

    // ---- The zone legend ----

    void zoneLegendNamesWhereEachZoneEnds() {
        appState().ftp = 200;
        appState().ftpRanges = ftpcolors::powerRanges(200);
        ZoneLegend legend;
        QCOMPARE(legend.size(), QSize(LongVLabels::kLabelWidth, ZoneLegend::kHeight));
        QCOMPARE(legend.labelTexts(), QStringList({"72W", "174W", "238W", "374W"}));
        // Musical Cadence Mode: its cadence zones, in rpm
        appState().workoutMode = appState().cadenceMode = true;
        appState().ftpRanges = ftpcolors::cadenceRanges(40);
        QCOMPARE(legend.labelTexts(), QStringList({"40rpm", "65rpm", "90rpm", "115rpm"}));
    }

    void zoneLegendHalosTextThatDoesntStandOut() {
        ZoneLegend legend;
        legend.resize(legend.size());
        // the default theme: black text on the gradient's mid-tones (on its own, it's its window: the middle)
        QVERIFY(legend.haloed());
        // light text on a black window stands out
        theme::current().colors = {{QStringLiteral("#28275e"), QStringLiteral("#000000")},
                                   {QStringLiteral("#9fc5d1"), QStringLiteral("#1a1a1a")},
                                   {QStringLiteral("#000000"), QStringLiteral("#ffffff")}};
        QVERIFY(!legend.haloed());
        // drawn with its halo: light around the dark text, past the text's own outline
        theme::current().reset();
        QImage image(legend.size(), QImage::Format_ARGB32);
        image.fill(QColor(QStringLiteral("#5f6e98")));
        legend.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        int light = 0;
        for (int y = static_cast<int>(ZoneLegend::kBarHeight + ZoneLegend::kTextGap); y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                light += image.pixelColor(x, y).lightness() > 200;
            }
        }
        QVERIFY(light > 50);
    }

    void smoothBarGlidesBetweenPixels() {
        SmoothProgressBar bar;
        bar.setRange(0, 100);
        bar.setFraction(1.5);
        QCOMPARE(*bar.fraction(), 1.0);
        // a hard set overrides the smooth position
        bar.setValue(40);
        QVERIFY(!bar.fraction());
        bar.resize(101, 10);
        bar.setBarColors(QColor(Qt::blue), QColor(Qt::white));
        bar.setFraction(0.5);
        QImage image(bar.size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        // (without the window background a top level widget gets)
        bar.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        // rounded: what's behind shows at the corners
        QCOMPARE(bar.cornerRadius(), 3.0);
        QVERIFY(image.pixelColor(0, 0).alpha() < 128);
        QVERIFY(image.pixelColor(100, 9).alpha() < 128);
        QCOMPARE(image.pixelColor(10, 5), QColor(Qt::blue));
        QCOMPARE(image.pixelColor(90, 5), QColor(Qt::white));
        // the fill is framed in the bar color (2px at this height), never reaching the bar's edges
        QCOMPARE(bar.frameWidth(), 2.0);
        QCOMPARE(image.pixelColor(1, 5), QColor(Qt::white));
        QCOMPARE(image.pixelColor(10, 1), QColor(Qt::white));
        QCOMPARE(image.pixelColor(10, 8), QColor(Qt::white));
        // the leading edge's pixel blended
        const QColor edge = image.pixelColor(50, 5);
        QVERIFY(edge != QColor(Qt::blue) && edge != QColor(Qt::white));
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_MAIN(TestWidgets)
#include "tst_widgets.moc"

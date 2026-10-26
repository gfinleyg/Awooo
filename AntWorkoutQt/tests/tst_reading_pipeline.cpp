// ANT readings into what the workout screen shows (src/ant/ReadingPipeline), fed by a real bridge on a fake stick:
// the hold-last-value smoothing, the cadence/speed fallbacks and the sensor history (tests/test_poller.py)

#include "AppState.hpp"
#include "FakeAnt.hpp"
#include "PowerCalibration.hpp"
#include "ReadingPipeline.hpp"
#include "TestSupport.hpp"
#include "WorkoutData.hpp"
#include "WorkoutHelpers.hpp"

using namespace awo;
using namespace awo::ant;
using awotest::FakeAnt;

namespace {

// What the workout screen was given
struct Shown {
    QList<int> power;
    QList<int> heartRate;
    QList<double> cadence;
    QList<int> cadenceDecimals;
    QList<double> speed;
    QList<double> distance;
};

antnative::RawMessage meter(int watts, int cadence = 90) {
    return awotest::powerPage(100, watts, cadence, awotest::uniqueByte(), 0, 5);
}

// the i'th combined speed/cadence page, a second (stepTicks) apart
antnative::RawMessage combined(int i, double cadenceRpm = 90.0, double wheelMps = 8.0, int stepTicks = 1024) {
    const int cadenceRevolutions = static_cast<int>(std::nearbyint(i * cadenceRpm / 60));
    const int speedRevolutions = static_cast<int>(std::nearbyint(i * wheelMps / 2.0));
    return awotest::combinedPage(300, (i * stepTicks) & 0xFFFF, cadenceRevolutions & 0xFFFF, (i * stepTicks) & 0xFFFF,
                                 speedRevolutions & 0xFFFF, 0, 1);
}

antnative::RawMessage speedPage(int i, int revolutionsPerSecond = 4) {
    return awotest::bscPage(123, 500, (i * 1024) & 0xFFFF, (i * revolutionsPerSecond) & 0xFFFF, 1, 1);
}

bool near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

QList<int> column(const SensorHistory& history) {
    return history.column(0);
}

}  // namespace

class TestReadingPipeline : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::make_unique<awotest::TempWorkingDir>();
        appState() = AppState();
        appState().wheelCircumference = 2.0;
        workoutData().clear();
        ant_ = std::make_unique<FakeAnt>();
        bridge_ = ant_->makeBridge();
        bridge_->setRunning(true);
        bridge_->startupCleanup();
        QVERIFY(!bridge_->initializeWithRetries());
        bridge_->setInitialized(true);
        bridge_->markScanStarted();
        shown_ = Shown();
    }

    void cleanup() {
        pipeline_.reset();
        bridge_.reset();
        ant_.reset();
        dir_.reset();
    }

    // ---- Power ----

    void powerReadingsAndHold() {
        ride({{Role::Power, {11, 100}}});
        for (int watts : {200, 210, 0, 0, 0, 0, 220}) {
            hear(meter(watts));
        }
        // three zeros could be misreads: the last good value shows instead
        QCOMPARE(shown_.power, (QList<int>{200, 210, 210, 210, 210, 0, 220}));
        QCOMPARE(column(workoutData().power), (QList<int>{200, 210, 0, 0, 0, 0, 220}));
        QCOMPARE(appState().savedPowerVals.values, (QList<double>{200, 210, 210, 210, 210, 0, 220}));
        QCOMPARE(kHoldPowerReadings, 3);
    }

    void powerCalibrationIsApplied() {
        calibration::save(11, 100, 10, 1.0);
        ride({{Role::Power, {11, 100}}});
        hear(meter(210));
        QCOMPARE(shown_.power, QList<int>{200});
    }

    void powerGraphKeepsItsWindow() {
        ride({{Role::Power, {11, 100}}});
        const int window = appState().graphSecondsShown - appState().graphBreakpoint;
        for (int second = 0; second < window + 50; ++second) {
            appState().secondsPassed = second;
            hear(meter(200 + second % 7));
        }
        const QList<double>& times = appState().savedPowerVals.times;
        QVERIFY(times.last() - times.first() <= window);
        QCOMPARE(times.size(), window + 1);
    }

    void inactivePipelineIgnoresReadings() {
        ride({{Role::Power, {11, 100}}});
        pipeline_->setActive(false);
        hear(meter(200));
        QVERIFY(shown_.power.isEmpty());
    }

    // ---- Heart rate ----

    void heartRateAndHold() {
        ride({{Role::Power, {11, 100}}, {Role::Hrm, {120, 200}}}, true);
        hear(meter(100));
        QList<int> bpms{150};
        for (int i = 0; i <= kHoldHrReadings; ++i) {
            bpms << 0;
        }
        for (int i = 0; i < bpms.size(); ++i) {
            hear(awotest::hrPage(200, bpms[i], i, 1000 + i, 0, 1));
        }
        QList<int> expected(kHoldHrReadings + 1, 150);
        expected << 0;
        QCOMPARE(shown_.heartRate, expected);
        QCOMPARE(workoutData().heartRate.size(), bpms.size());
    }

    // ---- Cadence ----

    void combinedSensorCadenceSpeedAndDistance() {
        ride({{Role::Power, {11, 100}}, {Role::SpeedCadence, {121, 300}}}, false, true);
        for (int i = 0; i < 4; ++i) {
            hear(combined(i, 60));
        }
        // the first reading is only the baseline
        QCOMPARE(shown_.cadence.size(), 3);
        for (double rpm : shown_.cadence) {
            QVERIFY(near(rpm, 60));
        }
        // 4 revolutions of a 2m wheel a second: 8 m/s
        QVERIFY(near(shown_.speed.last(), 8 * 2.23694, 0.1));
        QCOMPARE(appState().totalRevolutions, 12);
        QVERIFY(near(shown_.distance.last(), 24 * 0.000621371, 0.01));
        QCOMPARE(workoutData().speedCadence.size(), 4);
    }

    void cadenceRollsOver() {
        ride({{Role::SpeedCadence, {121, 300}}}, false, true);
        // the event time and revolution counters wrap at 16 bits
        hear(awotest::combinedPage(300, 65000, 65535, 65000, 65534, 0, 1));
        hear(awotest::combinedPage(300, (65000 + 1024) & 0xFFFF, 1, (65000 + 1024) & 0xFFFF, 2, 0, 1));
        QCOMPARE(shown_.cadence.size(), 1);
        QVERIFY(near(shown_.cadence[0], 120));
    }

    void cadenceHoldAndKnownGarbage() {
        ride({{Role::SpeedCadence, {121, 300}}}, false, true);
        hear(awotest::combinedPage(300, 1000, 10, 1000, 10, 0, 1));
        hear(awotest::combinedPage(300, 2024, 11, 2024, 14, 0, 1));
        // a cadence event time the sensor is known to glitch to is skipped
        hear(awotest::combinedPage(300, 513, 12, 3048, 18, 0, 1));
        QCOMPARE(shown_.cadence.size(), 1);
        QVERIFY(near(shown_.cadence[0], 60));
        // (a stop: the same counters, a later speed event)
        hear(awotest::combinedPage(300, 3048, 12, 4072, 18, 0, 1));
        QVERIFY(near(shown_.cadence.last(), 60));
    }

    void cadenceFallsBackToThePowerMeter() {
        ride({{Role::Power, {11, 100}}, {Role::Cadence, {122, 400}}}, false, false, true);
        hear(awotest::bscPage(122, 400, 1024, 1, 1, 1));
        hear(awotest::bscPage(122, 400, 2048, 2, 1, 1));
        QCOMPARE(shown_.cadence.size(), 1);
        QVERIFY(near(shown_.cadence[0], 60));
        // the cadence sensor drops out
        bridge_->selectedSensor(Role::Cadence)->mark_state("LOST");
        hear(meter(200, 85));
        QCOMPARE(shown_.cadence.last(), 85.0);
        QCOMPARE(shown_.cadenceDecimals.last(), 0);
        QCOMPARE(column(workoutData().fallbackCadence), QList<int>{85});
        // and the fallback's 0 isn't replaced with the sensor's old reading
        hear(meter(0, 0));
        QCOMPARE(shown_.cadence.last(), 85.0);
        // back again: the baseline starts over
        bridge_->selectedSensor(Role::Cadence)->mark_state("ACTIVE");
        hear(awotest::bscPage(122, 400, 3072, 3, 1, 1));
        QCOMPARE(shown_.cadence.last(), 85.0);
        hear(awotest::bscPage(122, 400, 4096, 4, 1, 1));
        QVERIFY(near(shown_.cadence.last(), 60));
        QCOMPARE(shown_.cadenceDecimals.last(), 1);
    }

    void cadenceHoldsThroughShortZeros() {
        // Musical Cadence Mode: cadence from the power meter alone
        ride({{Role::Power, {11, 100}}}, false, false, false, false, true);
        hear(meter(200, 80));
        for (int i = 0; i <= kHoldCadenceReadings; ++i) {
            hear(meter(0, 0));
        }
        QList<double> expected(kHoldCadenceReadings + 1, 80);
        expected << 0;
        QCOMPARE(shown_.cadence, expected);
    }

    // ---- Speed ----

    void speedSensorAndHold() {
        ride({{Role::Power, {11, 100}}, {Role::Speed, {123, 500}}}, false, false, false, true);
        for (int i = 0; i < 3; ++i) {
            hear(speedPage(i));
        }
        // stopped
        for (int i = 3; i < 3 + kHoldSpeedReadings + 1; ++i) {
            hear(awotest::bscPage(123, 500, (i * 1024) & 0xFFFF, 8, 1, 1));
        }
        const double mph = 8 * 2.23694;
        QCOMPARE(shown_.speed.size(), 2 + kHoldSpeedReadings + 1);
        for (int i = 0; i < 2 + kHoldSpeedReadings; ++i) {
            QVERIFY(near(shown_.speed[i], mph, 0.05));
        }
        QCOMPARE(shown_.speed.last(), 0.0);
        QCOMPARE(workoutData().speed.size(), 3 + kHoldSpeedReadings + 1);
    }

    void speedEstimateWhileTheSensorIsOut() {
        ride({{Role::Power, {11, 100}}, {Role::Speed, {123, 500}}}, false, false, false, true);
        // learn the power -> speed curve from real readings
        for (int i = 0; i <= ReadingPipeline::kSpeedCalibrationMinSamples; ++i) {
            hear(meter(100 + 20 * i));
            hear(speedPage(i, 2 + i));
        }
        QCOMPARE(pipeline_->speedCalibration().size(), ReadingPipeline::kSpeedCalibrationMinSamples);
        const double distance = shown_.distance.last();
        bridge_->selectedSensor(Role::Speed)->mark_state("LOST");
        hear(meter(150));
        // an estimate is shown, but adds no distance
        QVERIFY(shown_.speed.last() > 0 && shown_.speed.last() < 60);
        QCOMPARE(shown_.distance.last(), distance);
    }

    void noSpeedEstimateFromATrainer() {
        hear(awotest::trainerPage(300, 200, 85, 0, 0, 5));
        ride({{Role::Power, {17, 300}}, {Role::Speed, {123, 500}}}, false, false, false, true);
        for (int watts = 100; watts < 400; watts += 50) {
            pipeline_->speedCalibration().append({0, static_cast<double>(watts), watts / 10.0});
        }
        hear(speedPage(0));
        bridge_->selectedSensor(Role::Speed)->mark_state("LOST");
        hear(awotest::trainerPage(300, 250, 85, 0, 0, 5));
        QVERIFY(shown_.speed.isEmpty());
    }

    void speedEstimateCurve_data() {
        QTest::addColumn<double>("watts");
        QTest::addColumn<double>("mph");
        const QList<std::pair<double, double>> cases = {{0, 0.0},     {-5, 0.0},    {50, 5.0},    {100, 10.0}, {150, 12.5},
                                                        {200, 15.0}, {250, 17.5}, {300, 20.0}, {500, 20.0}};
        for (const auto& [watts, mph] : cases) {
            QTest::newRow(qPrintable(QString::number(watts))) << watts << mph;
        }
    }

    void speedEstimateCurve() {
        QFETCH(double, watts);
        QFETCH(double, mph);
        ride({{Role::Power, {11, 100}}});
        pipeline_->speedCalibration() = {{0, 100, 10.0}, {0, 200, 15.0}, {0, 200, 15.0}, {0, 300, 20.0}, {0, 300, 20.0}};
        QVERIFY(near(*pipeline_->estimateSpeedFromPower(watts), mph));
    }

    void speedEstimateNeedsEnoughSamples() {
        ride({{Role::Power, {11, 100}}});
        pipeline_->speedCalibration() = {{0, 100, 10.0}};
        QVERIFY(!pipeline_->estimateSpeedFromPower(100));
    }

    void speedCalibrationAgesOut() {
        double now = 1000;
        ReadingPipeline pipeline(bridge_.get(), [&now] { return now; });
        pipeline.recordSpeedCalibration(100, 10);
        now += ReadingPipeline::kSpeedCalibrationMaxAgeSeconds + 1;
        pipeline.recordSpeedCalibration(200, 15);
        QCOMPARE(pipeline.speedCalibration().size(), 1);
        QCOMPARE(pipeline.speedCalibration()[0].watts, 200.0);
    }

    // ---- Stopping ----

    void stopHoldsATrainerAtZero() {
        hear(awotest::trainerPage(300, 200, 85, 0, 0, 5));
        ride({{Role::Power, {17, 300}}});
        bridge_->setTrainerTargetPower(250);
        pipeline_->stop();
        QCOMPARE(*bridge_->trainerTarget()->watts, 0);
        QVERIFY(!pipeline_->isActive());
        // stopping twice is fine
        pipeline_->stop();
    }

private:
    // A started pipeline on the bridge, connected to the selection, recording what it shows
    void ride(const Selection& selection, bool hrm = false, bool speedCadence = false, bool cadence = false,
              bool speed = false, bool cadenceMode = false) {
        AppState& state = appState();
        state.workoutMode = state.cadenceMode = cadenceMode;
        state.antHRM = hrm;
        state.antSpeedCadence = speedCadence;
        state.antCadence = cadence;
        state.antSpeed = speed;
        bridge_->requestConnect(selection);
        bridge_->processPendingRequest();
        pipeline_ = std::make_unique<ReadingPipeline>(bridge_.get());
        QObject::connect(pipeline_.get(), &ReadingPipeline::powerShown, [this](int watts) { shown_.power << watts; });
        QObject::connect(pipeline_.get(), &ReadingPipeline::heartRateShown, [this](int bpm) { shown_.heartRate << bpm; });
        QObject::connect(pipeline_.get(), &ReadingPipeline::cadenceShown, [this](double rpm, int decimals) {
            shown_.cadence << rpm;
            shown_.cadenceDecimals << decimals;
        });
        QObject::connect(pipeline_.get(), &ReadingPipeline::speedShown, [this](double mph) { shown_.speed << mph; });
        QObject::connect(pipeline_.get(), &ReadingPipeline::distanceShown, [this](double miles) { shown_.distance << miles; });
        pipeline_->start();
    }

    void hear(const antnative::RawMessage& message) { ant_->stick->deliver(message); }

    std::unique_ptr<awotest::TempWorkingDir> dir_;
    std::unique_ptr<FakeAnt> ant_;
    std::unique_ptr<AntBridge> bridge_;
    std::unique_ptr<ReadingPipeline> pipeline_;
    Shown shown_;
};

QTEST_GUILESS_MAIN(TestReadingPipeline)
#include "tst_reading_pipeline.moc"

// The session's rides (src/core/Session): tests/test_session.py and golden/session.json

#include "AppState.hpp"
#include "Session.hpp"
#include "TestSupport.hpp"

using namespace awo;

namespace {

// A ride as the app leaves it at its synopsis: power read every second from START
void ridden(int seconds, bool workoutMode = true, bool cadenceMode = false, const QString& ftpTestMode = {},
            int ftp = 200) {
    AppState& state = appState();
    state.ftp = ftp;
    state.workoutMode = workoutMode;
    state.cadenceMode = cadenceMode;
    state.ftpTestMode = ftpTestMode;
    state.ftpRanges = ftpcolors::powerRanges(ftp);
    state.workoutTitle = QStringLiteral("Test Ride");
    state.workoutFilenameShort = QStringLiteral("test.awo");
    state.startTimestamp = QDateTime::currentDateTime().addSecs(-seconds);
    session::beginRide();
    const double start = state.startTimestamp.toMSecsSinceEpoch() / 1000.0;
    for (int t = 1; t < seconds; ++t) {
        workoutData().power.appendAt(start + t, {200});
    }
    state.secondsPassed = seconds;
    state.blockGrades.clear();
    grading::Grade grade;
    grade.pct = 90.0;
    grade.ridden = seconds;
    grade.end = seconds;
    grade.label = QStringLiteral("200W");
    state.blockGrades.set({0, 0, 0}, grade);
}

Ride ride(RideKind kind, const QString& name = {}) {
    Ride r;
    r.kind = kind;
    r.name = name;
    return r;
}

}  // namespace

class TestSession : public QObject {
    Q_OBJECT

private slots:
    void init() {
        appState() = AppState();
        workoutData().clear();
    }

    void aRideJoinsOnceAfterTheOnesBefore() {
        ridden(60);
        session::addCurrentRide();
        // (the synopsis can be built again for the same ride: it's still just the one)
        session::addCurrentRide();
        QCOMPARE(session::rides().size(), 1);
        const Ride first = session::rides()[0];
        QCOMPARE(first.kind, RideKind::Workout);
        QVERIFY(first.graded);
        QCOMPARE(first.start, 0.0);
        QVERIFY(std::abs(first.seconds - 60) < 0.01);
        QCOMPARE(first.data.power.size(), 59);
        // its readings are its own copy: the next ride's start over
        workoutData().clear();
        QCOMPARE(session::rides()[0].data.power.size(), 59);

        ridden(30, false);
        session::addCurrentRide();
        const Ride second = session::rides()[1];
        QCOMPARE(second.kind, RideKind::Free);
        QVERIFY(!second.graded);
        QVERIFY(std::abs(second.start - 60) < 0.01);
        QVERIFY(std::abs(session::seconds() - 90) < 0.01);

        session::clear();
        QVERIFY(session::rides().isEmpty());
    }

    void eachRideKeepsTheZonesFromItsStart() {
        ridden(60, true, false, {}, 200);
        const ftpcolors::ZoneRanges zones = ftpcolors::powerRanges(200);
        // the FTP changed after START (an FTP test's Apply FTP), before the ride joined
        appState().ftp = 300;
        session::addCurrentRide();
        const Ride r = session::rides()[0];
        QCOMPARE(r.ftp, 200);
        QCOMPARE(r.powerRanges, zones);
        QVERIFY(r.powerRanges != ftpcolors::powerRanges(300));
    }

    void aMusicalCadenceRideKeepsItsZonesAsCadenceZones() {
        ridden(60, true, true);
        appState().ftpRanges = ftpcolors::cadenceRanges(60);
        session::beginRide();
        session::addCurrentRide();
        const ftpcolors::ZoneRanges ranges = session::rides()[0].targetRanges;
        QVERIFY(ranges.cadence);
        QCOMPARE(ftpcolors::fadeFrom(ranges), ftpcolors::kCadenceFadeFrom);
    }

    void anFtpTestKeepsItsResult() {
        ridden(60, true, false, QStringLiteral("20m"));
        appState().ftpTestResult = ftptest::Result{280.5, true};
        session::addCurrentRide();
        const Ride r = session::rides()[0];
        QCOMPARE(r.kind, RideKind::FtpTest);
        QVERIFY(!r.graded);
        QCOMPARE(r.ftpTestMode, QStringLiteral("20m"));
        QCOMPARE(*r.ftpTestResult->average, 280.5);
    }

    void headings() {
        QCOMPARE(session::heading(ride(RideKind::Free)), QStringLiteral("Free Ride"));
        QCOMPARE(session::heading(ride(RideKind::Workout, QStringLiteral("Tempo"))), QStringLiteral("Workout: Tempo"));
        QCOMPARE(session::heading(ride(RideKind::Cadence, QStringLiteral("Songs"))),
                 QStringLiteral("Musical Cadence: Songs"));
        QCOMPARE(session::heading(ride(RideKind::FtpTest, QStringLiteral("20 Minute FTP Test"))),
                 QStringLiteral("20 Minute FTP Test"));
        QCOMPARE(session::heading(ride(RideKind::Workout)), QStringLiteral("Workout"));
    }

    void notesSayHowLongAndIfNotGraded() {
        Ride r;
        r.secondsPassed = 3600;
        r.graded = true;
        QCOMPARE(session::note(r), QStringLiteral("1h"));
        r.secondsPassed = 90;
        r.graded = false;
        QVERIFY(session::note(r).endsWith(QStringLiteral("not graded")));
    }

    void perSecondAverages() {
        const QList<RingSample> samples = session::perSecond({{0.1, 100, 80}, {0.6, 200, std::nullopt}, {1.2, 300, 90}});
        QCOMPARE(samples, (QList<RingSample>{{0.5, 150, 80}, {1.5, 300, 90}}));
    }

    void ringFromReadings() {
        const std::pair<QList<double>, QList<double>> power{{0.5, 1.5, 2.5, 3.5}, {100, 200, 300, 400}};
        const std::pair<QList<double>, QList<double>> cadence{{0.5, 1.5}, {80, 90}};
        const Ring ring = session::ringFromReadings(8, power, cadence, true);
        QCOMPARE(ring.seconds, 8.0);
        QCOMPARE(ring.samples.size(), 8);
        // the last 3 seconds' power, the last second's cadence
        QCOMPARE(ring.samples[0], (RingSample{0.5, 100, 80}));
        QCOMPARE(ring.samples[3].power, 300.0);
        // the cadence gone quiet with the power still coming in: the lane's outer edge
        QVERIFY(!ring.samples[3].cadence);
        // both quiet: not pedaling
        QCOMPARE(ring.samples[7], (RingSample{7.5, 0, 0}));
        // no cadence to show
        for (const RingSample& sample : session::ringFromReadings(4, power, {}, false).samples) {
            QVERIFY(!sample.cadence);
        }
    }

    // ---- The same as the Python app ----

    void golden() {
        const QJsonObject all = awotest::golden(QStringLiteral("session.json")).toObject();
        const QJsonObject perSecond = all[QStringLiteral("perSecond")].toObject();
        QList<RingSample> samples;
        for (const QJsonValue value : perSecond[QStringLiteral("samples")].toArray()) {
            samples.append({value[0].toDouble(), value[1].toDouble(), awotest::optionalNumber(value[2])});
        }
        const QList<RingSample> result = session::perSecond(samples);
        const QJsonArray expected = perSecond[QStringLiteral("result")].toArray();
        QCOMPARE(result.size(), expected.size());
        for (qsizetype i = 0; i < result.size(); ++i) {
            QVERIFY(awotest::sameNumber(expected[i][0], result[i].seconds));
            QVERIFY(awotest::sameNumber(expected[i][1], result[i].power));
            QVERIFY(awotest::sameNumber(expected[i][2], result[i].cadence));
        }
        const auto readings = [](const QJsonValue& value) {
            std::pair<QList<double>, QList<double>> pair;
            for (const QJsonValue t : value[0].toArray()) {
                pair.first.append(t.toDouble());
            }
            for (const QJsonValue v : value[1].toArray()) {
                pair.second.append(v.toDouble());
            }
            return pair;
        };
        for (const QJsonValue value : all[QStringLiteral("rings")].toArray()) {
            const Ring ring = session::ringFromReadings(value[QStringLiteral("length")].toDouble(),
                                                        readings(value[QStringLiteral("power")]),
                                                        readings(value[QStringLiteral("cadence")]),
                                                        value[QStringLiteral("hasCadence")].toBool());
            const QJsonArray expectedRing = value[QStringLiteral("ring")].toArray();
            QCOMPARE(ring.seconds, expectedRing[0].toDouble());
            const QJsonArray expectedSamples = expectedRing[1].toArray();
            QCOMPARE(ring.samples.size(), expectedSamples.size());
            for (qsizetype i = 0; i < ring.samples.size(); ++i) {
                QVERIFY(awotest::sameNumber(expectedSamples[i][0], ring.samples[i].seconds));
                QVERIFY(awotest::sameNumber(expectedSamples[i][1], ring.samples[i].power));
                QVERIFY(awotest::sameNumber(expectedSamples[i][2], ring.samples[i].cadence));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestSession)
#include "tst_session.moc"

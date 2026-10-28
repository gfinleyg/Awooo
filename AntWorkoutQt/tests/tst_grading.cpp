// Block grading (src/core/WorkoutGrading): tests/test_workout_grading.py and golden/grading.json

#include <functional>

#include "TestSupport.hpp"
#include "WorkoutGrading.hpp"

using namespace awo;
using namespace awo::grading;

namespace {

constexpr double kTick = 0.1;

using Curve = std::function<std::optional<double>(double)>;

Curve constant(std::optional<double> value) {
    return [value](double) { return value; };
}

// Feeds the grader 100ms ticks from start to duration
BlockGrader& ride(BlockGrader& grader, double duration, const Curve& target, const Curve& actual, double start = 0.0) {
    const int steps = static_cast<int>(std::nearbyint((duration - start) / kTick));
    for (int i = 0; i < steps; ++i) {
        const double t = start + i * kTick;
        grader.add(t, kTick, *target(t), actual(t));
    }
    return grader;
}

Grade rideGrade(double duration, double previous, int ftp, bool free, const Curve& target, const Curve& actual) {
    BlockGrader grader(duration, previous, ftp, free);
    return ride(grader, duration, target, actual).finish();
}

bool near(std::optional<double> value, double expected, double tolerance = 1e-6) {
    return value && std::abs(*value - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

Grade record(std::optional<double> pct, double ridden, const QString& label = QStringLiteral("200W 1m"),
             bool endedEarly = false) {
    Grade grade;
    grade.pct = pct;
    grade.ridden = ridden;
    grade.label = label;
    grade.endedEarly = endedEarly;
    return grade;
}

Block steady(int watts, int seconds = 60) {
    Block block;
    block.type = BlockType::Steady;
    block.seconds = seconds;
    block.power.fill(watts);
    return block;
}

}  // namespace

class TestGrading : public QObject {
    Q_OBJECT

private slots:
    void letters_data() {
        QTest::addColumn<double>("pct");
        QTest::addColumn<QString>("grade");
        const QList<std::pair<double, const char*>> cases = {
            {100.01, "S"}, {100, "A+"}, {97, "A+"}, {96.99, "A"}, {93, "A"}, {90, "A-"}, {87, "B+"}, {83, "B"},
            {80, "B-"},    {77, "C+"},  {73, "C"},  {70, "C-"},   {67, "D+"}, {63, "D"}, {60, "D-"}, {59.99, "F"}, {0, "F"},
        };
        for (const auto& [pct, grade] : cases) {
            QTest::newRow(qPrintable(QString::number(pct))) << pct << QString::fromUtf8(grade);
        }
    }

    void letters() {
        QFETCH(double, pct);
        QFETCH(QString, grade);
        QCOMPARE(letter(pct), grade);
        QCOMPARE(letter(std::nullopt), QStringLiteral("--"));
    }

    void gradeColors() {
        QCOMPARE(gradeColor(QStringLiteral("A+")), gradeColor(QStringLiteral("A")));
        QCOMPARE(gradeColor(QStringLiteral("S")), QStringLiteral("#f5c400"));
        QCOMPARE(gradeColor(QStringLiteral("--")), kNoGradeColor);
    }

    void gracePeriods() {
        QCOMPARE(startGrace(300, 0), 3.0);
        QVERIFY(near(startGrace(300, -200), 7));
        QCOMPARE(startGrace(300, 5000), kGraceMaxSeconds);
        QVERIFY(near(startGrace(8, 200), 2));  // a quarter of the block at most
        QCOMPARE(endGrace(300), 2.0);
        QVERIFY(near(endGrace(10), 1));
    }

    void onTargetSteadyBlock() {
        const Grade grade = rideGrade(60, 200, 250, false, constant(200), constant(200));
        // 0.8 FTP: the intensity bonus takes a perfect ride over 100%, an S
        QVERIFY(near(grade.avg, 100) && near(grade.low, 100) && near(grade.high, 100));
        QVERIFY(near(grade.bonus, 100 * 0.04 * 0.3));
        QVERIFY(near(grade.pct, 101.2));
        QCOMPARE(letter(grade.pct), QStringLiteral("S"));
        QVERIFY(std::abs(grade.graded - (60 - 3 - 2)) <= 0.11);
        QVERIFY(!grade.endedEarly);
        QCOMPARE(grade.ridden, 60.0);
    }

    void underTarget() {
        const Grade grade = rideGrade(60, 200, 250, false, constant(200), constant(180));
        QVERIFY(near(grade.avg, 90));
        QCOMPARE(*grade.penalty, 0.0);
        QVERIFY(near(grade.pct, 90 + 1.2));
    }

    void overTargetIsPenalized() {
        const Grade grade = rideGrade(60, 200, 250, false, constant(200), constant(240));
        // 20% over at 0.8 FTP: 0.5 x (0.2 - 0.03) over the average, 0.2 x (1.2 - 1.1) surge
        QVERIFY(near(grade.overAvg, 8.5));
        QVERIFY(near(grade.surge, 2));
        QVERIFY(near(grade.pct, 100 - 10.5 + 1.2));
    }

    void overTargetPenaltyIsCapped() {
        QVERIFY(near(rideGrade(60, 200, 250, false, constant(200), constant(1000)).penalty, 100 * kMaxOverPenalty));
    }

    void recoveryBlocksAreStricterAboutGoingOver() {
        // recovery slope: 1.0 x (0.2 - 0.03)
        QVERIFY(near(rideGrade(60, 100, 250, false, constant(100), constant(120)).overAvg, 17));
    }

    void hardBlocksHaveMoreTolerance() {
        // 2x FTP, 6% over: tolerance 8% at 2x FTP, nothing over the average, no surge
        const Grade hard = rideGrade(60, 500, 250, false, constant(500), constant(530));
        QCOMPARE(*hard.overAvg, 0.0);
        QCOMPARE(*hard.surge, 0.0);
        QVERIFY(near(hard.bonus, 4));
    }

    void oneSlowStretchLowersTheLow() {
        const Grade grade = rideGrade(60, 200, 250, false, constant(200),
                                      [](double t) { return std::optional<double>(20 <= t && t < 25 ? 100 : 200); });
        QVERIFY(std::abs(*grade.low - 50) <= 1);
        QVERIFY(*grade.pct < 95);
    }

    void dropoutsAreLeftOut() {
        const Grade grade = rideGrade(60, 200, 250, false, constant(200), [](double t) {
            return 20 <= t && t < 30 ? std::nullopt : std::optional<double>(200);
        });
        QVERIFY(std::abs(grade.dropped - 10) <= 0.11);
        QVERIFY(near(grade.avg, 100));
    }

    void startAndEndGraceAreNotGraded() {
        // way off during the grace periods only: a 200W step is 3 + 4 seconds of grace at the start
        const Grade grade = rideGrade(60, 0, 250, false, constant(200),
                                      [](double t) { return std::optional<double>(t < 6.5 || t > 58.5 ? 0 : 200); });
        QVERIFY(near(grade.avg, 100));
        QVERIFY(std::abs(grade.grace - (7 + 2)) <= 0.11);
    }

    void resumeGrace() {
        BlockGrader grader(120, 200, 250, false);
        ride(grader, 30, constant(200), constant(200));
        grader.resume(30, 200);
        // getting back up after the pause isn't graded
        ride(grader, 120, constant(200), [](double t) { return std::optional<double>(t < 36.9 ? 0 : 200); }, 30);
        QVERIFY(near(grader.finish().avg, 100));
    }

    void rampIsComparedSecondBySecond() {
        const Curve rising = [](double t) { return std::optional<double>(100 + t); };
        const Grade grade = rideGrade(120, 100, 250, false, rising, rising);
        QVERIFY(near(grade.avg, 100));
        QVERIFY(near(grade.low, 100));
    }

    void shortBlockUsesTheAverageOnly() {
        const Grade grade = rideGrade(8, 200, 250, false, constant(200), constant(180));
        QVERIFY(!grade.low && !grade.high);
        QVERIFY(near(grade.pct, 90 + 1.2));
    }

    void blockWithReadingsOnlyInItsGraceIsGradedOnThose() {
        // (a 200W step into a 20s block: 5s of start grace, 2s at the end)
        const Grade grade = rideGrade(20, 0, 250, false, constant(200), [](double t) {
            return t < 4.9 || t > 18.1 ? std::optional<double>(180) : std::nullopt;
        });
        QVERIFY(near(grade.avg, 90));
        QCOMPARE(grade.grace, 0.0);
        QVERIFY(std::abs(grade.graded - 6.8) <= 0.11);
    }

    void freeRide() {
        const Grade grade =
            rideGrade(60, 0, 250, true, constant(0), [](double t) { return std::optional<double>(t < 15 ? 0 : 150); });
        QVERIFY(grade.free);
        // (the last 2s are end grace)
        QVERIFY(std::abs(*grade.pedaling - 100.0 * 43 / 58) <= 0.3);
        QCOMPARE(grade.pct, grade.pedaling);
    }

    void nothingGraded() {
        Grade grade = BlockGrader(60, 0, 250, false).finish();
        QVERIFY(!grade.pct);
        QCOMPARE(grade.graded, 0.0);
        grade = rideGrade(60, 0, 250, false, constant(200), constant(std::nullopt));
        QVERIFY(!grade.pct && grade.dropped > 0);
        // a zero target has nothing to compare to
        QVERIFY(!rideGrade(60, 0, 250, false, constant(0), constant(100)).pct);
        // zero length ticks are ignored
        BlockGrader grader(60, 0, 250, false);
        grader.add(10, 0, 200, 200);
        QVERIFY(!grader.firstTarget());
    }

    void endedEarly() {
        BlockGrader grader(300, 200, 250, false);
        ride(grader, 100, constant(200), constant(200));
        const Grade grade = grader.finish(100);
        QVERIFY(grade.endedEarly);
        QCOMPARE(grade.ridden, 100.0);
        QVERIFY(!grader.finish(300).endedEarly);
    }

    void rollingRatiosAndPercentile() {
        QList<Sample> samples;
        for (int i = 0; i < 5; ++i) {
            samples.append({1.0, 100, 100});
        }
        for (int i = 0; i < 5; ++i) {
            samples.append({1.0, 100, 200});
        }
        const QList<double> ratios = rollingRatios(samples);
        QVERIFY(near(ratios.first(), 1.0) && near(ratios.last(), 2.0));
        QCOMPARE(ratios.size(), 8);
        // less than one window: the whole
        QCOMPARE(rollingRatios({{0.5, 100, 50}}), QList<double>{0.5});
        QCOMPARE(rollingRatios({{0.5, 0, 50}}), QList<double>{0.0});
        QCOMPARE(percentile({5, 1, 3, 2, 4}, 0.95), 5.0);
        QCOMPARE(percentile({5, 1, 3, 2, 4}, 0.5), 3.0);
        QCOMPARE(percentile({7}, 0.95), 7.0);
    }

    void latestPowerReading() {
        QVERIFY(!latestPower({}, {}, 10));
        QCOMPARE(*latestPower({8, 9}, {100, 150}, 10), 150.0);
        QVERIFY(!latestPower({7}, {100}, 10));
    }

    void rows() {
        Block rest;
        rest.seconds = 30;
        const QList<WorkoutEntry> workout = {WorkoutEntry::ofBlock(steady(200)),
                                             WorkoutEntry::interval(3, {steady(200), rest}),
                                             WorkoutEntry::intervalBlock(3, 0, steady(200)),
                                             WorkoutEntry::ofBlock(steady(200))};
        Grades grades;
        grades.set({0, 0, 0}, record(95, 60));
        grades.set({1, 0, 0}, record(101, 60, QStringLiteral("300W 1m")));
        grades.set({1, 0, 1}, record(85, 60, QStringLiteral("300W 1m")));
        grades.set({1, 1, 0}, record(std::nullopt, 30, QStringLiteral("Free 30s")));
        grades.set({1, 0, 2}, record(70, 30, QStringLiteral("300W 1m"), true));
        // block 3 never reached
        const QList<GradeRow> rows = workoutRows(workout, grades);
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].kind, GradeRow::Kind::Block);
        QCOMPARE(rows[0].grade, QStringLiteral("A"));
        QVERIFY(rows[0].note.isEmpty());
        QCOMPARE(rows[1].kind, GradeRow::Kind::Interval);
        QCOMPARE(rows[1].label, QStringLiteral("3x"));
        // repeats weighted by the time ridden
        QVERIFY(near(rows[2].pct, (101.0 * 60 + 85 * 60 + 70 * 30) / 150));
        QCOMPARE(rows[2].note, QStringLiteral("S · B · C- (ended)"));
        QVERIFY(!rows[3].pct);
        QCOMPARE(rows[3].grade, QStringLiteral("--"));
    }

    void rowsSongs() {
        const QList<WorkoutEntry> workout(5, WorkoutEntry::ofBlock(steady(90)));
        Grades grades;
        grades.set({0, 0, 0}, record(100, 60, QStringLiteral("90rpm 1m")));
        grades.set({1, 0, 0}, record(80, 30, QStringLiteral("90rpm 1m")));
        grades.set({2, 0, 0}, record(std::nullopt, 60, QStringLiteral("Free Ride 1m")));
        grades.set({3, 0, 0}, record(90, 60, QStringLiteral("90rpm 1m")));
        // block 4, the last song's, never reached
        const QMap<int, QString> songs = {{0, QStringLiteral("Band - First")}, {2, QStringLiteral("Band - Second")},
                                          {4, QStringLiteral("Band - Third")}};
        const QList<GradeRow> rows = workoutRows(workout, grades, songs);
        QList<std::pair<GradeRow::Kind, bool>> shape;
        for (const GradeRow& row : rows) {
            shape.append({row.kind, row.indent});
        }
        QCOMPARE(shape, (QList<std::pair<GradeRow::Kind, bool>>{
                            {GradeRow::Kind::Song, false}, {GradeRow::Kind::Block, true}, {GradeRow::Kind::Block, true},
                            {GradeRow::Kind::Song, false}, {GradeRow::Kind::Block, true}, {GradeRow::Kind::Block, true}}));
        QCOMPARE(rows[0].label, QStringLiteral("Band - First"));
        QVERIFY(near(rows[0].pct, (100.0 * 60 + 80 * 30) / 90));
        QCOMPARE(rows[0].grade, letter(rows[0].pct));
        // an ungraded (free ride) block leaves the song's grade to the others
        QVERIFY(near(rows[3].pct, 90));
        // no songs: no song rows, nothing indented
        for (const GradeRow& row : workoutRows(workout, grades)) {
            QCOMPARE(row.kind, GradeRow::Kind::Block);
            QVERIFY(!row.indent);
        }
    }

    void average() {
        QVERIFY(!workoutAverage(Grades()));
        Grades one;
        one.set({1, 0, 0}, record(std::nullopt, 60));
        QVERIFY(!workoutAverage(one));
        Grades grades;
        grades.set({1, 0, 0}, record(100, 60));
        grades.set({2, 0, 0}, record(50, 20));
        grades.set({3, 0, 0}, record(std::nullopt, 60));
        grades.set({4, 0, 0}, record(10, 0));
        QVERIFY(near(workoutAverage(grades), (100.0 * 60 + 50 * 20) / 80));
    }

    // ---- The same as the Python app ----

    void golden() {
        const QJsonObject all = awotest::golden(QStringLiteral("grading.json")).toObject();
        int scenario = 0;
        for (const QJsonValue value : all[QStringLiteral("scenarios")].toArray()) {
            const QJsonObject c = value.toObject();
            BlockGrader grader(c[QStringLiteral("duration")].toDouble(), c[QStringLiteral("previous")].toDouble(),
                               c[QStringLiteral("ftp")].toInt(), c[QStringLiteral("free")].toBool());
            const std::optional<double> resumeAt = awotest::optionalNumber(c[QStringLiteral("resumeAt")]);
            bool resumed = false;
            for (const QJsonValue tickValue : c[QStringLiteral("ticks")].toArray()) {
                const QJsonArray tick = tickValue.toArray();
                const double t = tick[0].toDouble();
                if (resumeAt && !resumed && t >= *resumeAt) {
                    grader.resume(t, tick[2].toDouble());
                    resumed = true;
                }
                grader.add(t, tick[1].toDouble(), tick[2].toDouble(), awotest::optionalNumber(tick[3]));
            }
            const Grade grade = grader.finish(awotest::optionalNumber(c[QStringLiteral("endedAt")]));
            const QJsonObject expected = c[QStringLiteral("grade")].toObject();
            const QString where = QStringLiteral("scenario %1").arg(scenario++);
            const auto check = [&](const char* name, std::optional<double> ours) {
                QVERIFY2(awotest::sameNumber(expected.value(QLatin1String(name)), ours),
                         qPrintable(where + u' ' + QLatin1String(name)));
            };
            check("pct", grade.pct);
            check("graded", grade.graded);
            check("dropped", grade.dropped);
            check("grace", grade.grace);
            check("duration", grade.duration);
            check("ridden", grade.ridden);
            check("pedaling", grade.pedaling);
            check("avg", grade.avg);
            check("low", grade.low);
            check("high", grade.high);
            check("overAvg", grade.overAvg);
            check("surge", grade.surge);
            check("penalty", grade.penalty);
            check("bonus", grade.bonus);
            check("intensity", grade.intensity);
            QCOMPARE(grade.free, expected[QStringLiteral("free")].toBool());
            QCOMPARE(grade.endedEarly, expected[QStringLiteral("endedEarly")].toBool());
        }
        for (const QJsonValue value : all[QStringLiteral("rolling")].toArray()) {
            QList<Sample> samples;
            for (const QJsonValue sample : value[QStringLiteral("samples")].toArray()) {
                samples.append({sample[0].toDouble(), sample[1].toDouble(), sample[2].toDouble()});
            }
            const QList<double> ratios = rollingRatios(samples);
            const QJsonArray expected = value[QStringLiteral("ratios")].toArray();
            QCOMPARE(ratios.size(), expected.size());
            for (qsizetype i = 0; i < ratios.size(); ++i) {
                QVERIFY(awotest::sameNumber(expected[i], ratios[i]));
            }
        }
        for (const QJsonValue value : all[QStringLiteral("letters")].toArray()) {
            QCOMPARE(letter(awotest::optionalNumber(value[0])), value[1].toString());
        }
    }
};

QTEST_GUILESS_MAIN(TestGrading)
#include "tst_grading.moc"

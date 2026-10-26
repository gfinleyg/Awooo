// The FTP tests (src/core/FtpTest): tests/test_ftp_test.py and golden/ftptest.json

#include "AwoParser.hpp"
#include "FtpTest.hpp"
#include "TestSupport.hpp"

using namespace awo;

class TestFtpTest : public QObject {
    Q_OBJECT

private slots:
    void warmupLength() {
        int seconds = 0;
        for (const QString& line : ftptest::kWarmupLines) {
            seconds += line.section(u':', 1).section(u',', 0, 0).toInt();
        }
        QCOMPARE(ftptest::kWarmupSeconds, seconds);
        QCOMPARE(ftptest::kWarmupSeconds, 180 + 60 + 120 + 60 + 480);
    }

    void findTests() {
        QVERIFY(!ftptest::find(QString()));
        QCOMPARE(ftptest::find(QStringLiteral("ramp20"))->stepWatts, 20);
        QVERIFY(ftptest::isRamp(QStringLiteral("ramp10")) && !ftptest::isRamp(QStringLiteral("1h")));
    }

    void effortTestLines_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("effort");
        QTest::newRow("20m") << "20m" << 1200;
        QTest::newRow("1h") << "1h" << 3600;
    }

    void effortTestLines() {
        QFETCH(QString, mode);
        QFETCH(int, effort);
        QCOMPARE(ftptest::workoutLines(mode), QStringList{QStringLiteral("T:") + ftptest::find(mode)->name} +
                                                  ftptest::kWarmupLines + QStringList{QStringLiteral("0:%1").arg(effort)});
        // no FTP: the warmup is a free ride as long
        QCOMPARE(ftptest::workoutLines(mode, false).mid(1),
                 (QStringList{QStringLiteral("0:%1").arg(ftptest::kWarmupSeconds), QStringLiteral("0:%1").arg(effort)}));
    }

    void rampTestLinesAndBlocks() {
        QCOMPARE(ftptest::workoutLines(QStringLiteral("ramp10")),
                 QStringList{QStringLiteral("T:10W delta FTP Ramp")} + ftptest::kWarmupLines);
        const auto watts = [](const QString& mode, int index) {
            const WorkoutEntry entry = ftptest::rampBlock(mode, index);
            return std::tuple(entry.block.type, entry.block.seconds, entry.block.power[0], entry.block.power[20]);
        };
        QCOMPARE(watts(QStringLiteral("ramp10"), 0), std::tuple(BlockType::Steady, 60, 50, 50));
        QCOMPARE(watts(QStringLiteral("ramp10"), 3), std::tuple(BlockType::Steady, 60, 80, 80));
        QCOMPARE(watts(QStringLiteral("ramp20"), 2), std::tuple(BlockType::Steady, 60, 140, 140));
    }

    void workoutLinesAreValidAwo() {
        // (an effort longer than a workout file's blocks is let through, as the setup screen builds the test)
        for (const ftptest::Test& test : ftptest::tests()) {
            for (bool ftpSet : {true, false}) {
                const AwoResult result = parseAwoLines(ftptest::workoutLines(test.key, ftpSet),
                                                       {ftpSet ? 250 : 0, std::max(kMaxBlockSeconds, test.effortSeconds)});
                QVERIFY(result.ok());
                QCOMPARE(result.workout.title, test.name);
            }
        }
    }

    void effortMessages() {
        for (const QString& mode : {QStringLiteral("20m"), QStringLiteral("1h")}) {
            const int start = 900;
            const QList<Message> messages = ftptest::messages(mode, start, 250);
            const int effortEnd = start + ftptest::find(mode)->effortSeconds;
            bool mentionsFtp = false;
            for (const Message& message : messages) {
                QVERIFY(message.start < message.end && !message.text.isEmpty());
                mentionsFtp = mentionsFtp || message.text.contains(QStringLiteral("250"));
                if (message.text == u"GO!") {
                    QCOMPARE(std::pair(message.start, message.end), std::pair(start, start + ftptest::kGoSeconds));
                }
                if (message.text == u"Get Ready...") {
                    QCOMPARE(std::pair(message.start, message.end), std::pair(start - 10, start));
                }
                if (message.text.startsWith(QStringLiteral("Two minutes left"))) {
                    QCOMPARE(message.start, effortEnd - 120);
                }
            }
            QVERIFY(mentionsFtp);
            QCOMPARE(messages.size() == 6, mode == u"20m");
        }
    }

    void rampMessages() {
        const QList<Message> messages = ftptest::messages(QStringLiteral("ramp10"), 900, 0);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(std::pair(messages[0].start, messages[0].end), std::pair(870, 885));
        QCOMPARE(std::pair(messages[1].start, messages[1].end), std::pair(885, 900));
        QVERIFY(messages[0].text.contains(QStringLiteral("10W")));
    }

    void ftpFromAverage() {
        QCOMPARE(ftptest::ftpFromAverage(QStringLiteral("20m"), 300), 285);
        QCOMPARE(ftptest::ftpFromAverage(QStringLiteral("1h"), 251.6), 252);
        QCOMPARE(ftptest::ftpFromAverage(QStringLiteral("ramp10"), 400), 300);
    }

    void effortAverageIsTimeWeightedAndSkipsDropouts() {
        ftptest::EffortAverage average;
        QVERIFY(!average.average());
        average.add(1, 100);
        average.add(3, 200);
        average.add(5, std::nullopt);  // dropout
        average.add(0, 1000);          // no time
        average.add(-1, 1000);
        QCOMPARE(*average.average(), 175.0);
    }

    void bestRollingAverage() {
        ftptest::BestRollingAverage best(60);
        for (int i = 0; i < 30; ++i) {
            best.add(1, 100);
        }
        QVERIFY(!best.average());  // not a full window yet
        for (int i = 0; i < 30; ++i) {
            best.add(1, 300);
        }
        QCOMPARE(*best.average(), 200.0);
        for (int i = 0; i < 30; ++i) {
            best.add(1, 300);
        }
        QCOMPARE(*best.average(), 300.0);
        // worse later on doesn't lower the best
        for (int i = 0; i < 60; ++i) {
            best.add(1, std::nullopt);  // not pedaling counts as 0
        }
        QCOMPARE(*best.average(), 300.0);
    }

    void bestRollingAverageNewWindow() {
        ftptest::BestRollingAverage best(10);
        for (int i = 0; i < 5; ++i) {
            best.add(1, 500);
        }
        best.startNewWindow();
        for (int i = 0; i < 10; ++i) {
            best.add(1, 100);
        }
        // the 500s from before don't count toward the next window
        QCOMPARE(*best.average(), 100.0);
        best.add(0, 1000);
        QCOMPARE(*best.average(), 100.0);
    }

    // ---- The same as the Python app ----

    void golden() {
        const QJsonObject all = awotest::golden(QStringLiteral("ftptest.json")).toObject();
        for (const QJsonValue value : all[QStringLiteral("messages")].toArray()) {
            const QJsonObject c = value.toObject();
            AWO_COMPARE_JSON(awotest::toPython(ftptest::messages(c[QStringLiteral("mode")].toString(),
                                                                 c[QStringLiteral("start")].toInt(),
                                                                 c[QStringLiteral("ftp")].toInt())),
                             c[QStringLiteral("messages")]);
        }
        for (const QJsonValue value : all[QStringLiteral("averages")].toArray()) {
            const QJsonArray c = value.toArray();
            QCOMPARE(ftptest::ftpFromAverage(c[0].toString(), c[1].toDouble()), c[2].toInt());
        }
        for (const QJsonValue value : all[QStringLiteral("rolling")].toArray()) {
            const QJsonObject c = value.toObject();
            ftptest::BestRollingAverage best(c[QStringLiteral("window")].toDouble());
            ftptest::EffortAverage effort;
            for (const QJsonValue stepValue : c[QStringLiteral("steps")].toArray()) {
                const QJsonArray step = stepValue.toArray();
                if (step[0].isString()) {
                    best.startNewWindow();
                    continue;
                }
                best.add(step[0].toDouble(), awotest::optionalNumber(step[1]));
                effort.add(step[0].toDouble(), awotest::optionalNumber(step[1]));
                QVERIFY(awotest::sameNumber(step[2], best.average()));
                QVERIFY(awotest::sameNumber(step[3], effort.average()));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestFtpTest)
#include "tst_ftp_test.moc"

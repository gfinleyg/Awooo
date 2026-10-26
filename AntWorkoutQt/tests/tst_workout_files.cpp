// Reading .awo workout files (src/core/AwoParser): tests/test_workout_files.py, and every case in golden/awo.json
// read the same as the Python app reads it

#include <QDirIterator>

#include "AwoParser.hpp"
#include "TestSupport.hpp"

using namespace awo;

namespace {

AwoResult parse(const QStringList& lines, int ftp = 200, int maxBlockSeconds = kMaxBlockSeconds) {
    return parseAwoLines(lines, {ftp, maxBlockSeconds, false});
}

// a steady block's 21 targets at FTP 200
Targets wattsByDifficulty(double power) {
    Targets values{};
    for (int i = -10; i <= 10; ++i) {
        values[i + 10] = static_cast<int>(power * (200 + static_cast<int>((200 * i) * .02)));
    }
    return values;
}

Block steadyBlock(int seconds, double power, int position = 0) {
    Block block;
    block.type = BlockType::Steady;
    block.seconds = seconds;
    block.power = wattsByDifficulty(power);
    block.position = position;
    return block;
}

Block freeBlock(int seconds, int position = 0) {
    Block block;
    block.seconds = seconds;
    block.position = position;
    return block;
}

QList<WorkoutEntry> entriesOfType(const QList<WorkoutEntry>& entries, WorkoutEntry::Type type) {
    QList<WorkoutEntry> found;
    for (const WorkoutEntry& entry : entries) {
        if (entry.type == type) {
            found << entry;
        }
    }
    return found;
}

}  // namespace

class TestWorkoutFiles : public QObject {
    Q_OBJECT

private slots:
    void init() { dir_ = std::make_unique<awotest::TempWorkingDir>(); }
    void cleanup() { dir_.reset(); }

    // ---- What a workout reads as ----

    void freeSteadyAndTitle() {
        const AwoResult result = parse({QStringLiteral("T: My Ride "), QStringLiteral("0:60,3"),
                                        QStringLiteral("1:300,0.75"), QString()});
        QVERIFY(result.ok());
        QCOMPARE(result.workout.title, QStringLiteral("My Ride"));
        QCOMPARE(result.workout.entries,
                 (QList<WorkoutEntry>{WorkoutEntry::ofBlock(freeBlock(60, 3)),
                                      WorkoutEntry::ofBlock(steadyBlock(300, 0.75))}));
        QCOMPARE(result.workout.entries[1].block.power[10], 150);
        QCOMPARE(result.workout.entries[1].block.power[0], 120);
        QCOMPARE(result.workout.seconds, 360);
        QVERIFY(result.workout.messages.isEmpty());
    }

    void explicitWattsMoveWithDifficultyButNotFtp() {
        const Targets powers = parse({QStringLiteral("1:60,250W")}).workout.entries[0].block.power;
        QCOMPARE(powers[10], 250);
        QCOMPARE(powers[0], 200);
        QCOMPARE(powers[20], 300);
        QCOMPARE(parse({QStringLiteral("1:60,250W")}, 300).workout.entries[0].block.power[10], 250);
    }

    void rampStepsCoverTheBlock() {
        const AwoResult result = parse({QStringLiteral("2:120,0.5,1.0,2")});
        QVERIFY(result.ok());
        const Block& block = result.workout.entries[0].block;
        QCOMPARE(block.type, BlockType::Ramp);
        QCOMPARE(block.seconds, 120);
        QCOMPARE(block.position, 2);
        QCOMPARE(block.power[10], 100);
        QCOMPARE(block.powerEnd[10], 200);
        const QList<WorkoutEntry> steps = result.workout.entries.mid(1);
        int total = 0;
        int last = 0;
        for (const WorkoutEntry& step : steps) {
            QCOMPARE(step.type, WorkoutEntry::Type::RampStep);
            QCOMPARE(step.rampSeconds, 120);
            QVERIFY(step.stepSeconds >= 1);  // at most a step a second
            QVERIFY(step.stepPower[10] >= last);  // only going up
            last = step.stepPower[10];
            total += step.stepSeconds;
        }
        QCOMPARE(total, 120);
        QCOMPARE(steps.last().stepPower[10], 200);
    }

    void rampDown() {
        const QList<WorkoutEntry> steps = parse({QStringLiteral("2:60,1.0,0.5")}).workout.entries.mid(1);
        for (qsizetype i = 1; i < steps.size(); ++i) {
            QVERIFY(steps[i].stepPower[10] <= steps[i - 1].stepPower[10]);
        }
        QCOMPARE(steps.last().stepPower[10], 100);
    }

    void rampTooShallowForAWholeWattStillGetsAStep() {
        const QList<WorkoutEntry> steps = parse({QStringLiteral("2:20,0.600,0.602")}).workout.entries.mid(1);
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps[0].stepSeconds, 20);
    }

    void intervals() {
        const AwoResult result = parse({QStringLiteral("3:3;1:30,1.2,3;2:20,0.5,0.6;0:10,1")});
        const WorkoutEntry& header = result.workout.entries[0];
        QCOMPARE(header.type, WorkoutEntry::Type::Interval);
        QCOMPARE(header.repeats, 3);
        QCOMPARE(header.intervalBlocks.size(), 3);
        // each repeat's blocks, counting the repeats down
        QList<std::pair<int, int>> order;
        for (const WorkoutEntry& entry : entriesOfType(result.workout.entries, WorkoutEntry::Type::IntervalBlock)) {
            order.append({entry.repeats, entry.subIndex});
        }
        QCOMPARE(order, (QList<std::pair<int, int>>{{3, 0}, {3, 1}, {3, 2}, {2, 0}, {2, 1}, {2, 2}, {1, 0}, {1, 1},
                                                     {1, 2}}));
        // each repeat's ramp has its own steps, right after its ramp block
        for (qsizetype i = 0; i < result.workout.entries.size(); ++i) {
            const WorkoutEntry& entry = result.workout.entries[i];
            if (entry.type == WorkoutEntry::Type::IntervalBlock && entry.subIndex == 1) {
                QCOMPARE(result.workout.entries[i + 1].type, WorkoutEntry::Type::RampStep);
            }
        }
        QCOMPARE(result.workout.seconds, 3 * (30 + 20 + 10));
    }

    void rampWithTheSameStartAndEndIsSteady() {
        const AwoResult result = parse({QStringLiteral("2:60,0.5,0.5,3"), QStringLiteral("3:2;2:30,250W,250W;0:30")});
        QCOMPARE(result.workout.entries[0], WorkoutEntry::ofBlock(steadyBlock(60, 0.5, 3)));
        // in an interval too: a steady block, no ramp steps after it
        QCOMPARE(result.workout.entries[1].intervalBlocks[0].type, BlockType::Steady);
        QList<WorkoutEntry::Type> types;
        for (const WorkoutEntry& entry : result.workout.entries.mid(1)) {
            types << entry.type;
        }
        QCOMPARE(types, (QList<WorkoutEntry::Type>{WorkoutEntry::Type::Interval, WorkoutEntry::Type::IntervalBlock,
                                                    WorkoutEntry::Type::IntervalBlock, WorkoutEntry::Type::IntervalBlock,
                                                    WorkoutEntry::Type::IntervalBlock}));
        QCOMPARE(result.workout.seconds, 180);
    }

    void oneBlockIntervalOfASteadyBlockIsOneLongerBlock() {
        const AwoResult result = parse({QStringLiteral("3:4;1:30,1.2,3|Go|5"), QStringLiteral("3:2;0:45")});
        QCOMPARE(result.workout.entries, (QList<WorkoutEntry>{WorkoutEntry::ofBlock(steadyBlock(120, 1.2, 3)),
                                                              WorkoutEntry::ofBlock(freeBlock(90))}));
        QCOMPARE(result.workout.seconds, 210);
        QCOMPARE(result.workout.messages, (QList<Message>{{0, 5, QStringLiteral("Go")}, {30, 35, QStringLiteral("Go")},
                                                          {60, 65, QStringLiteral("Go")}, {90, 95, QStringLiteral("Go")}}));
    }

    void oneBlockIntervalOfASamePowerRampIsOneSteadyBlock() {
        const AwoResult result = parse({QStringLiteral("3:3;2:20,0.5,0.50|Up|5|10")});
        QCOMPARE(result.workout.entries, QList<WorkoutEntry>{WorkoutEntry::ofBlock(steadyBlock(60, 0.5))});
        QCOMPARE(result.workout.messages, (QList<Message>{{10, 15, QStringLiteral("Up")}, {30, 35, QStringLiteral("Up")},
                                                          {50, 55, QStringLiteral("Up")}}));
    }

    void oneBlockIntervalUpToABlockLong() {
        QCOMPARE(parse({QStringLiteral("3:2;0:900")}).workout.entries,
                 QList<WorkoutEntry>{WorkoutEntry::ofBlock(freeBlock(1800))});
    }

    void messagesArePlacedInWorkoutTime() {
        const AwoResult result = parse({QStringLiteral("0:30"), QStringLiteral("1:60,0.5|Start|10|0|Later|5|50"),
                                        QStringLiteral("1:20,0.5|Whole block")});
        QCOMPARE(result.workout.messages,
                 (QList<Message>{{30, 40, QStringLiteral("Start")}, {80, 85, QStringLiteral("Later")},
                                 {90, 110, QStringLiteral("Whole block")}}));
    }

    void intervalMessagesShowEveryRep() {
        const AwoResult result = parse({QStringLiteral("0:10"), QStringLiteral("3:2;1:30,1.2|Go|5;0:20|Rest||5")});
        QCOMPARE(result.workout.messages,
                 (QList<Message>{{10, 15, QStringLiteral("Go")}, {45, 60, QStringLiteral("Rest")},
                                 {60, 65, QStringLiteral("Go")}, {95, 110, QStringLiteral("Rest")}}));
    }

    void rejections_data() {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("reason");
        const QList<std::pair<const char*, const char*>> cases = {
            {"Title", "Title line must be"},
            {"T:", "Title line must be"},
            {"4:60", "Invalid block type"},
            {"1:60", "Steady state needs 2 or 3 values"},
            {"1:60,0.5,1,2", "Steady state needs 2 or 3 values"},
            {"0:60:1", "exactly one colon"},
            {"0:1801", "between 1 and 1800s"},
            {"0:0", "between 1 and 1800s"},
            {"0:6.5", "durations must be integers"},
            {"1:60,3.5", "Power must be"},
            {"1:60,0.01", "Power must be"},
            {"1:60,abc", "Power must be"},
            {"1:60,2001W", "Watts must be"},
            {"1:60,0W", "Watts must be"},
            {"1:60,0.5,6", "Position must be 0 to 5"},
            {"0:60,x", "Position must be 0 to 5"},
            {"2:60,0.5", "Ramp needs 3 or 4 values"},
            {"3:2;1:30,1;1:30,1;1:30,1;1:30,1", "Interval needs 1 to 3 blocks"},
            {"3:0;1:30,1;0:30", "repeats must be"},
            {"3:x;1:30,1;0:30", "repeats must be"},
            {"3:1001;1:30,1;0:30", "can be at most 1000"},
            {"3:2;1:30,1;", "Empty interval block"},
            {"3:2|Hi;1:30,1;0:30", "not on 3:<repeats>"},
            {"3:2;1:30,1;7:30", "Invalid block type in interval"},
            {"3;1:30,1;0:30", "Interval must start with 3:<repeats>"},
            {"3:3;2:60,0.5,1.0", "can't be a ramp"},
            {"3:10;1:400,0.6", "at most 1800s in all"},
            {"3:2;0:901", "at most 1800s in all"},
            {"3:2;1:30,1;3:2;1:30,1", "Invalid block type in interval"},
            {"1:60,0.5|", "Message text can't be empty"},
            {"1:60,0.5|Hi|10|60", "start must be before the block ends"},
            {"1:60,0.5|Hi|0", "seconds must be a whole number"},
            {"1:60,0.5|Hi|20|50", "must end by the end of its block"},
            {"1:60,0.5|Hi|5|0|Bye", "Multiple messages need"},
            {"3:2;1:30,1|Hi|40;0:30", "must end by the end of its block"},
        };
        for (const auto& [line, reason] : cases) {
            QTest::newRow(line) << QString::fromUtf8(line) << QString::fromUtf8(reason);
        }
        QTest::newRow("two titles") << QStringLiteral("T:A\nT:B") << QStringLiteral("Only one title");
    }

    void rejections() {
        QFETCH(QString, line);
        QFETCH(QString, reason);
        const AwoResult result = parse(line.split(u'\n'));
        QVERIFY(!result.ok());
        QCOMPARE(result.errors.size(), 1);
        QVERIFY2(result.errors[0].reason.contains(reason), qPrintable(result.errors[0].reason));
    }

    void rejectionSaysWhere() {
        const AwoResult result = parse({QStringLiteral("T:x"), QStringLiteral("1:60,0.5"), QStringLiteral("1:60,9")});
        QCOMPARE(result.errors[0].line, 3);
        QCOMPARE(result.errors[0].text, QStringLiteral("1:60,9"));
    }

    void everyBadLineWhenAskedFor() {
        // (awo-verifier's way)
        const AwoResult result = parseAwoLines({QStringLiteral("1:60,9"), QStringLiteral("0:60"), QStringLiteral("4:1"),
                                                QStringLiteral("T:A"), QStringLiteral("T:B")},
                                               {200, kMaxBlockSeconds, true});
        QCOMPARE(result.errors.size(), 3);
        QCOMPARE(result.errors[0].line, 1);
        QCOMPARE(result.errors[1].line, 3);
        QCOMPARE(result.errors[2].line, 5);
    }

    // ---- Files ----

    void fileWithBomAndTrailingNewline() {
        awotest::writeFile(QStringLiteral("bom.awo"), "\xEF\xBB\xBFT:Bom\n1:60,0.5\n");
        const AwoResult result = readWorkoutFile(QStringLiteral("bom.awo"), {200});
        QVERIFY(result.ok());
        QCOMPARE(result.workout.title, QStringLiteral("Bom"));
        QCOMPARE(result.workout.entries.size(), 1);
    }

    void fileLineEndings() {
        awotest::writeFile(QStringLiteral("crlf.awo"), "T:Crlf\r\n1:60,0.5\r0:30\r\n");
        const AwoResult result = readWorkoutFile(QStringLiteral("crlf.awo"), {200});
        QVERIFY(result.ok());
        QCOMPARE(result.workout.entries.size(), 2);
    }

    void fileNotUtf8() {
        awotest::writeFile(QStringLiteral("latin.awo"), "T:Caf\xe9\n1:60,0.5\n");
        const AwoResult result = readWorkoutFile(QStringLiteral("latin.awo"), {200});
        QVERIFY(!result.ok());
        QVERIFY(result.errors[0].reason.contains(QStringLiteral("UTF-8")));
    }

    void fileWrongType() {
        awotest::writeFile(QStringLiteral("workout.txt"), "1:60,0.5\n");
        const AwoResult result = readWorkoutFile(QStringLiteral("workout.txt"), {200});
        QVERIFY(!result.ok());
        QVERIFY(result.errors[0].reason.contains(QStringLiteral(".awo or .zwo")));
    }

    void missingFile() {
        const AwoResult result = readWorkoutFile(QStringLiteral("nowhere.awo"), {200});
        QVERIFY(!result.ok());
        QVERIFY(result.errors[0].reason.startsWith(QStringLiteral("Can't open")));
    }

    void zwoFileIsReadThroughTheConverter() {
        awotest::writeFile(QStringLiteral("ride.zwo"), R"(<workout_file><name>Z</name><workout>
            <SteadyState Duration="60" Power="0.5"/><Ramp Duration="60" PowerLow="0.5" PowerHigh="0.75"/>
            </workout></workout_file>)");
        const AwoResult result = readWorkoutFile(QStringLiteral("ride.zwo"), {200});
        QVERIFY(result.ok());
        QCOMPARE(result.workout.title, QStringLiteral("Z"));
        QCOMPARE(result.workout.entries[0].block.type, BlockType::Steady);
        QCOMPARE(result.workout.entries[1].block.type, BlockType::Ramp);
    }

    void brokenZwoFileIsRejected() {
        awotest::writeFile(QStringLiteral("broken.zwo"), "<workout_file><workout></workout_file>");
        QVERIFY(!readWorkoutFile(QStringLiteral("broken.zwo"), {200}).ok());
    }

    // ---- The files in the project ----

    void shippedAndScratchpadWorkouts() {
        int files = 0;
        QDirIterator it(awotest::pythonAppDir() + QStringLiteral("/awoWorkouts"), {QStringLiteral("*.awo")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const bool bad = it.fileName().startsWith(QStringLiteral("bad_"));
            const AwoResult result = readWorkoutFile(path, {200});
            QVERIFY2(result.ok() != bad, qPrintable(path));
            if (!bad) {
                QVERIFY(result.workout.seconds > 0);
            }
            ++files;
        }
        QVERIFY(files > 50);
    }

    // ---- The same as the Python app ----

    void golden_data() {
        QTest::addColumn<QJsonObject>("expected");
        QTest::addColumn<QStringList>("lines");
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("maxBlockSeconds");
        const QJsonObject all = awotest::golden(QStringLiteral("awo.json")).toObject();
        for (const QJsonValue value : all.value(QStringLiteral("files")).toArray()) {
            const QJsonObject c = value.toObject();
            const QString name = QStringLiteral("file %1 ftp %2").arg(c[QStringLiteral("file")].toString())
                                     .arg(c[QStringLiteral("ftp")].toInt());
            QTest::newRow(qPrintable(name)) << c << QStringList() << c[QStringLiteral("file")].toString()
                                            << kMaxBlockSeconds;
        }
        for (const QString& kind : {QStringLiteral("lines"), QStringLiteral("lineFiles"), QStringLiteral("ftpTests")}) {
            int index = 0;
            for (const QJsonValue value : all.value(kind).toArray()) {
                const QJsonObject c = value.toObject();
                QStringList lines;
                for (const QJsonValue line : c[QStringLiteral("lines")].toArray()) {
                    lines << line.toString();
                }
                const QString name = QStringLiteral("%1 %2 ftp %3: %4").arg(kind).arg(index++)
                                         .arg(c[QStringLiteral("ftp")].toInt()).arg(lines.join(u'|').left(40));
                QTest::newRow(qPrintable(name)) << c << lines << QString()
                                                << c[QStringLiteral("maxBlockSeconds")].toInt(kMaxBlockSeconds);
            }
        }
    }

    void golden() {
        QFETCH(QJsonObject, expected);
        QFETCH(QStringList, lines);
        QFETCH(QString, file);
        QFETCH(int, maxBlockSeconds);
        const AwoOptions options{expected[QStringLiteral("ftp")].toInt(), maxBlockSeconds, false};
        const AwoResult result =
            file.isEmpty() ? parseAwoLines(lines, options)
                           : readWorkoutFile(awotest::pythonAppDir() + QStringLiteral("/awoWorkouts/") + file, options);
        // (deliberately different: the Python app took any number of repeats)
        if (lines == QStringList{QStringLiteral("3:1001;1:1,0.5;0:1")}) {
            QVERIFY(expected[QStringLiteral("ok")].toBool());
            QVERIFY(!result.ok());
            return;
        }
        QCOMPARE(result.ok(), expected[QStringLiteral("ok")].toBool());
        if (result.ok()) {
            QCOMPARE(result.workout.title, expected[QStringLiteral("title")].toString());
            QCOMPARE(result.workout.seconds, expected[QStringLiteral("seconds")].toInt());
            AWO_COMPARE_JSON(awotest::toPython(result.workout.entries), expected[QStringLiteral("entries")]);
            AWO_COMPARE_JSON(awotest::toPython(result.workout.messages), expected[QStringLiteral("messages")]);
        } else {
            const QJsonArray rejection = expected[QStringLiteral("rejection")].toArray();
            QCOMPARE(result.errors[0].reason, rejection[0].toString());
            QCOMPARE(result.errors[0].line, rejection[1].toInt());
            QCOMPARE(result.errors[0].text, rejection[2].toString());
        }
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_GUILESS_MAIN(TestWorkoutFiles)
#include "tst_workout_files.moc"

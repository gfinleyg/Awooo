// Zwift .zwo files as .awo lines (src/core/ZwoConvert): tests/test_zwo_convert.py and golden/zwo.json

#include "AwoParser.hpp"
#include "TestSupport.hpp"
#include "ZwoConvert.hpp"

using namespace awo;

namespace {

QByteArray zwo(const QByteArray& body, const QByteArray& header = "<name>Test Ride</name>") {
    return "<workout_file>" + header + "<sportType>bike</sportType><workout>" + body + "</workout></workout_file>";
}

ZwoConversion convert(const QByteArray& body, const QByteArray& header = "<name>Test Ride</name>") {
    return convertZwoData(zwo(body, header));
}

QStringList blocks(const ZwoConversion& conversion) {
    return conversion.lines.mid(1);
}

// whatever the converter writes is a workout the app reads
void verifyValid(const QStringList& lines) {
    const AwoResult result = parseAwoLines(lines, {250});
    QVERIFY2(result.ok(), result.ok() ? "" : qPrintable(result.errors[0].reason + u' ' + result.errors[0].text));
}

bool anyContains(const QStringList& texts, const QString& part) {
    for (const QString& text : texts) {
        if (text.contains(part)) {
            return true;
        }
    }
    return false;
}

}  // namespace

class TestZwo : public QObject {
    Q_OBJECT

private slots:
    void init() { dir_ = std::make_unique<awotest::TempWorkingDir>(); }
    void cleanup() { dir_.reset(); }

    void basicBlocks() {
        const ZwoConversion result = convert(R"(
            <Warmup Duration="300" PowerLow="0.25" PowerHigh="0.75"/>
            <SteadyState Duration="600" Power="0.88"/>
            <FreeRide Duration="120"/>
            <Cooldown Duration="300" PowerLow="0.7" PowerHigh="0.3"/>)");
        QCOMPARE(result.lines, (QStringList{"T:Test Ride", "2:300,0.25,0.75", "1:600,0.88", "0:120", "2:300,0.7,0.3"}));
        QVERIFY(result.warnings.isEmpty());
        verifyValid(result.lines);
    }

    void attributeNamesAreCaseInsensitive() {
        const ZwoConversion result = convert(
            R"(<steadystate duration="60" power="0.5"/><RAMP DURATION="60" POWERLOW="0.5" POWERHIGH="0.6"/>)");
        QCOMPARE(blocks(result), (QStringList{"1:60,0.5", "2:60,0.5,0.6"}));
    }

    void rampWithTheSameStartAndEndIsSteady() {
        QCOMPARE(blocks(convert(R"(<Ramp Duration="60" PowerLow="0.5" PowerHigh="0.5"/>)")), QStringList{"1:60,0.5"});
        // a SteadyState with a range is a ramp
        QCOMPARE(blocks(convert(R"(<SteadyState Duration="60" PowerLow="0.5" PowerHigh="0.7"/>)")),
                 QStringList{"2:60,0.5,0.7"});
    }

    void longBlocksAreSplit() {
        const ZwoConversion result = convert(R"(<SteadyState Duration="4000" Power="0.6"/><FreeRide Duration="1801"/>)");
        QCOMPARE(blocks(result), (QStringList{"1:1334,0.6", "1:1333,0.6", "1:1333,0.6", "0:901", "0:900"}));
        verifyValid(result.lines);
    }

    void longRampIsSplitIntoAContinuousRamp() {
        QCOMPARE(blocks(convert(R"(<Warmup Duration="3600" PowerLow="0.4" PowerHigh="0.8"/>)")),
                 (QStringList{"2:1800,0.4,0.6", "2:1800,0.6,0.8"}));
    }

    void intervals() {
        const ZwoConversion result =
            convert(R"(<IntervalsT Repeat="4" OnDuration="30" OffDuration="60" OnPower="1.2" OffPower="0.5"/>)");
        QCOMPARE(blocks(result), QStringList{"3:4;1:30,1.2;1:60,0.5"});
        verifyValid(result.lines);
    }

    void intervalsAlternativePowerNamesAndNoRest() {
        QCOMPARE(blocks(convert(
                     R"(<IntervalsT Repeat="3" OnDuration="20" OffDuration="0" PowerOnHigh="1.5" PowerOffLow="0.4"/>)")),
                 QStringList{"1:60,1.5"});
    }

    void intervalsWithBlocksTooLongAreWrittenOut() {
        const ZwoConversion result =
            convert(R"(<IntervalsT Repeat="2" OnDuration="2000" OffDuration="60" OnPower="0.8" OffPower="0.5"/>)");
        QCOMPARE(blocks(result), (QStringList{"1:1000,0.8", "1:1000,0.8", "1:60,0.5", "1:1000,0.8", "1:1000,0.8", "1:60,0.5"}));
        verifyValid(result.lines);
    }

    void intervalMessagesInTheFirstRepStayOnTheInterval() {
        const ZwoConversion result = convert(
            R"(<IntervalsT Repeat="3" OnDuration="30" OffDuration="30" OnPower="1.2" OffPower="0.5">)"
            R"(<textevent timeoffset="5" message="Go | hard; now"/>)"
            R"(<textevent timeoffset="40" message="Rest" duration="5"/></IntervalsT>)");
        QCOMPARE(blocks(result), QStringList{"3:3;1:30,1.2|Go / hard, now|10|5;1:30,0.5|Rest|5|10"});
        verifyValid(result.lines);
    }

    void intervalMessagesLaterOnAreWrittenRepByRep() {
        const ZwoConversion result = convert(
            R"(<IntervalsT Repeat="2" OnDuration="30" OffDuration="30" OnPower="1.2" OffPower="0.5">)"
            R"(<textevent timeoffset="70" message="Second rep"/></IntervalsT>)");
        QCOMPARE(blocks(result), (QStringList{"1:30,1.2", "1:30,0.5", "1:30,1.2|Second rep|10|10", "1:30,0.5"}));
        verifyValid(result.lines);
    }

    void textEventsOnBlocks() {
        const ZwoConversion result = convert(
            R"(<SteadyState Duration="60" Power="0.6"><textevent timeoffset="55" message="End soon"/>)"
            R"(<textevent timeoffset="90" message="Past the end"/><textevent timeoffset="0" message="  "/>)"
            R"(</SteadyState>)");
        // a message lasts to the end of its block at most; one past the end or with no text is left out
        QCOMPARE(blocks(result), QStringList{"1:60,0.6|End soon|5|55"});
        verifyValid(result.lines);
    }

    void maxEffortAndCadence() {
        const ZwoConversion result = convert(
            R"(<MaxEffort Duration="20"/><SteadyState Duration="60" Power="0.6" Cadence="105"/>)"
            R"(<SteadyState Duration="60" Power="0.6" Cadence="60"/>)"
            R"(<SteadyState Duration="60" Power="0.6" Cadence="85"/>)");
        QCOMPARE(blocks(result), (QStringList{"0:20|Max effort!|10|0", "1:60,0.6,1|105 RPM|60|0",
                                              "1:60,0.6,2|60 RPM|60|0", "1:60,0.6|85 RPM|60|0"}));
        verifyValid(result.lines);
    }

    void intervalCadences() {
        QCOMPARE(blocks(convert(R"(<IntervalsT Repeat="2" OnDuration="30" OffDuration="30" OnPower="1.2" )"
                                R"(OffPower="0.5" Cadence="110" CadenceResting="65"/>)")),
                 QStringList{"3:2;1:30,1.2,1|110 RPM|30|0;1:30,0.5,2|65 RPM|30|0"});
    }

    void ftpOverrideWritesWatts() {
        const ZwoConversion result =
            convert(R"(<SteadyState Duration="60" Power="0.8"/><Ramp Duration="60" PowerLow="0.5" PowerHigh="1.0"/>)",
                    "<name>W</name><ftpOverride>250</ftpOverride>");
        QCOMPARE(blocks(result), (QStringList{"1:60,200W", "2:60,125W,250W"}));
        QVERIFY(anyContains(result.warnings, QStringLiteral("250")));
        verifyValid(result.lines);
    }

    void outOfRangePowersAreClamped() {
        ZwoConversion result = convert(R"(<SteadyState Duration="60" Power="5"/><SteadyState Duration="60" Power="0.01"/>)");
        QCOMPARE(blocks(result), (QStringList{"1:60,3", "1:60,0.05"}));
        QVERIFY(anyContains(result.warnings, QStringLiteral("limited")));
        result = convert(R"(<SteadyState Duration="60" Power="10"/>)", "<ftpOverride>300</ftpOverride>");
        QCOMPARE(result.lines, QStringList{"1:60,2000W"});
    }

    void unsupportedBlocks() {
        const ZwoConversion result =
            convert(R"(<SteadyState Duration="60" Power="0.6"/><SolidState Duration="30"/><Mystery/>)");
        QCOMPARE(blocks(result), (QStringList{"1:60,0.6", "0:30|SolidState|10|0"}));
        QCOMPARE(result.warnings.size(), 2);
    }

    void otherSportAndZeroLengthBlocks() {
        const ZwoConversion result = convertZwoData(R"(<workout_file><sportType>run</sportType><workout>)"
                                                    R"(<SteadyState Duration="0" Power="1"/>)"
                                                    R"(<SteadyState Duration="60" Power="1"/></workout></workout_file>)");
        QVERIFY(result.title.isEmpty());
        QCOMPARE(result.lines, QStringList{"1:60,1"});
        QVERIFY(anyContains(result.warnings, QStringLiteral("run")));
    }

    void strayClosingTagsAreRepaired() {
        const ZwoConversion result =
            convert(R"(<SteadyState Duration="60" Power="0.6"></SteadyState></SteadyState><FreeRide Duration="30"/>)");
        QCOMPARE(blocks(result), (QStringList{"1:60,0.6", "0:30"}));
        QVERIFY(anyContains(result.warnings, QStringLiteral("closing tag")));
    }

    void errors_data() {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QString>("error");
        QTest::newRow("not xml") << QByteArray("not xml at all") << "Not a readable workout file";
        QTest::newRow("unclosed") << QByteArray("<workout_file><workout>") << "Not a readable workout file";
        QTest::newRow("html") << QByteArray("<html><body/></html>") << "no <workout_file>";
        QTest::newRow("no workout") << QByteArray("<workout_file><name>x</name></workout_file>") << "No <workout>";
        QTest::newRow("override") << QByteArray("<workout_file><ftpOverride>abc</ftpOverride><workout>"
                                                "<FreeRide Duration='60'/></workout></workout_file>")
                                  << "ftpOverride";
        QTest::newRow("no power") << QByteArray("<workout_file><workout><SteadyState Duration='60'/></workout></workout_file>")
                                  << "without a Power";
        QTest::newRow("not a number") << QByteArray("<workout_file><workout><SteadyState Duration='x' Power='1'/>"
                                                    "</workout></workout_file>")
                                      << "isn't a number";
        QTest::newRow("infinite") << QByteArray("<workout_file><workout><SteadyState Duration='inf' Power='1'/>"
                                                "</workout></workout_file>")
                                  << "isn't a number";
        QTest::newRow("ramp") << QByteArray("<workout_file><workout><Ramp Duration='60' PowerLow='0.5'/></workout>"
                                            "</workout_file>")
                              << "PowerHigh";
        QTest::newRow("interval") << QByteArray("<workout_file><workout><IntervalsT Repeat='2' OnDuration='30'/>"
                                                "</workout></workout_file>")
                                  << "OnPower";
        QTest::newRow("no blocks") << QByteArray("<workout_file><workout></workout></workout_file>") << "no blocks";
    }

    void errors() {
        QFETCH(QByteArray, text);
        QFETCH(QString, error);
        const ZwoConversion result = convertZwoData(text);
        QVERIFY(!result.ok());
        QVERIFY2(result.error.contains(error), qPrintable(result.error));
    }

    void missingFile() {
        QVERIFY(convertZwo(QStringLiteral("nowhere.zwo")).error.startsWith(QStringLiteral("Can't open")));
    }

    void writeAwoFile() {
        awotest::writeFile(QStringLiteral("ride.zwo"), zwo(R"(<SteadyState Duration="60" Power="0.6"/>)"));
        QString path;
        const ZwoConversion result = writeAwo(QStringLiteral("ride.zwo"), {}, &path);
        QVERIFY(result.ok());
        QVERIFY(result.warnings.isEmpty());
        QCOMPARE(path, QStringLiteral("ride.awo"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("T:Test Ride\n1:60,0.6\n"));
    }

    void awoPaths() {
        QCOMPARE(awoPathFor(QStringLiteral("C:/w/ride.zwo")), QStringLiteral("C:/w/ride.awo"));
        QCOMPARE(awoPathFor(QStringLiteral("C:/w/ride.ZWO")), QStringLiteral("C:/w/ride.awo"));
        QCOMPARE(awoPathFor(QStringLiteral("C:/w.x/ride")), QStringLiteral("C:/w.x/ride.awo"));
    }

    // ---- The same as the Python app ----

    void golden() {
        for (const QJsonValue value : awotest::golden(QStringLiteral("zwo.json")).toArray()) {
            const QJsonObject c = value.toObject();
            const ZwoConversion result = convertZwoData(c[QStringLiteral("xml")].toString().toUtf8());
            const QString context = c[QStringLiteral("xml")].toString().left(80);
            if (!c[QStringLiteral("error")].isNull()) {
                const QString expected = c[QStringLiteral("error")].toString();
                QVERIFY2(!result.ok(), qPrintable(context));
                // (the XML reader's own words differ: what's before them must match)
                const QString common = expected.section(QStringLiteral(" ("), 0, 0);
                QVERIFY2(result.error.startsWith(common), qPrintable(result.error + u'/' + expected));
                continue;
            }
            QVERIFY2(result.ok(), qPrintable(context + u' ' + result.error));
            QStringList lines, warnings;
            for (const QJsonValue line : c[QStringLiteral("lines")].toArray()) {
                lines << line.toString();
            }
            for (const QJsonValue warning : c[QStringLiteral("warnings")].toArray()) {
                warnings << warning.toString();
            }
            QCOMPARE(result.lines, lines);
            QCOMPARE(result.title, c[QStringLiteral("title")].toString());
            QCOMPARE(result.warnings, warnings);
        }
    }

private:
    std::unique_ptr<awotest::TempWorkingDir> dir_;
};

QTEST_GUILESS_MAIN(TestZwo)
#include "tst_zwo.moc"

#pragma once

// The Python workout screen's own rides (golden/hud.json, make_golden.py's hud_golden): the workout or FTP test each
// was of, set up as the golden ride set it up, and what one ridden here has to match of it. tst_workout_engine rides
// them on the engine alone, tst_hud on the whole workout screen.

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QtTest>

#include "AppState.hpp"
#include "AwoParser.hpp"
#include "CadenceWorkout.hpp"
#include "FtpColors.hpp"
#include "FtpTest.hpp"
#include "TestSupport.hpp"
#include "WorkoutEngine.hpp"

namespace awotest {

// the workout (or FTP test) the ride is of, into AppState as the setup screen builds it
inline bool buildRideWorkout(const QJsonObject& ride) {
    using namespace awo;
    AppState& state = appState();
    AwoOptions options;
    options.ftp = state.ftp;
    const QString test = ride.value(QStringLiteral("ftpTest")).toString();
    if (ride.value(QStringLiteral("music")).isArray()) {
        // Musical Cadence Mode: music.json's click tracks, as the setup screen builds the ride from them
        QHash<QString, awo::music::Analysis> byName;
        for (const QJsonValue& track : golden(QStringLiteral("music.json"))[QStringLiteral("tracks")].toArray()) {
            byName.insert(track[QStringLiteral("name")].toString(),
                          awo::music::Analysis::fromJson(track[QStringLiteral("analysis")].toObject()));
        }
        QList<std::pair<QString, std::optional<QString>>> songs;
        QHash<QString, awo::music::Analysis> analyses;
        for (const QJsonValue& song : ride.value(QStringLiteral("music")).toArray()) {
            const QString name = song[0].toString();
            const QString path = QStringLiteral("C:\\music\\") + name + QStringLiteral(".wav");
            songs.append({path, song[1].isNull() ? std::nullopt : std::optional<QString>(song[1].toString())});
            analyses.insert(path, byName.value(name));
        }
        awo::music::applyRide(awo::music::buildWorkout(songs, analyses, ride[QStringLiteral("preferred")].toDouble(),
                                                       ride[QStringLiteral("minSegment")].toDouble()));
        state.ftpRanges = ftpcolors::cadenceRanges(ftpcolors::kCadenceScaleBottomRpm);
        return true;
    }
    AwoResult result;
    if (!test.isEmpty()) {
        const ftptest::Test* ftpTest = ftptest::find(test);
        options.maxBlockSeconds = std::max(kMaxBlockSeconds, ftpTest->effortSeconds);
        result = parseAwoLines(ftptest::workoutLines(test, state.ftp != 0), options);
    } else {
        QStringList lines = ride.value(QStringLiteral("workout")).toString().split(u'\n');
        lines.removeLast();
        result = parseAwoLines(lines, options);
    }
    if (!result.ok()) {
        return false;
    }
    state.workoutList = result.workout.entries;
    state.workoutMessages = result.workout.messages;
    state.workoutTitle = result.workout.title;
    state.workoutDurationSeconds = result.workout.seconds;
    if (!test.isEmpty()) {
        int effortStart = 0;
        if (ftptest::isRamp(test)) {
            effortStart = state.workoutDurationSeconds;
            const WorkoutEntry first = ftptest::rampBlock(test, 0);
            state.workoutList.append(first);
            state.workoutDurationSeconds += first.block.seconds;
        } else {
            effortStart = state.workoutDurationSeconds - ftptest::find(test)->effortSeconds;
        }
        state.workoutMessages += ftptest::messages(test, effortStart, state.ftp);
    }
    return true;
}

// the rider and the ride's workout, as the golden ride set them
inline bool setUpRide(const QJsonObject& ride) {
    awo::AppState& state = awo::appState();
    state.ftp = ride[QStringLiteral("ftp")].toInt();
    state.maxHr = 185;
    state.wheelCircumference = 2.105;
    state.workoutMode = true;
    state.cadenceMode = ride.value(QStringLiteral("music")).isArray();
    state.ftpTestMode = ride[QStringLiteral("ftpTest")].toString();
    state.antHRM = state.antSpeedCadence = true;
    if (!buildRideWorkout(ride)) {
        return false;
    }
    state.workoutFilename = state.workoutFilenameShort = QStringLiteral("golden.awo");
    return true;
}

// the ride's difficulty changes and pauses by the tick they come before: [tick, "difficulty", value] or
// [tick, "pause"]
inline QMap<int, QList<QJsonArray>> rideEvents(const QJsonObject& ride) {
    QMap<int, QList<QJsonArray>> events;
    for (const QJsonValue& event : ride[QStringLiteral("events")].toArray()) {
        events[event.toArray()[0].toInt()].append(event.toArray());
    }
    return events;
}

inline void addRideRows() {
    QTest::addColumn<QJsonObject>("golden");
    for (const QJsonValue& value : golden(QStringLiteral("hud.json")).toArray()) {
        const QJsonObject ride = value.toObject();
        QTest::newRow(qPrintable(ride[QStringLiteral("ride")].toObject()[QStringLiteral("name")].toString())) << ride;
    }
}

inline QString numberDiff(const QString& what, double ours, double python) {
    return QStringLiteral("%1: %2, Python %3").arg(what).arg(ours, 0, 'g', 17).arg(python, 0, 'g', 17);
}

// the ride's power requests, grades, FTP test result, where it ended up and (songs) the songs heard the same as the
// Python one's (a failure returns early: the caller's test has failed)
inline void compareRide(const QJsonObject& golden, int ticks, const awo::WorkoutEngine& engine, bool songs = true) {
    using namespace awo;
    const AppState& state = appState();
    QCOMPARE(ticks, golden[QStringLiteral("ticks")].toInt());
    const QJsonArray seconds = golden[QStringLiteral("requestSeconds")].toArray();
    const QJsonArray watts = golden[QStringLiteral("requestWatts")].toArray();
    QCOMPARE(state.workoutPowerRequestsWatts.size(), watts.size());
    for (qsizetype i = 0; i < watts.size(); ++i) {
        QVERIFY2(state.workoutPowerRequestsWatts[i] == watts[i].toDouble(),
                 qPrintable(numberDiff(QStringLiteral("request %1 watts").arg(i), state.workoutPowerRequestsWatts[i],
                                       watts[i].toDouble())));
        QVERIFY2(state.workoutPowerRequestsSeconds[i] == seconds[i].toDouble(),
                 qPrintable(numberDiff(QStringLiteral("request %1 time").arg(i), state.workoutPowerRequestsSeconds[i],
                                       seconds[i].toDouble())));
    }

    const QJsonObject grades = golden[QStringLiteral("grades")].toObject();
    const QJsonArray order = golden[QStringLiteral("gradeOrder")].toArray();
    QCOMPARE(state.blockGrades.items().size(), order.size());
    for (qsizetype i = 0; i < order.size(); ++i) {
        const auto& [key, grade] = state.blockGrades.items()[i];
        const QString name = QStringLiteral("%1,%2,%3").arg(key.index).arg(key.sub).arg(key.rep);
        QCOMPARE(name, order[i].toString());
        const QJsonObject python = grades[name].toObject();
        QCOMPARE(grade.label, python[QStringLiteral("label")].toString());
        QCOMPARE(grade.free, python[QStringLiteral("free")].toBool());
        QCOMPARE(grade.endedEarly, python[QStringLiteral("endedEarly")].toBool());
        QCOMPARE(grade.top, python[QStringLiteral("top")].toInt());
        const QList<std::pair<QString, std::optional<double>>> numbers = {
            {"pct", grade.pct},         {"graded", grade.graded},     {"dropped", grade.dropped},
            {"grace", grade.grace},     {"duration", grade.duration}, {"ridden", grade.ridden},
            {"pedaling", grade.pedaling}, {"avg", grade.avg},         {"low", grade.low},
            {"high", grade.high},       {"overAvg", grade.overAvg},   {"surge", grade.surge},
            {"penalty", grade.penalty}, {"bonus", grade.bonus},       {"intensity", grade.intensity},
            {"start", grade.start},     {"end", grade.end}};
        for (const auto& [field, value] : numbers) {
            QVERIFY2(sameNumber(python[field], value),
                     qPrintable(QStringLiteral("%1 %2: %3, Python %4")
                                    .arg(name, field, value ? QString::number(*value, 'g', 17) : QStringLiteral("none"),
                                         QString::number(python[field].toDouble(), 'g', 17))));
        }
    }

    const QJsonValue result = golden[QStringLiteral("ftpTestResult")];
    if (result.isNull()) {
        QVERIFY(!state.ftpTestResult);
    } else {
        QVERIFY(state.ftpTestResult.has_value());
        QVERIFY(sameNumber(result[QStringLiteral("average")], state.ftpTestResult->average));
        QCOMPARE(state.ftpTestResult->completed, result[QStringLiteral("completed")].toBool());
    }
    QCOMPARE(state.workoutDurationSeconds, golden[QStringLiteral("workoutSeconds")].toInt());
    QCOMPARE(state.secondsPassed, golden[QStringLiteral("secondsPassed")].toDouble());
    QCOMPARE(engine.workoutSecondsPassed(), golden[QStringLiteral("workoutSecondsPassed")].toDouble());
    QCOMPARE(engine.activeWorkout(), golden[QStringLiteral("activeWorkout")].toInt());
    QCOMPARE(engine.completeShown(), golden[QStringLiteral("completed")].toBool());
    // Musical Cadence Mode: the songs heard, and when (the workout screen's record, not the engine's)
    if (!songs) {
        return;
    }
    const QJsonArray played = golden[QStringLiteral("musicPlayed")].toArray();
    QCOMPARE(state.musicPlayed.size(), played.size());
    for (qsizetype i = 0; i < played.size(); ++i) {
        const QJsonArray song = played[i].toArray();
        QCOMPARE(state.musicPlayed[i].track, song[0].toInt());
        QCOMPARE(state.musicPlayed[i].start, song[1].toDouble());
        QCOMPARE(state.musicPlayed[i].end, song[2].toDouble());
    }
}

}  // namespace awotest

#pragma once

// What the tests share: an empty working folder per test (the settings, log and caches the app writes beside itself
// never touch real ones), the golden files written by golden\make_golden.py, and workouts in the shape the Python
// app kept them (its tuples, as JSON arrays) to compare with them.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <memory>
#include <optional>

#include "WorkoutTypes.hpp"

namespace awotest {

class TempWorkingDir {
public:
    TempWorkingDir() : previous_(QDir::currentPath()) { QDir::setCurrent(dir_.path()); }
    ~TempWorkingDir() { QDir::setCurrent(previous_); }
    QString path() const { return dir_.path(); }
    QString file(const QString& name) const { return dir_.filePath(name); }

private:
    QTemporaryDir dir_;
    QString previous_;
};

inline QString pythonAppDir() {
    return QStringLiteral(AWO_PYTHON_APP_DIR);
}

inline QJsonValue golden(const QString& name) {
    QFile file(QStringLiteral(AWO_GOLDEN_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("No golden file %s (run tests\\golden\\make_golden.py)", qPrintable(name));
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array());
}

inline void writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}

inline QJsonArray targets(const awo::Targets& values) {
    QJsonArray array;
    for (int value : values) {
        array.append(value);
    }
    return array;
}

// A block as the Python app's tuple: (0, seconds, position), (1, seconds, (21 targets), position) or
// (2, seconds, ((start, end) x 21), position)
inline QJsonArray toPython(const awo::Block& block) {
    switch (block.type) {
    case awo::BlockType::FreeRide:
        return {0, block.seconds, block.position};
    case awo::BlockType::Steady:
        return {1, block.seconds, targets(block.power), block.position};
    case awo::BlockType::Ramp: {
        QJsonArray pairs;
        for (int d = 0; d < awo::kDifficulties; ++d) {
            pairs.append(QJsonArray{block.power[d], block.powerEnd[d]});
        }
        return {2, block.seconds, pairs, block.position};
    }
    }
    return {};
}

// (3, repeats, block, block[, block]), (4, seconds, (21 targets), ramp seconds), (5, repeats left, index, block)
inline QJsonArray toPython(const awo::WorkoutEntry& entry) {
    switch (entry.type) {
    case awo::WorkoutEntry::Type::Block:
        return toPython(entry.block);
    case awo::WorkoutEntry::Type::Interval: {
        QJsonArray array{3, entry.repeats};
        for (const awo::Block& block : entry.intervalBlocks) {
            array.append(toPython(block));
        }
        return array;
    }
    case awo::WorkoutEntry::Type::RampStep:
        return {4, entry.stepSeconds, targets(entry.stepPower), entry.rampSeconds};
    case awo::WorkoutEntry::Type::IntervalBlock:
        return {5, entry.repeats, entry.subIndex, toPython(entry.block)};
    }
    return {};
}

inline QJsonArray toPython(const QList<awo::WorkoutEntry>& entries) {
    QJsonArray array;
    for (const awo::WorkoutEntry& entry : entries) {
        array.append(toPython(entry));
    }
    return array;
}

inline QJsonArray toPython(const QList<awo::Message>& messages) {
    QJsonArray array;
    for (const awo::Message& message : messages) {
        array.append(QJsonArray{message.start, message.end, message.text});
    }
    return array;
}

// Python's value (a number, or null for None) against ours, to a relative 1e-9
inline bool sameNumber(const QJsonValue& python, std::optional<double> ours, double tolerance = 1e-9) {
    if (python.isNull() || python.isUndefined()) {
        return !ours.has_value();
    }
    if (!ours) {
        return false;
    }
    const double expected = python.toDouble();
    return std::abs(expected - *ours) <= tolerance * std::max(1.0, std::abs(expected));
}

inline std::optional<double> optionalNumber(const QJsonValue& value) {
    return value.isNull() || value.isUndefined() ? std::nullopt : std::optional<double>(value.toDouble());
}

}  // namespace awotest

// Compares, failing the test with both values shown
#define AWO_COMPARE_JSON(actual, expected)                                                                     \
    do {                                                                                                       \
        const QJsonValue awoActual_(actual);                                                                   \
        const QJsonValue awoExpected_(expected);                                                               \
        if (awoActual_ != awoExpected_) {                                                                      \
            QFAIL(qPrintable(QStringLiteral("JSON differs\n  actual:   %1\n  expected: %2")                    \
                                 .arg(QString::fromUtf8(QJsonDocument(QJsonArray{awoActual_}).toJson(          \
                                          QJsonDocument::Compact)),                                            \
                                      QString::fromUtf8(QJsonDocument(QJsonArray{awoExpected_}).toJson(        \
                                          QJsonDocument::Compact)))));                                         \
        }                                                                                                      \
    } while (false)

#include "WorkoutHelpers.hpp"

#include <QRegularExpression>

#include <cmath>

#include "PyCompat.hpp"

namespace awo {

const QStringList kPositionNames = {
    QStringLiteral("Seated"),     QStringLiteral("Seated High Cadence"), QStringLiteral("Seated Low Cadence"),
    QStringLiteral("Standing"),   QStringLiteral("Single Leg"),          QStringLiteral("Switch Single Leg"),
};

const QString kSongHeaderCss = QStringLiteral(
    "background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1); color: #fffffe;");

QString minimizedDuration(double seconds) {
    seconds = std::max(0.0, seconds);
    // (whole seconds shown, as Python's time.gmtime() takes them; which parts show goes by the exact time, so
    // 3600.5 is "1h 0m 0s")
    const long long whole = static_cast<long long>(std::floor(seconds));
    const long long h = whole / 3600;
    const long long m = whole % 3600 / 60;
    const long long s = whole % 60;
    if (seconds < 60) {
        return QStringLiteral("%1s").arg(s);
    }
    if (seconds < 3600) {
        if (py::mod(seconds, 60) == 0) {
            return QStringLiteral("%1m").arg(m);
        }
        return QStringLiteral("%1m %2s").arg(m).arg(s);
    }
    if (py::mod(seconds, 3600) == 0) {
        return QStringLiteral("%1h").arg(h);
    }
    if (py::mod(seconds, 60) == 0) {
        return QStringLiteral("%1h %2m").arg(h).arg(m);
    }
    return QStringLiteral("%1h %2m %3s").arg(h).arg(m).arg(s);
}

QString blockText(const Block& block, int difficulty, const QString& unit) {
    const QString position = kPositionNames.value(block.position);
    switch (block.type) {
    case BlockType::FreeRide:
        return position + u' ' + minimizedDuration(block.seconds);
    case BlockType::Steady:
        return QStringLiteral("%1 %2 %3 %4").arg(position).arg(block.power[difficulty]).arg(unit,
                                                                                            minimizedDuration(block.seconds));
    case BlockType::Ramp:
        return QStringLiteral("%1 %2-%3 %4 %5")
            .arg(position)
            .arg(block.power[difficulty])
            .arg(block.powerEnd[difficulty])
            .arg(unit, minimizedDuration(block.seconds));
    }
    return {};
}

QStringList blockText(const WorkoutEntry& entry, int difficulty, const QString& unit) {
    if (entry.type == WorkoutEntry::Type::Block) {
        return {blockText(entry.block, difficulty, unit)};
    }
    if (entry.type == WorkoutEntry::Type::Interval) {
        QStringList lines{QStringLiteral("%1x").arg(entry.repeats)};
        for (const Block& block : entry.intervalBlocks) {
            lines << blockText(block, difficulty, unit);
        }
        return lines;
    }
    return {};
}

QString workoutText(const QList<WorkoutEntry>& entries, const QString& unit, const QMap<int, QString>& songStarts) {
    QString text;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const auto song = songStarts.constFind(static_cast<int>(index));
        if (song != songStarts.constEnd()) {
            text += *song + u'\n';
        }
        const WorkoutEntry& entry = entries[index];
        if (entry.type == WorkoutEntry::Type::Block) {
            text += blockText(entry.block, kBaseDifficulty, unit) + u'\n';
        } else if (entry.type == WorkoutEntry::Type::Interval) {
            const QStringList lines = blockText(entry, kBaseDifficulty, unit);
            text += lines.first() + u' ' + lines.mid(1).join(QStringLiteral("\n      ")) + u'\n';
        }
    }
    return text;
}

std::optional<std::pair<int, int>> targetsFromText(const QString& text) {
    const QStringList words = text.split(u' ');
    for (const QString unit : {QStringLiteral("Watts"), QStringLiteral("RPM")}) {
        const qsizetype index = words.indexOf(unit);
        if (index < 0) {
            continue;
        }
        if (index == 0) {
            return std::nullopt;
        }
        const QStringList values = words[index - 1].split(u'-');
        const std::optional<long long> low = py::toInt(values[0]);
        const std::optional<long long> high = values.size() > 1 ? py::toInt(values[1]) : std::optional<long long>(0);
        if (!low || !high) {
            return std::nullopt;
        }
        return std::pair(static_cast<int>(*low), static_cast<int>(*high));
    }
    return std::nullopt;
}

bool isSongHeader(const QString& text, bool cadenceMode) {
    return cadenceMode && text.contains(QStringLiteral(" : "));
}

std::pair<QList<double>, QList<double>> withDropoutsAsZero(const QList<double>& times, const QList<double>& values,
                                                           std::optional<double> endTime) {
    if (times.isEmpty()) {
        return {};
    }
    QList<double> outTimes{times[0]};
    QList<double> outValues{values[0]};
    for (qsizetype i = 1; i < times.size(); ++i) {
        if (times[i] - times[i - 1] > kDropoutZeroSeconds) {
            outTimes << times[i - 1] + kDropoutEdgeSeconds << times[i] - kDropoutEdgeSeconds;
            outValues << 0 << 0;
        }
        outTimes << times[i];
        outValues << values[i];
    }
    if (endTime && *endTime - times.last() > kDropoutZeroSeconds) {
        outTimes << times.last() + kDropoutEdgeSeconds << *endTime;
        outValues << 0 << 0;
    }
    return {outTimes, outValues};
}

namespace selectedDevices {

const QStringList kSelectableTypes = {QStringLiteral("11"),  QStringLiteral("17"),  QStringLiteral("120"),
                                      QStringLiteral("121"), QStringLiteral("122"), QStringLiteral("123")};

namespace {
QStringList& lines() {
    static QStringList selected;
    return selected;
}
}  // namespace

QStringList read() {
    return lines();
}

void write(const QStringList& rawLines) {
    QStringList clean;
    for (const QString& raw : rawLines) {
        static const QRegularExpression whitespace(QStringLiteral("\\s+"));
        const QStringList parts = raw.split(whitespace, Qt::SkipEmptyParts);
        if (parts.size() != 2 || !kSelectableTypes.contains(parts[0]) || !py::isDigits(parts[1])) {
            continue;
        }
        // ANT+ device numbers are 16 bit, 0 is the wildcard
        bool ok = false;
        const qulonglong number = parts[1].toULongLong(&ok);
        if (!ok || number == 0 || number > 65535) {
            continue;
        }
        const QString line = parts[0] + u' ' + QString::number(number);
        if (!clean.contains(line)) {
            clean << line;
        }
    }
    lines() = clean;
}

void clear() {
    lines().clear();
}

}  // namespace selectedDevices

}  // namespace awo

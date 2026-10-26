#include "WorkoutGrading.hpp"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace awo::grading {

const QString kNoGrade = QStringLiteral("--");
const QString kNoGradeColor = QStringLiteral("#9fc5d1");

namespace {

// (lowest %, letter), best first; over 100% is an S
const QList<std::pair<double, QString>>& gradeBands() {
    static const QList<std::pair<double, QString>> bands = {
        {97, QStringLiteral("A+")}, {93, QStringLiteral("A")}, {90, QStringLiteral("A-")}, {87, QStringLiteral("B+")},
        {83, QStringLiteral("B")},  {80, QStringLiteral("B-")}, {77, QStringLiteral("C+")}, {73, QStringLiteral("C")},
        {70, QStringLiteral("C-")}, {67, QStringLiteral("D+")}, {63, QStringLiteral("D")}, {60, QStringLiteral("D-")},
    };
    return bands;
}

double clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

}  // namespace

QString letter(std::optional<double> pct) {
    if (!pct) {
        return kNoGrade;
    }
    if (*pct > 100) {
        return QStringLiteral("S");
    }
    for (const auto& [lowest, grade] : gradeBands()) {
        if (*pct >= lowest) {
            return grade;
        }
    }
    return QStringLiteral("F");
}

QString gradeColor(const QString& grade) {
    static const QHash<QChar, QString> colors = {
        {u'S', QStringLiteral("#f5c400")}, {u'A', QStringLiteral("#2f9e47")}, {u'B', QStringLiteral("#7fb52c")},
        {u'C', QStringLiteral("#e0c33a")}, {u'D', QStringLiteral("#e3801a")}, {u'F', QStringLiteral("#d63e34")},
    };
    return grade.isEmpty() ? kNoGradeColor : colors.value(grade[0], kNoGradeColor);
}

double startGrace(double duration, double step) {
    return std::min({kGraceBaseSeconds + std::abs(step) / kGraceWattsPerSecond, kGraceMaxSeconds,
                     kGraceMaxFraction * duration});
}

double endGrace(double duration) {
    return std::min(kEndGraceSeconds, kEndGraceMaxFraction * duration);
}

BlockGrader::BlockGrader(double duration, double previousTarget, int ftp, bool freeRide)
    : duration_(duration), previousTarget_(previousTarget), ftp_(std::max(1, ftp)), freeRide_(freeRide),
      endGrace_(endGrace(duration)) {}

void BlockGrader::resume(double blockSeconds, double target) {
    if (!freeRide_) {
        resumeGraceUntil_ = blockSeconds + startGrace(duration_, target);
    }
}

void BlockGrader::add(double blockSeconds, double dt, double target, std::optional<double> actual) {
    if (dt <= 0) {
        return;
    }
    if (!firstTarget_) {
        firstTarget_ = target;
        startGrace_ = freeRide_ ? 0.0 : startGrace(duration_, target - previousTarget_);
    }
    if (blockSeconds < *startGrace_ || blockSeconds < resumeGraceUntil_ || blockSeconds > duration_ - endGrace_) {
        graceSeconds_ += dt;
        if (actual) {
            graceSamples_.append({dt, target, *actual});
        }
        return;
    }
    if (!actual) {
        droppedSeconds_ += dt;
        return;
    }
    samples_.append({dt, target, *actual});
}

Grade BlockGrader::finish(std::optional<double> endedAt) const {
    const QList<Sample>* samples = &samples_;
    double grace = graceSeconds_;
    if (samples_.isEmpty() && !graceSamples_.isEmpty()) {
        samples = &graceSamples_;
        grace = 0.0;
    }
    double graded = 0;
    for (const auto& [dt, target, actual] : *samples) {
        graded += dt;
    }
    const bool endedEarly = endedAt && *endedAt < duration_;
    const double ridden = endedEarly ? *endedAt : duration_;
    Grade result;
    result.free = freeRide_;
    result.graded = graded;
    result.dropped = droppedSeconds_;
    result.grace = grace;
    result.duration = duration_;
    result.ridden = ridden;
    result.endedEarly = endedEarly;
    if (graded <= 0) {
        return result;
    }
    if (freeRide_) {
        double pedaling = 0;
        for (const auto& [dt, target, actual] : *samples) {
            if (actual > kFreeRideMinWatts) {
                pedaling += dt;
            }
        }
        result.pedaling = 100 * pedaling / graded;
        result.pct = result.pedaling;
        return result;
    }
    double targetSum = 0;
    for (const auto& [dt, target, actual] : *samples) {
        targetSum += dt * target;
    }
    if (targetSum <= 0) {
        return result;
    }
    double actualSum = 0;
    for (const auto& [dt, target, actual] : *samples) {
        actualSum += dt * actual;
    }
    const double rAvg = actualSum / targetSum;
    const double intensity = targetSum / graded / ftp_;
    const double overTol = kOverTolerance + kOverToleranceExtra * clamp(intensity - 1, 0, 1);
    const double overSlope = intensity < kRecoveryIntensity ? kRecoveryOverSlope : kOverSlope;
    const double overAvg = overSlope * std::max(0.0, rAvg - 1 - overTol);
    const double bonus = kBonusMax * clamp(intensity - kBonusFromIntensity, 0, 1);
    std::optional<double> rLow;
    std::optional<double> rHigh;
    double surge = 0.0;
    double base = 0;
    if (ridden < kShortBlockSeconds) {
        base = std::min(rAvg, 1.0);
    } else {
        const QList<double> rolling = rollingRatios(*samples);
        rLow = *std::min_element(rolling.begin(), rolling.end());
        rHigh = percentile(rolling, kHighPercentile);
        const double surgeTol = kSurgeTolerance + kSurgeToleranceExtra * clamp(intensity - 1, 0, 1);
        surge = kSurgeSlope * std::max(0.0, *rHigh - 1 - surgeTol);
        base = (kLowWeight * std::min(*rLow, 1.0) + kAvgWeight * std::min(rAvg, 1.0)) / (kLowWeight + kAvgWeight);
    }
    const double penalty = std::min(overAvg + surge, kMaxOverPenalty);
    const double score = std::max(0.0, base - penalty + bonus);
    result.pct = 100 * score;
    result.avg = 100 * rAvg;
    if (rLow) {
        result.low = 100 * *rLow;
    }
    if (rHigh) {
        result.high = 100 * *rHigh;
    }
    result.overAvg = 100 * overAvg;
    result.surge = 100 * surge;
    result.penalty = 100 * penalty;
    result.bonus = 100 * bonus;
    result.intensity = intensity;
    return result;
}

QList<double> rollingRatios(const QList<Sample>& samples) {
    QList<double> ratios;
    double windowDt = 0;
    double windowTarget = 0;
    double windowActual = 0;
    qsizetype start = 0;
    for (const auto& [dt, target, actual] : samples) {
        windowDt += dt;
        windowTarget += dt * target;
        windowActual += dt * actual;
        while (windowDt - std::get<0>(samples[start]) >= kRollingSeconds) {
            const auto [oldDt, oldTarget, oldActual] = samples[start];
            windowDt -= oldDt;
            windowTarget -= oldDt * oldTarget;
            windowActual -= oldDt * oldActual;
            ++start;
        }
        if (windowDt >= kRollingSeconds - 0.15 && windowTarget > 0) {
            ratios.append(windowActual / windowTarget);
        }
    }
    if (ratios.isEmpty()) {
        double total = 0;
        double actualTotal = 0;
        for (const auto& [dt, target, actual] : samples) {
            total += dt * target;
        }
        for (const auto& [dt, target, actual] : samples) {
            actualTotal += dt * actual;
        }
        ratios.append(total != 0 ? actualTotal / total : 0.0);
    }
    return ratios;
}

double percentile(QList<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<qsizetype>(std::floor(fraction * static_cast<double>(values.size() - 1) + 0.5));
    return values[std::min(values.size() - 1, index)];
}

std::optional<double> latestPower(const QList<double>& times, const QList<double>& values, double now) {
    if (times.isEmpty() || now - times.last() > kDropoutSeconds) {
        return std::nullopt;
    }
    return values.last();
}

void Grades::set(const GradeKey& key, const Grade& grade) {
    for (auto& item : items_) {
        if (item.first == key) {
            item.second = grade;
            return;
        }
    }
    items_.append({key, grade});
}

const Grade* Grades::find(const GradeKey& key) const {
    for (const auto& item : items_) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

std::optional<double> riddenAverage(const QList<const Grade*>& grades) {
    double total = 0;
    double weighted = 0;
    for (const Grade* grade : grades) {
        if (grade->pct && grade->ridden > 0) {
            total += grade->ridden;
        }
    }
    if (total == 0) {
        return std::nullopt;
    }
    for (const Grade* grade : grades) {
        if (grade->pct && grade->ridden > 0) {
            weighted += *grade->pct * grade->ridden;
        }
    }
    return weighted / total;
}

std::optional<double> workoutAverage(const Grades& grades) {
    QList<const Grade*> all;
    for (const auto& item : grades.items()) {
        all.append(&item.second);
    }
    return riddenAverage(all);
}

QList<GradeRow> workoutRows(const QList<WorkoutEntry>& entries, const Grades& grades, const QMap<int, QString>& songs) {
    QList<GradeRow> rows;
    // the song row being filled (an index into rows), and each song row's grades
    qsizetype songRow = -1;
    QMap<qsizetype, QList<const Grade*>> songGrades;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const int index = static_cast<int>(i);
        if (songs.contains(index)) {
            songRow = rows.size();
            rows.append({GradeRow::Kind::Song, songs.value(index), std::nullopt, QString(), QString()});
            songGrades[songRow];
        }
        if (songRow >= 0) {
            for (const auto& [key, grade] : grades.items()) {
                if (key.index == index) {
                    songGrades[songRow].append(&grade);
                }
            }
        }
        const WorkoutEntry& entry = entries[i];
        if (entry.type == WorkoutEntry::Type::Block) {
            if (const Grade* grade = grades.find({index, 0, 0})) {
                rows.append({GradeRow::Kind::Block, grade->label, grade->pct, letter(grade->pct),
                             grade->endedEarly ? QStringLiteral("ended early") : QString(), songRow >= 0});
            }
        } else if (entry.type == WorkoutEntry::Type::Interval) {
            QList<GradeRow> subRows;
            for (int sub = 0; sub < entry.intervalBlocks.size(); ++sub) {
                QList<const Grade*> records;
                for (int rep = 0; rep < entry.repeats; ++rep) {
                    if (const Grade* grade = grades.find({index, sub, rep})) {
                        records.append(grade);
                    }
                }
                if (records.isEmpty()) {
                    continue;
                }
                double riddenSum = 0;
                double weighted = 0;
                bool scored = false;
                for (const Grade* record : records) {
                    if (record->pct) {
                        scored = true;
                        weighted += *record->pct * record->ridden;
                    }
                }
                for (const Grade* record : records) {
                    if (record->pct) {
                        riddenSum += record->ridden;
                    }
                }
                const std::optional<double> pct = scored ? std::optional<double>(weighted / riddenSum) : std::nullopt;
                QStringList letters;
                for (const Grade* record : records) {
                    letters << letter(record->pct);
                }
                QString note = letters.join(QStringLiteral(" · "));
                if (records.last()->endedEarly) {
                    note += QStringLiteral(" (ended)");
                }
                subRows.append({GradeRow::Kind::Sub, records.first()->label, pct, letter(pct), note});
            }
            if (!subRows.isEmpty()) {
                rows.append({GradeRow::Kind::Interval, QStringLiteral("%1x").arg(entry.repeats)});
                rows += subRows;
            }
        }
    }
    // (a song none of whose blocks were ridden has no row)
    for (qsizetype i = rows.size() - 1; i >= 0; --i) {
        if (rows[i].kind != GradeRow::Kind::Song) {
            continue;
        }
        const QList<const Grade*>& records = songGrades[i];
        if (records.isEmpty()) {
            rows.removeAt(i);
            continue;
        }
        rows[i].pct = riddenAverage(records);
        rows[i].grade = letter(rows[i].pct);
    }
    return rows;
}

}  // namespace awo::grading

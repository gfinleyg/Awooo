#include "PowerCalibration.hpp"

#include <QDateTime>
#include <QJsonObject>

#include <algorithm>

#include "PyCompat.hpp"
#include "SavedSettings.hpp"

namespace awo::calibration {

namespace {

const QString kSettingsKey = QStringLiteral("powerCalibration");

QJsonObject savedObject() {
    return settings::load().value(kSettingsKey).toObject();
}

}  // namespace

QString key(int deviceType, int deviceNumber) {
    return QStringLiteral("%1 %2").arg(deviceType).arg(deviceNumber);
}

QHash<QString, Calibration> loadAll() {
    QHash<QString, Calibration> all;
    const QJsonObject saved = savedObject();
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject values = it.value().toObject();
        Calibration calibration;
        calibration.offsetWatts = values.value(QStringLiteral("offset_watts")).toDouble(0);
        calibration.scale = values.value(QStringLiteral("scale")).toDouble(1);
        const QJsonValue zero = values.value(QStringLiteral("ant_zero_offset"));
        if (zero.isDouble()) {
            calibration.antZeroOffset = zero.toInt();
        }
        calibration.updated = values.value(QStringLiteral("updated")).toString();
        all.insert(it.key(), calibration);
    }
    return all;
}

std::optional<Calibration> get(int deviceType, int deviceNumber) {
    const QHash<QString, Calibration> all = loadAll();
    const auto found = all.constFind(key(deviceType, deviceNumber));
    if (found == all.constEnd()) {
        return std::nullopt;
    }
    return *found;
}

bool save(int deviceType, int deviceNumber, double offsetWatts, double scale, std::optional<int> antZeroOffset) {
    QJsonObject saved = savedObject();
    saved.insert(key(deviceType, deviceNumber),
                 QJsonObject{{QStringLiteral("offset_watts"), py::round(offsetWatts, 1)},
                             {QStringLiteral("scale"), py::round(scale, 4)},
                             {QStringLiteral("ant_zero_offset"),
                              antZeroOffset ? QJsonValue(*antZeroOffset) : QJsonValue(QJsonValue::Null)},
                             {QStringLiteral("updated"), QDateTime::currentDateTime().toString(Qt::ISODate)}});
    return settings::update({{kSettingsKey, saved}});
}

bool clear(int deviceType, int deviceNumber) {
    QJsonObject saved = savedObject();
    if (!saved.contains(key(deviceType, deviceNumber))) {
        return true;
    }
    saved.remove(key(deviceType, deviceNumber));
    return settings::update({{kSettingsKey, saved}});
}

int apply(int watts, const std::optional<Calibration>& calibration) {
    if (!calibration || watts <= 0) {
        return watts;
    }
    const double corrected = (watts - calibration->offsetWatts) * calibration->scale;
    return std::max(0, static_cast<int>(py::round(corrected)));
}

QString describe(const std::optional<Calibration>& calibration) {
    if (!calibration) {
        return QStringLiteral("No calibration saved");
    }
    QString text = QStringLiteral("Offset %1 W, scale x%2").arg(py::formatG(calibration->offsetWatts),
                                                               py::fixed(calibration->scale, 3));
    if (!calibration->updated.isEmpty()) {
        text += QStringLiteral(" (saved %1)").arg(QString(calibration->updated).replace(u'T', u' '));
    }
    return text;
}

}  // namespace awo::calibration

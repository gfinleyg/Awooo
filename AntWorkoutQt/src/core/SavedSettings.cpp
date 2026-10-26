#include "SavedSettings.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

#include "PyCompat.hpp"

namespace awo::settings {

const QString kSettingsFile = QStringLiteral("savedSettingsANT.json");
const QString kLegacyStatsFile = QStringLiteral("savedStatsANT.txt");
const QString kLegacyCalibrationFile = QStringLiteral("powerCalibrationANT.txt");

namespace {

// written to a temporary file and swapped in, so a crash mid-write can't leave a half written settings file
bool write(const QJsonObject& data) {
    QSaveFile file(kSettingsFile);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
    return file.commit();
}

void migrateLegacyFiles() {
    QJsonObject data;
    QFile stats(kLegacyStatsFile);
    if (stats.open(QIODevice::ReadOnly)) {
        const QStringList values = QString::fromUtf8(stats.readAll()).trimmed().split(u',');
        // (read in order until one isn't a number, as the Python app did: the ones before it are kept)
        if (values.size() == 3) {
            if (const std::optional<double> wheel = py::toFloat(values[0])) {
                data.insert(QStringLiteral("wheelCircumference"), *wheel);
                if (const std::optional<long long> ftp = py::toInt(values[1])) {
                    data.insert(QStringLiteral("ftp"), static_cast<qint64>(*ftp));
                    if (const std::optional<long long> maxHr = py::toInt(values[2])) {
                        data.insert(QStringLiteral("maxHr"), static_cast<qint64>(*maxHr));
                    }
                }
            }
        }
    }
    QFile calibration(kLegacyCalibrationFile);
    if (calibration.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(calibration.readAll());
        if (document.isObject() && !document.object().isEmpty()) {
            data.insert(QStringLiteral("powerCalibration"), document.object());
        }
    }
    stats.close();
    calibration.close();
    if (!data.isEmpty() && write(data)) {
        QFile::remove(kLegacyStatsFile);
        QFile::remove(kLegacyCalibrationFile);
    }
}

}  // namespace

QJsonObject load() {
    if (!QFile::exists(kSettingsFile)) {
        migrateLegacyFiles();
    }
    QFile file(kSettingsFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

bool update(const QJsonObject& changes) {
    QJsonObject data = load();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        data.insert(it.key(), it.value());
    }
    return write(data);
}

}  // namespace awo::settings

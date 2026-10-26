#include "Units.hpp"

#include <QJsonObject>
#include <QStringList>

#include "PyCompat.hpp"
#include "SavedSettings.hpp"

namespace awo {

namespace {
const QStringList kWeightUnits = {QStringLiteral("lbs"), QStringLiteral("kg")};
const QStringList kPowerWeightUnits = {QStringLiteral("W/kg"), QStringLiteral("W/lb")};
const QStringList kDistanceUnits = {QStringLiteral("mi"), QStringLiteral("km")};
}  // namespace

Units& Units::current() {
    static Units units;
    return units;
}

void Units::load() {
    const QJsonObject saved = settings::load();
    const QString weightUnit = saved.value(QStringLiteral("weightUnit")).toString();
    if (kWeightUnits.contains(weightUnit)) {
        weight = weightUnit;
    }
    const QString powerWeightUnit = saved.value(QStringLiteral("powerWeightUnit")).toString();
    if (kPowerWeightUnits.contains(powerWeightUnit)) {
        powerWeight = powerWeightUnit;
    }
    const QString distanceUnit = saved.value(QStringLiteral("distanceUnit")).toString();
    if (kDistanceUnits.contains(distanceUnit)) {
        distance = distanceUnit;
    }
    const QJsonValue kg = saved.value(QStringLiteral("riderWeightKg"));
    if (kg.isDouble()) {
        const double value = kg.toDouble();
        if (value == 0 || (value >= kMinWeightKg && value <= kMaxWeightKg)) {
            riderWeightKg = value;
        }
    }
}

void Units::setUnits(const QString& weightUnit, const QString& powerWeightUnit, const QString& distanceUnit) {
    if (kWeightUnits.contains(weightUnit)) {
        weight = weightUnit;
    }
    if (kPowerWeightUnits.contains(powerWeightUnit)) {
        powerWeight = powerWeightUnit;
    }
    if (kDistanceUnits.contains(distanceUnit)) {
        distance = distanceUnit;
    }
    settings::update({{QStringLiteral("weightUnit"), weight},
                      {QStringLiteral("powerWeightUnit"), powerWeight},
                      {QStringLiteral("distanceUnit"), distance}});
    emit changed();
}

void Units::setRiderWeightKg(double kg) {
    riderWeightKg = kg;
    settings::update({{QStringLiteral("riderWeightKg"), py::round(riderWeightKg, 3)}});
    emit changed();
}

void Units::reset() {
    weight = QStringLiteral("lbs");
    powerWeight = QStringLiteral("W/kg");
    distance = QStringLiteral("mi");
    riderWeightKg = 0;
}

double Units::weightFromKg(double kg) const {
    return weight == u"lbs" ? kg * kLbsPerKg : kg;
}

double Units::weightToKg(double value) const {
    return weight == u"lbs" ? value / kLbsPerKg : value;
}

QString Units::weightText(std::optional<double> kg) const {
    return formatWeight(kg.value_or(riderWeightKg)) + u' ' + weight;
}

QString Units::formatWeight(double kg) const {
    const double value = py::round(weightFromKg(kg), 1);
    return value != static_cast<double>(static_cast<long long>(value)) ? py::formatG(value)
                                                                        : QString::number(static_cast<long long>(value));
}

std::pair<double, double> Units::weightRange() const {
    return {weightFromKg(kMinWeightKg), weightFromKg(kMaxWeightKg)};
}

std::optional<double> Units::powerToWeight(std::optional<double> watts) const {
    if (!hasWeight() || !watts) {
        return std::nullopt;
    }
    const double perUnit = powerWeight == u"W/lb" ? riderWeightKg * kLbsPerKg : riderWeightKg;
    return *watts / perUnit;
}

QString Units::powerToWeightText(std::optional<double> watts, const QString& prefix, const QString& suffix) const {
    const std::optional<double> value = powerToWeight(watts);
    if (!value) {
        return {};
    }
    return prefix + py::fixed(*value, 2) + u' ' + powerWeight + suffix;
}

double Units::speed(double mph) const {
    return distance == u"km" ? mph * kKmPerMile : mph;
}

double Units::distanceFromMiles(double miles) const {
    return distance == u"km" ? miles * kKmPerMile : miles;
}

QString Units::speedUnit() const {
    return distance == u"km" ? QStringLiteral("KM/H") : QStringLiteral("MPH");
}

QString Units::distanceUnit(bool longName) const {
    if (distance == u"km") {
        return longName ? QStringLiteral("Km") : QStringLiteral("km");
    }
    return longName ? QStringLiteral("Miles") : QStringLiteral("mi");
}

}  // namespace awo

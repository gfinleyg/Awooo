#pragma once

// The measurements picked from the title bar's menu (Units.py), saved in savedSettingsANT.json:
//   "weightUnit":      "lbs" or "kg"     the rider's weight on the setup screen
//   "powerWeightUnit": "W/kg" or "W/lb"  power to weight on the synopsis (only with a weight set)
//   "distanceUnit":    "mi" or "km"      speed (MPH / KM/H) and distance on the workout screen and synopsis
//   "riderWeightKg":   the rider's weight, always in kg (0: not set)
//
// Everything is measured in one unit and only converted for showing: weight in kg, speed in MPH and distance in
// miles (as the sensors are read), so switching units never loses anything. Screens that show a unit connect to
// changed().

#include <QObject>
#include <QString>

#include <optional>
#include <utility>

namespace awo {

class Units : public QObject {
    Q_OBJECT

public:
    static constexpr double kLbsPerKg = 2.20462262;
    static constexpr double kKmPerMile = 1.609344;
    // the weights the setup screen accepts (0 is always fine: not set), in kg
    static constexpr double kMinWeightKg = 20;
    static constexpr double kMaxWeightKg = 300;

    static Units& current();

    QString weight = QStringLiteral("lbs");
    QString powerWeight = QStringLiteral("W/kg");
    QString distance = QStringLiteral("mi");
    double riderWeightKg = 0;

    // the saved units and weight (anything missing or unknown keeps its default)
    void load();
    // empty or unknown ones are left as they are
    void setUnits(const QString& weightUnit = {}, const QString& powerWeightUnit = {}, const QString& distanceUnit = {});
    void setRiderWeightKg(double kg);
    // back to the defaults (tests)
    void reset();

    double weightFromKg(double kg) const;
    double weightToKg(double value) const;
    // the rider's weight (or kg) in the weight unit, e.g. "165 lbs"; "0 lbs" when it isn't set
    QString weightText(std::optional<double> kg = std::nullopt) const;
    // kg in the weight unit, to a tenth (no decimal for a whole number)
    QString formatWeight(double kg) const;
    // the weights the setup screen accepts, in the weight unit
    std::pair<double, double> weightRange() const;

    bool hasWeight() const { return riderWeightKg > 0; }
    // watts per kg or lb of the rider; none without a weight
    std::optional<double> powerToWeight(std::optional<double> watts) const;
    // e.g. " (3.25 W/kg)", or "" without a weight
    QString powerToWeightText(std::optional<double> watts, const QString& prefix = QStringLiteral(" ("),
                              const QString& suffix = QStringLiteral(")")) const;

    double speed(double mph) const;
    double distanceFromMiles(double miles) const;
    QString speedUnit() const;
    QString distanceUnit(bool longName = true) const;

signals:
    void changed();
};

}  // namespace awo

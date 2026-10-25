#pragma once

// ANT+ data page decoding and building for the six sensor types AntNative
// handles: heart rate (120), bike speed (123), bike cadence (122), combined
// speed & cadence (121), bicycle power (11) and fitness equipment (17).
//
// decode_page() turns one 8-byte payload into named fields. It's stateless:
// anything that needs earlier messages (rates from accumulated counts,
// rollover) is the profiles' job (ant_profiles.hpp). The field names are the
// same in every language: the Python message dict's "page_data", the C API's
// ant_message.page_fields and each profile's per-page store.
//
// Sources: the ANT+ device profiles Heart Rate (Rev 2.5), Bike Speed and
// Cadence (Rev 2.1), Bicycle Power (Rev 5.1) and Fitness Equipment (Rev 5.0),
// and Common Data Pages (Rev 3.1); each page notes its section or table.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace antnative {

constexpr int kDeviceTypePower = 11;
constexpr int kDeviceTypeFitnessEquipment = 17;
constexpr int kDeviceTypeHeartRate = 120;
constexpr int kDeviceTypeSpeedCadence = 121;
constexpr int kDeviceTypeCadence = 122;
constexpr int kDeviceTypeSpeed = 123;

using FieldValue = std::variant<long long, double, bool>;

struct PageField {
    const char* name;
    FieldValue value;
};

struct PageData {
    // The page number (the page change toggle bit removed where the profile
    // has one), or -1 for a combined speed & cadence sensor, which has no pages
    int page{-1};
    // What the page is, e.g. "manufacturer_information"; "unknown" for a page
    // this doesn't decode
    const char* name{"unknown"};
    std::vector<PageField> fields;

    const PageField* find(const char* field_name) const;
};

// Whether the device type's page byte carries a page change toggle bit (bit 7)
bool has_page_toggle(int device_type);

// `payload` is the 8-byte ANT payload. Returns an empty PageData (page -1,
// no fields) for a payload of any other size.
PageData decode_page(int device_type, const std::uint8_t* payload, std::size_t size);

// Control pages a display sends, each 8 bytes. Out-of-range values are
// clamped; an empty optional sends the field's "use the default" value.
namespace pages {

// FE-C page 48: percent of the trainer's maximum resistance (0-100, 0.5% steps)
std::vector<std::uint8_t> basic_resistance(double percent);
// FE-C page 49: watts (0-4000, 0.25 W steps)
std::vector<std::uint8_t> target_power(double watts);
// FE-C page 50: kg/m (0-1.86), km/h (+ head wind, -127 to 127), 0-1
std::vector<std::uint8_t> wind_resistance(std::optional<double> coefficient_kg_per_m,
                                          std::optional<double> wind_speed_kmh,
                                          std::optional<double> drafting_factor);
// FE-C page 51: grade percent (-200 to 200, 0.01% steps), coefficient of
// rolling resistance (0-0.0127)
std::vector<std::uint8_t> track_resistance(std::optional<double> grade_percent,
                                           std::optional<double> rolling_resistance);
// FE-C page 55: kg (0-655.34), kg (0-50), m (0-2.54), front:back (0.03-7.65),
// mm added to the wheel diameter (0-10)
std::vector<std::uint8_t> user_configuration(std::optional<double> user_weight_kg,
                                             std::optional<double> bicycle_weight_kg,
                                             std::optional<double> wheel_diameter_m,
                                             std::optional<double> gear_ratio,
                                             std::optional<int> wheel_diameter_offset_mm);
// Common page 70: ask the sensor to send `page` `times` times (1-127), or
// until it's acknowledged when `acknowledged`. The descriptors pick a
// subpage, e.g. bicycle power parameters subpage 1 (crank) or 0xFE.
std::vector<std::uint8_t> request_page(int page, int times = 1, bool acknowledged = false,
                                       std::optional<int> descriptor_1 = std::nullopt,
                                       std::optional<int> descriptor_2 = std::nullopt);
// Common page 76: sport mode (1 running, 2 cycling, 5 swimming)
std::vector<std::uint8_t> mode_settings(int sport_mode);
// Heart rate page 32: gym mode on or off; empty leaves it as it is
std::vector<std::uint8_t> heart_rate_feature_command(std::optional<bool> gym_mode);
// Bicycle power page 1, manual zero (calibration ID 0xAA)
std::vector<std::uint8_t> power_manual_zero();
// Bicycle power page 1, auto zero on or off (calibration ID 0xAB)
std::vector<std::uint8_t> power_auto_zero(bool enabled);
// Bicycle power page 2 subpage 1: crank length in mm (110-236.5, 0.5 mm
// steps); empty asks the sensor to determine it
std::vector<std::uint8_t> power_crank_length(std::optional<double> millimetres);
// Bicycle power page 2 subpage 0xFE: switch the cycling dynamics pages on or
// off - force angles (power phase), pedal center offset, rider position
// (seated/standing) and torque barycenter, which need the sensor at 8 Hz
std::vector<std::uint8_t> power_cycling_dynamics(bool eight_hz, bool power_phase, bool pedal_center_offset,
                                                 bool rider_position, bool torque_barycenter);

}  // namespace pages

}  // namespace antnative

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
// Sources: the Fitness Equipment (Rev 5.0) and Bike Speed and Cadence
// (Rev 2.1) device profiles and the Recording Bike Power Data application
// note (Rev 1.0). Pages those don't define are marked where they're decoded.

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
// until it's acknowledged when `acknowledged`
std::vector<std::uint8_t> request_page(int page, int times = 1, bool acknowledged = false);
// Bicycle power page 1, manual zero (calibration ID 0xAA)
std::vector<std::uint8_t> power_manual_zero();

}  // namespace pages

}  // namespace antnative

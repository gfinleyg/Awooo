#include "ant_pages.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace antnative {

namespace {

unsigned u16le(const std::uint8_t* p, std::size_t at) {
    return static_cast<unsigned>(p[at]) | (static_cast<unsigned>(p[at + 1]) << 8);
}

unsigned u16be(const std::uint8_t* p, std::size_t at) {
    return (static_cast<unsigned>(p[at]) << 8) | static_cast<unsigned>(p[at + 1]);
}

unsigned u24le(const std::uint8_t* p, std::size_t at) {
    return u16le(p, at) | (static_cast<unsigned>(p[at + 2]) << 16);
}

unsigned long u32le(const std::uint8_t* p, std::size_t at) {
    return static_cast<unsigned long>(u16le(p, at)) | (static_cast<unsigned long>(u16le(p, at + 2)) << 16);
}

int s16le(const std::uint8_t* p, std::size_t at) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16le(p, at)));
}

class Builder {
public:
    explicit Builder(PageData& data) : data_(data) {}

    void integer(const char* name, long long value) { data_.fields.push_back({name, value}); }
    void number(const char* name, double value) { data_.fields.push_back({name, value}); }
    void flag(const char* name, bool value) { data_.fields.push_back({name, value}); }

    // Each of these leaves the field out when the raw value is the page's
    // "invalid" marker
    void integer_unless(const char* name, long long raw, long long invalid) {
        if (raw != invalid) integer(name, raw);
    }
    void scaled_unless(const char* name, long long raw, long long invalid, double scale, double offset = 0.0) {
        if (raw != invalid) number(name, raw * scale + offset);
    }

private:
    PageData& data_;
};

// The battery status byte shared by HRM page 7, BSC page 4 and common page 82
// (BSC Rev 2.1 Table 5-9): bits 0-3 coarse voltage (0xF invalid), 4-6 status
void battery_fields(Builder& b, std::uint8_t fractional, std::uint8_t descriptive) {
    const unsigned coarse = descriptive & 0x0F;
    if (coarse != 0x0F) {
        b.number("battery_voltage", coarse + fractional / 256.0);
    }
    b.integer("battery_status", (descriptive >> 4) & 0x07);
}

// Background pages 1-3 shared by the heart rate and bike speed/cadence
// profiles (BSC Rev 2.1 Tables 5-5 to 5-7; HRM pages 1-3 have the same layout)
bool legacy_background_page(Builder& b, PageData& data, const std::uint8_t* p) {
    switch (data.page) {
        case 1:
            data.name = "cumulative_operating_time";
            b.integer("cumulative_operating_time_s", static_cast<long long>(u24le(p, 1)) * 2);
            return true;
        case 2:
            data.name = "manufacturer_id";
            b.integer("manufacturer_id", p[1]);
            b.integer("serial_number_upper", u16le(p, 2));
            return true;
        case 3:
            data.name = "product_id";
            b.integer("hardware_version", p[1]);
            b.integer("software_version", p[2]);
            b.integer("model_number", p[3]);
            return true;
        default:
            return false;
    }
}

// ANT+ common pages. Layouts from the ANT+ Common Pages document as the
// profiles above reference it (80, 81 and 82 aren't spelled out in the PDFs
// on hand; 70 is BSC Rev 2.1 Table 5-11, 71 is FE Rev 5.0 Table 8-48).
bool common_page(Builder& b, PageData& data, const std::uint8_t* p) {
    switch (data.page) {
        case 0x46:
            data.name = "request_data_page";
            b.integer("requested_transmission", p[5]);
            b.integer("requested_page", p[6]);
            b.integer("command_type", p[7]);
            return true;
        case 0x47:
            data.name = "command_status";
            b.integer("last_command_id", p[1]);
            b.integer("sequence_number", p[2]);
            b.integer("command_status", p[3]);
            // FE Rev 5.0 Table 8-49: the last command's settings, as applied
            switch (p[1]) {
                case 0x30:
                    b.number("total_resistance_percent", p[7] * 0.5);
                    break;
                case 0x31:
                    b.number("target_power_w", u16le(p, 6) * 0.25);
                    break;
                case 0x32:
                    b.scaled_unless("wind_resistance_coefficient", p[5], 0xFF, 0.01);
                    b.scaled_unless("wind_speed_kmh", p[6], 0xFF, 1.0, -127.0);
                    b.scaled_unless("drafting_factor", p[7], 0xFF, 0.01);
                    break;
                case 0x33:
                    b.scaled_unless("grade_percent", u16le(p, 5), 0xFFFF, 0.01, -200.0);
                    b.scaled_unless("rolling_resistance_coefficient", p[7], 0xFF, 5e-5);
                    break;
                default:
                    break;
            }
            return true;
        case 0x50:
            data.name = "manufacturer_information";
            b.integer("hardware_revision", p[3]);
            b.integer("manufacturer_id", u16le(p, 4));
            b.integer("model_number", u16le(p, 6));
            return true;
        case 0x51:
            data.name = "product_information";
            b.integer_unless("software_revision_supplemental", p[2], 0xFF);
            b.integer("software_revision_main", p[3]);
            b.integer_unless("serial_number", static_cast<long long>(u32le(p, 4)), 0xFFFFFFFFLL);
            return true;
        case 0x52: {
            data.name = "battery_status";
            if (p[2] != 0xFF) {
                b.integer("number_of_batteries", p[2] & 0x0F);
                b.integer("battery_id", (p[2] >> 4) & 0x0F);
            }
            // byte 7 bit 7: operating time in 2 s (1) or 16 s (0) units
            const long long units = (p[7] & 0x80) ? 2 : 16;
            b.integer("cumulative_operating_time_s", static_cast<long long>(u24le(p, 3)) * units);
            battery_fields(b, p[6], p[7]);
            return true;
        }
        default:
            return false;
    }
}

// Heart rate (120). Bytes 4-7 of every page are the heart beat fields, which
// decode_message() reads; these are each page's bytes 1-3. Pages 1-3 and 7
// share the BSC layouts; pages 5 (swim interval summary) and 6 (capabilities)
// follow the ANT+ Heart Rate profile, which isn't among the documents on hand.
void heart_rate_page(Builder& b, PageData& data, const std::uint8_t* p) {
    if (legacy_background_page(b, data, p)) {
        return;
    }
    switch (data.page) {
        case 0:
            data.name = "default";
            return;
        case 4:
            data.name = "previous_heart_beat";
            b.integer("manufacturer_specific", p[1]);
            b.number("previous_heart_beat_event_time", u16le(p, 2) / 1024.0);
            return;
        case 5:
            data.name = "swim_interval_summary";
            b.integer_unless("interval_average_heart_rate", p[1], 0);
            b.integer_unless("interval_maximum_heart_rate", p[2], 0);
            b.integer_unless("session_average_heart_rate", p[3], 0);
            return;
        case 6:
            data.name = "capabilities";
            b.integer("features_supported", p[2]);
            b.integer("features_enabled", p[3]);
            b.flag("extended_running_features", (p[3] & 0x01) != 0);
            b.flag("extended_cycling_features", (p[3] & 0x02) != 0);
            b.flag("extended_swimming_features", (p[3] & 0x04) != 0);
            return;
        case 7:
            data.name = "battery_status";
            b.integer_unless("battery_level_percent", p[1], 0xFF);
            battery_fields(b, p[2], p[3]);
            return;
        default:
            common_page(b, data, p);
            return;
    }
}

// Bike speed (123) and cadence (122), BSC Rev 2.1 sections 5.3 and 6.3. Bytes
// 4-7 (event time, cumulative revolutions) are the same on every page.
void speed_or_cadence_page(Builder& b, PageData& data, const std::uint8_t* p) {
    if (legacy_background_page(b, data, p)) {
        return;
    }
    switch (data.page) {
        case 0:
            data.name = "default";
            return;
        case 4:
            data.name = "battery_status";
            battery_fields(b, p[2], p[3]);
            return;
        case 5:
            data.name = "motion";
            b.flag("stopped", (p[1] & 0x01) != 0);
            return;
        default:
            common_page(b, data, p);
            return;
    }
}

// Bicycle power (11). 0x10-0x13 and 0x20 follow the field order of the
// Recording Bike Power Data note (Tables 4-1 to 4-8), with the byte sizes the
// FE-C profile uses for the same fields (page 26); 0x01 is the calibration
// response AntWorkout's calibration already reads. Cycling dynamics pages
// (0xE0-0xE2) and get/set parameters (0x02) aren't defined in the documents
// on hand and are left undecoded.
void power_page(Builder& b, PageData& data, const std::uint8_t* p) {
    switch (data.page) {
        case 0x01:
            data.name = "calibration";
            b.integer("calibration_id", p[1]);
            if (p[1] == 0xAC || p[1] == 0xAF) {
                b.flag("calibration_success", p[1] == 0xAC);
                b.integer_unless("auto_zero_status", p[2], 0xFF);
                b.integer("calibration_data", s16le(p, 6));
            }
            return;
        case 0x10:
            data.name = "power_only";
            b.integer("event_count", p[1]);
            if (p[2] != 0xFF) {
                b.integer("pedal_power_percent", p[2] & 0x7F);
                b.flag("pedal_power_right", (p[2] & 0x80) != 0);
            }
            b.integer_unless("instantaneous_cadence", p[3], 0xFF);
            b.integer("accumulated_power", u16le(p, 4));
            b.integer("instantaneous_power", u16le(p, 6));
            return;
        case 0x11:
            data.name = "wheel_torque";
            b.integer("event_count", p[1]);
            b.integer("wheel_ticks", p[2]);
            b.integer_unless("instantaneous_cadence", p[3], 0xFF);
            b.integer("wheel_period", u16le(p, 4));            // 1/2048 s
            b.integer("accumulated_torque", u16le(p, 6));      // 1/32 Nm
            return;
        case 0x12:
            data.name = "crank_torque";
            b.integer("event_count", p[1]);
            b.integer("crank_ticks", p[2]);
            b.integer_unless("instantaneous_cadence", p[3], 0xFF);
            b.integer("crank_period", u16le(p, 4));            // 1/2048 s
            b.integer("accumulated_torque", u16le(p, 6));      // 1/32 Nm
            return;
        case 0x13:
            data.name = "torque_effectiveness_and_pedal_smoothness";
            b.integer("event_count", p[1]);
            b.scaled_unless("left_torque_effectiveness", p[2], 0xFF, 0.5);
            b.scaled_unless("right_torque_effectiveness", p[3], 0xFF, 0.5);
            if (p[5] == 0xFE) {
                b.scaled_unless("combined_pedal_smoothness", p[4], 0xFF, 0.5);
            } else {
                b.scaled_unless("left_pedal_smoothness", p[4], 0xFF, 0.5);
                b.scaled_unless("right_pedal_smoothness", p[5], 0xFF, 0.5);
            }
            return;
        case 0x20:
            // Crank torque frequency: the only big-endian ANT+ page
            data.name = "crank_torque_frequency";
            b.integer("event_count", p[1]);
            b.number("slope", u16be(p, 2) / 10.0);                // Nm/Hz
            b.integer("timestamp", u16be(p, 4));                  // 1/2000 s
            b.integer("torque_ticks_stamp", u16be(p, 6));
            return;
        default:
            common_page(b, data, p);
            return;
    }
}

// FE-C general and specific data pages carry the FE state in byte 7 bits 4-7
// (FE Rev 5.0 Table 8-10)
void fe_state(Builder& b, std::uint8_t byte7) {
    b.integer("fe_state", (byte7 >> 4) & 0x07);
    b.flag("lap_toggle", (byte7 & 0x80) != 0);
}

// Fitness equipment (17), FE Rev 5.0 section 8
void fitness_equipment_page(Builder& b, PageData& data, const std::uint8_t* p) {
    const unsigned caps = p[7] & 0x0F;
    switch (data.page) {
        case 0x01:
            data.name = "calibration_response";
            b.flag("zero_offset_calibration_success", (p[1] & 0x40) != 0);
            b.flag("spin_down_calibration_success", (p[1] & 0x80) != 0);
            b.scaled_unless("temperature_c", p[3], 0xFF, 0.5, -25.0);
            b.integer_unless("zero_offset", u16le(p, 4), 0xFFFF);
            b.integer_unless("spin_down_time_ms", u16le(p, 6), 0xFFFF);
            return;
        case 0x02:
            data.name = "calibration_in_progress";
            b.flag("zero_offset_calibration_pending", (p[1] & 0x40) != 0);
            b.flag("spin_down_calibration_pending", (p[1] & 0x80) != 0);
            b.integer("temperature_condition", (p[2] >> 4) & 0x03);
            b.integer("speed_condition", (p[2] >> 6) & 0x03);
            b.scaled_unless("temperature_c", p[3], 0xFF, 0.5, -25.0);
            b.scaled_unless("target_speed_mps", u16le(p, 4), 0xFFFF, 0.001);
            b.integer_unless("target_spin_down_time_ms", u16le(p, 6), 0xFFFF);
            return;
        case 0x10:
            data.name = "general_fe_data";
            b.integer("equipment_type", p[1] & 0x1F);
            b.number("elapsed_time_s", p[2] * 0.25);                // rolls over at 64 s
            if (caps & 0x04) {
                b.integer("distance_m", p[3]);                      // rolls over at 256 m
            }
            b.scaled_unless("speed_mps", u16le(p, 4), 0xFFFF, 0.001);
            b.integer_unless("heart_rate", p[6], 0xFF);
            b.integer("heart_rate_source", caps & 0x03);
            b.flag("virtual_speed", (caps & 0x08) != 0);
            fe_state(b, p[7]);
            return;
        case 0x11:
            data.name = "general_settings";
            b.scaled_unless("cycle_length_m", p[3], 0xFF, 0.01);
            b.scaled_unless("incline_percent", s16le(p, 4), 0x7FFF, 0.01);
            b.number("resistance_level_percent", p[6] * 0.5);
            fe_state(b, p[7]);
            return;
        case 0x12:
            data.name = "general_metabolic_data";
            b.scaled_unless("mets", u16le(p, 2), 0xFFFF, 0.01);
            b.scaled_unless("caloric_burn_rate_kcal_per_hr", u16le(p, 4), 0xFFFF, 0.1);
            if (caps & 0x01) {
                b.integer("calories", p[6]);                        // rolls over at 256
            }
            fe_state(b, p[7]);
            return;
        case 0x13:
            data.name = "treadmill";
            b.integer_unless("cadence", p[4], 0xFF);                // strides/min
            if (caps & 0x02) {
                b.number("negative_vertical_distance_m", p[5] * -0.1);
            }
            if (caps & 0x01) {
                b.number("positive_vertical_distance_m", p[6] * 0.1);
            }
            fe_state(b, p[7]);
            return;
        case 0x14:
            data.name = "elliptical";
            if (caps & 0x02) {
                b.number("positive_vertical_distance_m", p[2] * 0.1);
            }
            if (caps & 0x01) {
                b.integer("stride_count", p[3]);
            }
            b.integer_unless("cadence", p[4], 0xFF);
            b.integer_unless("instantaneous_power", u16le(p, 5), 0xFFFF);
            fe_state(b, p[7]);
            return;
        case 0x16:
            data.name = "rower";
            if (caps & 0x01) {
                b.integer("stroke_count", p[3]);
            }
            b.integer_unless("cadence", p[4], 0xFF);                // strokes/min
            b.integer_unless("instantaneous_power", u16le(p, 5), 0xFFFF);
            fe_state(b, p[7]);
            return;
        case 0x17:
            data.name = "climber";
            if (caps & 0x01) {
                b.integer("stride_cycles", p[3]);
            }
            b.integer_unless("cadence", p[4], 0xFF);
            b.integer_unless("instantaneous_power", u16le(p, 5), 0xFFFF);
            fe_state(b, p[7]);
            return;
        case 0x18:
            data.name = "nordic_skier";
            if (caps & 0x01) {
                b.integer("stride_count", p[3]);
            }
            b.integer_unless("cadence", p[4], 0xFF);
            b.integer_unless("instantaneous_power", u16le(p, 5), 0xFFFF);
            fe_state(b, p[7]);
            return;
        case 0x19: {
            data.name = "trainer";
            b.integer("update_event_count", p[1]);
            b.integer_unless("instantaneous_cadence", p[2], 0xFF);
            const unsigned watts = p[5] | ((p[6] & 0x0F) << 8);
            // 0xFFF marks both power fields invalid
            if (watts != 0xFFF) {
                b.integer("accumulated_power", u16le(p, 3));
                b.integer("instantaneous_power", watts);
            }
            const unsigned status = (p[6] >> 4) & 0x0F;
            b.integer("trainer_status", status);
            b.flag("bicycle_power_calibration_required", (status & 0x01) != 0);
            b.flag("resistance_calibration_required", (status & 0x02) != 0);
            b.flag("user_configuration_required", (status & 0x04) != 0);
            b.integer("target_power_limits", p[7] & 0x03);
            fe_state(b, p[7]);
            return;
        }
        case 0x1A:
            data.name = "trainer_torque";
            b.integer("update_event_count", p[1]);
            b.integer("wheel_ticks", p[2]);
            b.integer("wheel_period", u16le(p, 3));               // 1/2048 s
            b.integer("accumulated_torque", u16le(p, 5));         // 1/32 Nm
            fe_state(b, p[7]);
            return;
        case 0x30:
            data.name = "basic_resistance";
            b.number("total_resistance_percent", p[7] * 0.5);
            return;
        case 0x31:
            data.name = "target_power";
            b.number("target_power_w", u16le(p, 6) * 0.25);
            return;
        case 0x32:
            data.name = "wind_resistance";
            b.scaled_unless("wind_resistance_coefficient", p[5], 0xFF, 0.01);
            b.scaled_unless("wind_speed_kmh", p[6], 0xFF, 1.0, -127.0);
            b.scaled_unless("drafting_factor", p[7], 0xFF, 0.01);
            return;
        case 0x33:
            data.name = "track_resistance";
            b.scaled_unless("grade_percent", u16le(p, 5), 0xFFFF, 0.01, -200.0);
            b.scaled_unless("rolling_resistance_coefficient", p[7], 0xFF, 5e-5);
            return;
        case 0x36:
            data.name = "fe_capabilities";
            b.integer_unless("maximum_resistance_n", u16le(p, 5), 0xFFFF);
            b.flag("supports_basic_resistance", (p[7] & 0x01) != 0);
            b.flag("supports_target_power", (p[7] & 0x02) != 0);
            b.flag("supports_simulation", (p[7] & 0x04) != 0);
            return;
        case 0x37:
            data.name = "user_configuration";
            b.scaled_unless("user_weight_kg", u16le(p, 1), 0xFFFF, 0.01);
            b.integer_unless("wheel_diameter_offset_mm", p[4] & 0x0F, 0x0F);
            b.scaled_unless("bicycle_weight_kg", (p[4] >> 4) | (p[5] << 4), 0xFFF, 0.05);
            b.scaled_unless("wheel_diameter_m", p[6], 0xFF, 0.01);
            b.scaled_unless("gear_ratio", p[7], 0x00, 0.03);
            return;
        default:
            if (data.page >= 27 && data.page <= 47) {
                // FE Rev 5.0 8.7: future main pages still carry the FE state
                data.name = "future_main_page";
                fe_state(b, p[7]);
                return;
            }
            common_page(b, data, p);
            return;
    }
}

// Rounded half to even, as Python's round() does (what the Python builders
// these replaced used)
std::uint8_t clamp_byte(double value, double low, double high) {
    return static_cast<std::uint8_t>(std::nearbyint(std::clamp(value, low, high)));
}

std::uint16_t clamp_u16(double value, double low, double high) {
    return static_cast<std::uint16_t>(std::nearbyint(std::clamp(value, low, high)));
}

}  // namespace

const PageField* PageData::find(const char* field_name) const {
    for (const PageField& field : fields) {
        if (std::strcmp(field.name, field_name) == 0) {
            return &field;
        }
    }
    return nullptr;
}

bool has_page_toggle(int device_type) {
    return device_type == kDeviceTypeHeartRate || device_type == kDeviceTypeCadence ||
           device_type == kDeviceTypeSpeed;
}

PageData decode_page(int device_type, const std::uint8_t* payload, std::size_t size) {
    PageData data;
    if (payload == nullptr || size != 8 || device_type == kDeviceTypeSpeedCadence) {
        return data;
    }
    data.page = has_page_toggle(device_type) ? (payload[0] & 0x7F) : payload[0];
    Builder b(data);
    switch (device_type) {
        case kDeviceTypeHeartRate:
            heart_rate_page(b, data, payload);
            break;
        case kDeviceTypeCadence:
        case kDeviceTypeSpeed:
            speed_or_cadence_page(b, data, payload);
            break;
        case kDeviceTypePower:
            power_page(b, data, payload);
            break;
        case kDeviceTypeFitnessEquipment:
            fitness_equipment_page(b, data, payload);
            break;
        default:
            common_page(b, data, payload);
            break;
    }
    return data;
}

namespace pages {

std::vector<std::uint8_t> basic_resistance(double percent) {
    return {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, clamp_byte(percent * 2, 0, 200)};
}

std::vector<std::uint8_t> target_power(double watts) {
    const std::uint16_t quarter_watts = clamp_u16(watts * 4, 0, 16000);
    return {0x31, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            static_cast<std::uint8_t>(quarter_watts & 0xFF), static_cast<std::uint8_t>(quarter_watts >> 8)};
}

std::vector<std::uint8_t> wind_resistance(std::optional<double> coefficient_kg_per_m,
                                          std::optional<double> wind_speed_kmh,
                                          std::optional<double> drafting_factor) {
    return {0x32, 0xFF, 0xFF, 0xFF, 0xFF,
            coefficient_kg_per_m ? clamp_byte(*coefficient_kg_per_m * 100, 0, 186) : std::uint8_t{0xFF},
            wind_speed_kmh ? clamp_byte(*wind_speed_kmh + 127, 0, 254) : std::uint8_t{0xFF},
            drafting_factor ? clamp_byte(*drafting_factor * 100, 0, 100) : std::uint8_t{0xFF}};
}

std::vector<std::uint8_t> track_resistance(std::optional<double> grade_percent,
                                           std::optional<double> rolling_resistance) {
    const std::uint16_t grade = grade_percent ? clamp_u16((*grade_percent + 200) * 100, 0, 40000) : 0xFFFF;
    return {0x33, 0xFF, 0xFF, 0xFF, 0xFF,
            static_cast<std::uint8_t>(grade & 0xFF), static_cast<std::uint8_t>(grade >> 8),
            rolling_resistance ? clamp_byte(*rolling_resistance / 5e-5, 0, 254) : std::uint8_t{0xFF}};
}

std::vector<std::uint8_t> user_configuration(std::optional<double> user_weight_kg,
                                             std::optional<double> bicycle_weight_kg,
                                             std::optional<double> wheel_diameter_m,
                                             std::optional<double> gear_ratio,
                                             std::optional<int> wheel_diameter_offset_mm) {
    const std::uint16_t user = user_weight_kg ? clamp_u16(*user_weight_kg * 100, 0, 65534) : 0xFFFF;
    const std::uint16_t bike = bicycle_weight_kg ? clamp_u16(*bicycle_weight_kg * 20, 0, 1000) : 0xFFF;
    const std::uint8_t offset = wheel_diameter_offset_mm
        ? static_cast<std::uint8_t>(std::clamp(*wheel_diameter_offset_mm, 0, 10)) : std::uint8_t{0x0F};
    return {0x37,
            static_cast<std::uint8_t>(user & 0xFF), static_cast<std::uint8_t>(user >> 8),
            0xFF,
            static_cast<std::uint8_t>(offset | ((bike & 0x0F) << 4)), static_cast<std::uint8_t>(bike >> 4),
            wheel_diameter_m ? clamp_byte(*wheel_diameter_m * 100, 0, 254) : std::uint8_t{0xFF},
            gear_ratio ? clamp_byte(*gear_ratio / 0.03, 1, 255) : std::uint8_t{0x00}};
}

std::vector<std::uint8_t> request_page(int page, int times, bool acknowledged) {
    const std::uint8_t count = static_cast<std::uint8_t>(std::clamp(times, acknowledged ? 0 : 1, 127));
    return {0x46, 0xFF, 0xFF, 0xFF, 0xFF,
            static_cast<std::uint8_t>(count | (acknowledged ? 0x80 : 0x00)),
            static_cast<std::uint8_t>(std::clamp(page, 0, 255)), 0x01};
}

std::vector<std::uint8_t> power_manual_zero() {
    return {0x01, 0xAA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

}  // namespace pages

}  // namespace antnative

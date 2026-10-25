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

int s8(std::uint8_t value) {
    return static_cast<std::int8_t>(value);
}

// 256 binary radians to the circle
double brads_to_degrees(unsigned brads) {
    return brads * 360.0 / 256.0;
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
// (Common Data Pages Rev 3.1 Table 6-14): bits 0-3 coarse voltage (0xF
// invalid), 4-6 status
void battery_fields(Builder& b, std::uint8_t fractional, std::uint8_t descriptive) {
    const unsigned coarse = descriptive & 0x0F;
    if (coarse != 0x0F) {
        b.number("battery_voltage", coarse + fractional / 256.0);
    }
    b.integer("battery_status", (descriptive >> 4) & 0x07);
}

// Background pages 1-3 shared by the heart rate and bike speed/cadence
// profiles (HRM Rev 2.5 Tables 9-11, BSC Rev 2.1 Tables 5-5 to 5-7)
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

// SW revision as Common Data Pages Rev 3.1 Equations 6-3 and 6-4 read it
void software_version(Builder& b, std::uint8_t supplemental, std::uint8_t main) {
    b.integer_unless("software_revision_supplemental", supplemental, 0xFF);
    b.integer("software_revision_main", main);
    b.number("software_version", supplemental != 0xFF ? (main * 100 + supplemental) / 1000.0 : main / 10.0);
}

// Common page 84 subfields (Common Data Pages Rev 3.1 Table 6-17)
void subfield(Builder& b, std::uint8_t subpage, const std::uint8_t* p, std::size_t at) {
    switch (subpage) {
        case 1: b.number("temperature_c", s16le(p, at) * 0.01); break;
        case 2: b.number("barometric_pressure_kpa", u16le(p, at) * 0.01); break;
        case 3: b.number("humidity_percent", u16le(p, at) * 0.01); break;
        case 4: b.number("wind_speed_kmh", u16le(p, at) * 0.01); break;
        case 5: b.number("wind_direction_deg", u16le(p, at) * 0.05); break;
        case 6: b.integer("charging_cycles", u16le(p, at)); break;
        case 7: b.number("minimum_operating_temperature_c", s16le(p, at) * 0.01); break;
        case 8: b.number("maximum_operating_temperature_c", s16le(p, at) * 0.01); break;
        default: break;
    }
}

// ANT+ common pages (Common Data Pages Rev 3.1 section 6; page 71's response
// data for FE-C control pages is FE Rev 5.0 Table 8-49)
bool common_page(Builder& b, PageData& data, const std::uint8_t* p) {
    switch (data.page) {
        case 0x46:
            data.name = "request_data_page";
            b.integer_unless("slave_serial_number", u16le(p, 1), 0xFFFF);
            b.integer_unless("descriptor_1", p[3], 0xFF);
            b.integer_unless("descriptor_2", p[4], 0xFF);
            b.integer("requested_transmission", p[5]);
            b.integer("requested_page", p[6]);
            b.integer("command_type", p[7]);
            return true;
        case 0x47:
            data.name = "command_status";
            b.integer("last_command_id", p[1]);
            b.integer("sequence_number", p[2]);
            b.integer("command_status", p[3]);
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
        case 0x49:
            data.name = "generic_command";
            b.integer_unless("slave_serial_number", u16le(p, 1), 0xFFFF);
            b.integer("slave_manufacturer_id", u16le(p, 3));
            b.integer("sequence_number", p[5]);
            b.integer_unless("command_number", u16le(p, 6), 0xFFFF);
            return true;
        case 0x4A:
            data.name = "open_channel_command";
            b.integer("serial_number", u24le(p, 1));
            b.integer("channel_device_type", p[4]);
            b.integer("rf_frequency", p[5]);
            b.integer("channel_period", u16le(p, 6));
            return true;
        case 0x4C:
            data.name = "mode_settings";
            b.integer("sport_mode", p[7]);
            return true;
        case 0x4E:
            data.name = "multi_component_manufacturer_information";
            if (p[2] != 0xFF) {
                b.integer("number_of_components", p[2] & 0x0F);
                b.integer("component_id", (p[2] >> 4) & 0x0F);
            }
            b.integer("hardware_revision", p[3]);
            b.integer("manufacturer_id", u16le(p, 4));
            b.integer("model_number", u16le(p, 6));
            return true;
        case 0x4F:
            data.name = "multi_component_product_information";
            if (p[1] != 0xFF) {
                b.integer("number_of_components", p[1] & 0x0F);
                b.integer("component_id", (p[1] >> 4) & 0x0F);
            }
            software_version(b, p[2], p[3]);
            b.integer_unless("serial_number", static_cast<long long>(u32le(p, 4)), 0xFFFFFFFFLL);
            return true;
        case 0x50:
            data.name = "manufacturer_information";
            b.integer("hardware_revision", p[3]);
            b.integer("manufacturer_id", u16le(p, 4));
            b.integer("model_number", u16le(p, 6));
            return true;
        case 0x51:
            data.name = "product_information";
            software_version(b, p[2], p[3]);
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
        case 0x53:
            data.name = "time_and_date";
            b.integer("seconds", p[2]);
            b.integer("minutes", p[3]);
            b.integer("hours", p[4]);
            b.integer("day", p[5] & 0x1F);
            b.integer_unless("day_of_week", (p[5] >> 5) & 0x07, 7);   // Sunday 0
            b.integer("month", p[6]);
            b.integer("year", 2000 + p[7]);
            return true;
        case 0x54:
            data.name = "subfield_data";
            b.integer_unless("subpage_1", p[2], 0xFF);
            b.integer_unless("subpage_2", p[3], 0xFF);
            subfield(b, p[2], p, 4);
            subfield(b, p[3], p, 6);
            return true;
        case 0x55:
            data.name = "memory_level";
            b.number("memory_used_percent", p[4] * 0.5);
            b.number("memory_total_size", u16le(p, 5) * 0.1);
            b.flag("memory_size_in_bytes", (p[7] & 0x80) != 0);       // else bits
            b.integer("memory_size_prefix", p[7] & 0x7F);              // 0 base, 1 kilo, 2 mega, 3 tera
            return true;
        case 0x56:
            data.name = "paired_devices";
            b.integer("peripheral_index", p[1]);
            b.integer("connected_devices", p[2]);
            b.flag("paired", (p[3] & 0x80) != 0);
            b.integer("connection_state", (p[3] >> 3) & 0x0F);         // 0 closed, 1 searching, 2 synchronised
            b.integer("network_key", p[3] & 0x07);                     // 0 public, 1 private, 2 ANT+, 3 ANT-FS
            b.integer("peripheral_device_number", u16le(p, 4));
            b.integer("peripheral_transmission_type", p[6]);
            b.integer("peripheral_device_type", p[7]);
            return true;
        case 0x57:
            data.name = "error_description";
            b.integer("system_component", p[2] & 0x0F);                // 0xF: the whole system
            b.integer("error_level", (p[2] >> 6) & 0x03);              // 1 warning, 2 critical
            b.integer_unless("profile_error_code", p[3], 0xFF);
            b.integer_unless("manufacturer_error_code", static_cast<long long>(u32le(p, 4)), 0xFFFFFFFFLL);
            return true;
        default:
            return false;
    }
}

// Heart rate (120), HRM Rev 2.5 section 6.5. Bytes 4-7 of every page are the
// heart beat fields, which decode_message() reads; these are each page's
// bytes 1-3. Heart rate monitors don't send common pages (section 6.7), and a
// legacy one's page byte means nothing, so nothing else is decoded.
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
            b.integer_unless("manufacturer_specific", p[1], 0xFF);
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
            b.flag("extended_running_supported", (p[2] & 0x01) != 0);
            b.flag("extended_cycling_supported", (p[2] & 0x02) != 0);
            b.flag("extended_swimming_supported", (p[2] & 0x04) != 0);
            b.flag("gym_mode_supported", (p[2] & 0x08) != 0);
            b.flag("extended_running_enabled", (p[3] & 0x01) != 0);
            b.flag("extended_cycling_enabled", (p[3] & 0x02) != 0);
            b.flag("extended_swimming_enabled", (p[3] & 0x04) != 0);
            b.flag("gym_mode_enabled", (p[3] & 0x08) != 0);
            return;
        case 7:
            data.name = "battery_status";
            if (p[1] <= 100) {
                b.integer("battery_level_percent", p[1]);
            }
            battery_fields(b, p[2], p[3]);
            return;
        case 9:
            // Optical sensors compute the beat time from the heart rate rather
            // than measuring it: don't derive more (e.g. HRV) from those
            data.name = "device_information";
            b.integer("heartbeat_event_type", p[1] & 0x03);
            b.flag("heartbeat_time_computed", (p[1] & 0x03) == 1);
            return;
        default:
            return;
    }
}

// Bike speed (123) and cadence (122), BSC Rev 2.1 sections 5.3 and 6.3. Bytes
// 4-7 (event time, cumulative revolutions) are the same on every page. Only
// pages 0-5 are defined (common pages are only sent to these sensors), and a
// legacy sensor's page byte means nothing.
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
            return;
    }
}

// Bicycle power calibration messages (Power Rev 5.1 section 14)
void power_calibration(Builder& b, const std::uint8_t* p) {
    b.integer("calibration_id", p[1]);
    switch (p[1]) {
        case 0xAB:   // auto zero configuration (Table 14-4)
            b.integer_unless("auto_zero_status", p[2], 0xFF);
            break;
        case 0xAC:   // calibration response (Table 14-5)
        case 0xAF:
            b.flag("calibration_success", p[1] == 0xAC);
            b.integer_unless("auto_zero_status", p[2], 0xFF);   // 0 off, 1 on
            b.integer("calibration_data", s16le(p, 6));
            break;
        case 0x12:   // auto zero support (Table 14-6)
            b.flag("auto_zero_supported", (p[2] & 0x01) != 0);
            b.flag("auto_zero_enabled", (p[2] & 0x02) != 0);
            break;
        case 0x10:   // crank torque frequency defined messages, big endian (Tables 14-8 to 14-13)
            b.integer("ctf_defined_id", p[2]);
            switch (p[2]) {
                case 0x01: b.integer("ctf_zero_offset_hz", u16be(p, 6)); break;
                case 0x02: b.number("ctf_slope", u16be(p, 6) / 10.0); break;
                case 0x03: b.integer("ctf_serial_number", u16be(p, 6)); break;
                case 0xAC: b.integer("ctf_acknowledged_id", p[3]); break;
                default: break;
            }
            break;
        default:     // 0xAA request, 0xBA-0xBD custom parameters (manufacturer specific)
            break;
    }
}

// Get/set parameters subpages (Power Rev 5.1 section 15.2). The capability
// bits of subpages 0xFD and 0xFE are inverted: 0 means supported / enabled.
void power_parameters(Builder& b, const std::uint8_t* p) {
    b.integer("subpage", p[1]);
    switch (p[1]) {
        case 0x01:   // crank parameters (Tables 15-3 to 15-5)
            if (p[4] <= 0xFD) {
                b.number("crank_length_mm", 110.0 + p[4] * 0.5);
            }
            b.flag("auto_crank_length_requested", p[4] == 0xFE);
            b.integer("crank_length_status", p[5] & 0x03);           // 0 invalid, 1 default, 2 manual, 3 auto/fixed
            b.integer("sensor_software_mismatch", (p[5] >> 2) & 0x03);
            b.integer("sensor_availability", (p[5] >> 4) & 0x03);    // 1 left, 2 right, 3 both present
            b.integer("custom_calibration_status", (p[5] >> 6) & 0x03);
            b.flag("auto_crank_length_supported", (p[6] & 0x01) != 0);
            break;
        case 0x02:   // power phase configuration (Table 15-7)
            if (p[2] <= 200) {
                b.number("peak_torque_threshold_percent", p[2] * 0.5);
            }
            break;
        case 0x04:   // rider position configuration (Table 15-8)
            b.integer("transition_time_offset_s", p[2]);
            break;
        case 0xFD:   // advanced capabilities 1 (Tables 15-9 to 15-12)
            b.flag("crank_length_is_default", (p[2] & 0x01) == 0);
            b.flag("crank_length_required", (p[2] & 0x02) == 0);
            b.flag("supports_4hz", (p[4] & 0x01) == 0);
            b.flag("supports_8hz", (p[4] & 0x02) == 0);
            b.flag("supports_auto_zero", (p[4] & 0x10) == 0);
            b.flag("supports_auto_crank_length", (p[4] & 0x20) == 0);
            b.flag("supports_torque_effectiveness", (p[4] & 0x40) == 0);
            b.flag("transmitting_4hz", (p[6] & 0x01) == 0);
            b.flag("transmitting_8hz", (p[6] & 0x02) == 0);
            b.flag("auto_zero_enabled", (p[6] & 0x10) == 0);
            b.flag("auto_crank_length_enabled", (p[6] & 0x20) == 0);
            b.flag("torque_effectiveness_enabled", (p[6] & 0x40) == 0);
            break;
        case 0xFE:   // advanced capabilities 2: the cycling dynamics pages (Tables 15-13 to 15-15)
            b.flag("supports_4hz", (p[4] & 0x01) == 0);
            b.flag("supports_8hz", (p[4] & 0x02) == 0);
            b.flag("supports_power_phase", (p[4] & 0x08) == 0);
            b.flag("supports_pedal_center_offset", (p[4] & 0x10) == 0);
            b.flag("supports_rider_position", (p[4] & 0x20) == 0);
            b.flag("supports_torque_barycenter", (p[4] & 0x40) == 0);
            b.flag("transmitting_4hz", (p[6] & 0x01) == 0);
            b.flag("transmitting_8hz", (p[6] & 0x02) == 0);
            b.flag("power_phase_enabled", (p[6] & 0x08) == 0);
            b.flag("pedal_center_offset_enabled", (p[6] & 0x10) == 0);
            b.flag("rider_position_enabled", (p[6] & 0x20) == 0);
            b.flag("torque_barycenter_enabled", (p[6] & 0x40) == 0);
            break;
        default:
            break;
    }
}

// Right/left force angle (Power Rev 5.1 Tables 17-1, 17-2): angles in brads,
// 0xC0 in both of a pair = not supported
void force_angles(Builder& b, const std::uint8_t* p) {
    b.integer("event_count", p[1]);
    if (!(p[2] == 0xC0 && p[3] == 0xC0)) {
        b.number("start_angle_deg", brads_to_degrees(p[2]));
        b.number("end_angle_deg", brads_to_degrees(p[3]));
    }
    if (!(p[4] == 0xC0 && p[5] == 0xC0)) {
        b.number("start_peak_angle_deg", brads_to_degrees(p[4]));
        b.number("end_peak_angle_deg", brads_to_degrees(p[5]));
    }
    b.number("average_torque_nm", u16le(p, 6) / 32.0);
}

// Bicycle power (11), Power Rev 5.1 sections 8-19
void power_page(Builder& b, PageData& data, const std::uint8_t* p) {
    switch (data.page) {
        case 0x01:
            data.name = "calibration";
            power_calibration(b, p);
            return;
        case 0x02:
            data.name = "parameters";
            power_parameters(b, p);
            return;
        case 0x03: {
            // Measurement output (Tables 16-1, 16-2): value x 2^scale
            data.name = "measurement_output";
            b.integer("number_of_data_types", p[1] & 0x0F);
            b.integer("data_type", p[2]);
            b.integer("scale_factor", s8(p[3]));
            b.integer("timestamp", u16le(p, 4));                       // 1/2048 s
            const int value = s16le(p, 6);
            const bool may_be_invalid = p[2] == 11 || p[2] == 12 || (p[2] >= 32 && p[2] <= 35) ||
                                        p[2] == 40 || p[2] == 41;
            if (!(may_be_invalid && value == -32768)) {
                b.number("measurement_value", std::ldexp(static_cast<double>(value), s8(p[3])));
            }
            return;
        }
        case 0x10:
            data.name = "power_only";
            b.integer("event_count", p[1]);
            if (p[2] != 0xFF) {
                b.integer("pedal_power_percent", p[2] & 0x7F);
                b.flag("pedal_power_right", (p[2] & 0x80) != 0);   // else unknown which pedal
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
        case 0x14:
            data.name = "torque_barycenter";
            b.number("torque_barycenter_angle_deg", 30.0 + p[1] * 0.5);
            return;
        case 0x20:
            // Crank torque frequency: big endian
            data.name = "crank_torque_frequency";
            b.integer("event_count", p[1]);
            b.number("slope", u16be(p, 2) / 10.0);                // Nm/Hz
            b.integer("timestamp", u16be(p, 4));                  // 1/2000 s
            b.integer("torque_ticks_stamp", u16be(p, 6));
            return;
        case 0xE0:
            data.name = "right_force_angle";
            force_angles(b, p);
            return;
        case 0xE1:
            data.name = "left_force_angle";
            force_angles(b, p);
            return;
        case 0xE2: {
            // Pedal position (Table 17-3)
            data.name = "pedal_position";
            b.integer("event_count", p[1]);
            const int position = (p[2] >> 6) & 0x03;   // 0 seated, 1 to seated, 2 standing, 3 to standing
            b.integer("rider_position", position);
            b.flag("standing", position == 2);
            b.integer_unless("cadence", p[3], 0xFF);
            b.integer_unless("right_pedal_center_offset_mm", s8(p[4]), -128);
            b.integer_unless("left_pedal_center_offset_mm", s8(p[5]), -128);
            return;
        }
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

std::vector<std::uint8_t> request_page(int page, int times, bool acknowledged, std::optional<int> descriptor_1,
                                       std::optional<int> descriptor_2) {
    const std::uint8_t count = static_cast<std::uint8_t>(std::clamp(times, acknowledged ? 0 : 1, 127));
    auto descriptor = [](std::optional<int> value) {
        return value ? static_cast<std::uint8_t>(std::clamp(*value, 0, 254)) : std::uint8_t{0xFF};
    };
    return {0x46, 0xFF, 0xFF, descriptor(descriptor_1), descriptor(descriptor_2),
            static_cast<std::uint8_t>(count | (acknowledged ? 0x80 : 0x00)),
            static_cast<std::uint8_t>(std::clamp(page, 0, 255)), 0x01};
}

std::vector<std::uint8_t> mode_settings(int sport_mode) {
    return {0x4C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, static_cast<std::uint8_t>(std::clamp(sport_mode, 0, 255))};
}

std::vector<std::uint8_t> heart_rate_feature_command(std::optional<bool> gym_mode) {
    // byte 6 bit 7: apply byte 7 bit 7 (gym mode)
    return {0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            static_cast<std::uint8_t>(gym_mode ? 0xFF : 0x7F),
            static_cast<std::uint8_t>(gym_mode && *gym_mode ? 0x80 : 0x00)};
}

std::vector<std::uint8_t> power_manual_zero() {
    return {0x01, 0xAA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

std::vector<std::uint8_t> power_auto_zero(bool enabled) {
    return {0x01, 0xAB, static_cast<std::uint8_t>(enabled ? 0x01 : 0x00), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

std::vector<std::uint8_t> power_crank_length(std::optional<double> millimetres) {
    // (length - 110 mm) / 0.5 mm; 0xFE asks the sensor to work it out
    const std::uint8_t value = millimetres ? clamp_byte((*millimetres - 110.0) * 2, 0, 253) : std::uint8_t{0xFE};
    return {0x02, 0x01, 0xFF, 0xFF, value, 0x00, 0x00, 0xFF};
}

std::vector<std::uint8_t> power_cycling_dynamics(bool eight_hz, bool power_phase, bool pedal_center_offset,
                                                 bool rider_position, bool torque_barycenter) {
    // Subpage 0xFE: the mask's 0 bits are the settings to apply (all but the
    // reserved bits 2 and 7); in the value 0 enables, 1 disables
    const std::uint8_t mask = 0x84;
    std::uint8_t value = 0x84;
    value |= eight_hz ? 0x01 : 0x02;   // 4 Hz off and 8 Hz on, or the reverse
    value |= power_phase ? 0x00 : 0x08;
    value |= pedal_center_offset ? 0x00 : 0x10;
    value |= rider_position ? 0x00 : 0x20;
    value |= torque_barycenter ? 0x00 : 0x40;
    return {0x02, 0xFE, 0xFF, 0xFF, mask, 0xFF, value, 0xFF};
}

}  // namespace pages

}  // namespace antnative

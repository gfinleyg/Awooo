#include "ant_profiles.hpp"

namespace antnative {

namespace {

constexpr int kCommonPageBattery = 0x52;
constexpr int kHeartRatePageBattery = 0x07;
constexpr int kBscPageBattery = 0x04;

std::optional<double> battery_voltage(int fractional, int descriptive) {
    const int coarse = descriptive & 0x0F;
    if (coarse == 0x0F) {
        return std::nullopt;
    }
    return coarse + fractional / 256.0;
}

int u16le(const std::vector<std::uint8_t>& p, std::size_t at) {
    return p[at] | (p[at + 1] << 8);
}

}  // namespace

// ---- Battery ---------------------------------------------------------------

bool BatteryStatus::known_status() const {
    return status >= kBatteryStatusNew && status <= kBatteryStatusCritical;
}

bool BatteryStatus::is_low() const {
    if (status == kBatteryStatusLow || status == kBatteryStatusCritical) {
        return true;
    }
    if (!known_status() && level_percent) {
        return *level_percent <= kLowBatteryPercent;
    }
    return false;
}

int BatteryStatus::severity() const {
    if (status == kBatteryStatusCritical) {
        return 3;
    }
    if (is_low()) {
        return 2;
    }
    if (known_status()) {
        return 1;
    }
    return 0;
}

std::optional<BatteryStatus> decode_battery(int device_type, const std::vector<std::uint8_t>* payload) {
    // A combined speed/cadence sensor has no way to report its battery
    if (payload == nullptr || payload->size() < 8 || device_type == kDeviceTypeSpeedCadence) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t>& p = *payload;
    int page = p[0];
    if (has_page_toggle(device_type)) {
        page &= 0x7F;
    }

    BatteryStatus battery;
    if (page == kCommonPageBattery) {
        battery.status = (p[7] >> 4) & 0x07;
        battery.voltage = battery_voltage(p[6], p[7]);
        battery.battery_id = p[2] != 0xFF ? (p[2] >> 4) & 0x0F : 0;
        return battery;
    }
    if (device_type == kDeviceTypeHeartRate && page == kHeartRatePageBattery) {
        battery.status = (p[3] >> 4) & 0x07;
        if (p[1] <= 100) {
            battery.level_percent = p[1];
        }
        battery.voltage = battery_voltage(p[2], p[3]);
        return battery;
    }
    if ((device_type == kDeviceTypeCadence || device_type == kDeviceTypeSpeed) && page == kBscPageBattery) {
        battery.status = (p[3] >> 4) & 0x07;
        battery.voltage = battery_voltage(p[2], p[3]);
        return battery;
    }
    return std::nullopt;
}

// ---- Profile ---------------------------------------------------------------

bool Profile::accept_page(int page, int raw_page, bool toggle, const std::vector<std::uint8_t>& payload) {
    if (last_payload_key_ && last_payload_key_->first == page && last_payload_key_->second == payload) {
        return false;
    }
    last_payload_key_ = std::make_pair(page, payload);
    last_page = page;
    last_raw = RawPage{page, raw_page, toggle, payload};
    ++pages_received;
    ++page_counts[page];
    PageData decoded = decode_page(device_type(), payload.data(), 8);
    if (decoded.page >= 0) {
        pages[decoded.page] = std::move(decoded);
    }
    return true;
}

// ---- Heart rate (profiles/heart_rate.py) -----------------------------------

void HeartRateProfile::on_message(const DecodedMessage& message) {
    if (!message.payload || message.payload->size() < 8 || !message.page) {
        return;
    }
    const std::vector<std::uint8_t>& payload = *message.payload;
    if (!accept_page(*message.page, payload[0], message.page_change_toggle.value_or(false), payload)) {
        return;
    }
    if (!message.heart_beat_event_time) {
        return;
    }

    const double event_time = *message.heart_beat_event_time;
    const int event_ticks = message.heart_beat_event_ticks.value_or(static_cast<int>(event_time * 1024));

    // Rollover-safe: the 16 bit event time wraps every 64 s
    if (!last_event_ticks_) {
        event_time_unwrapped = event_time;
    } else {
        const int delta_ticks = (event_ticks - *last_event_ticks_) & 0xFFFF;
        if (delta_ticks < 0x8000) {
            *event_time_unwrapped += delta_ticks / 1024.0;
        }
    }
    last_event_ticks_ = event_ticks;

    ++readings_received;
    latest = HeartRateReading{
        *message.page,
        message.heart_beat_count.value_or(0),
        event_time,
        message.computed_heart_rate.value_or(0),
        message.previous_heart_beat_event_time,
        message.rr_interval,
        event_time_unwrapped,
    };
    ++version;
}

// ---- Bicycle power (profiles/power_meter.py) -------------------------------

void PowerMeterProfile::on_message(const DecodedMessage& message) {
    if (!message.payload || message.payload->size() < 8 || !message.page) {
        return;
    }
    const std::vector<std::uint8_t>& payload = *message.payload;
    if (!accept_page(*message.page, payload[0], message.page_change_toggle.value_or(false), payload)) {
        return;
    }
    if (!message.instantaneous_power_watts) {
        return;
    }

    // Instantaneous cadence is byte 3 of the Standard Power-Only page (byte 2
    // is pedal power balance)
    const int cadence_raw = payload[3];
    ++readings_received;
    latest = PowerReading{
        *message.page,
        *message.instantaneous_power_watts,
        cadence_raw == 0xFF ? std::nullopt : std::optional<int>(cadence_raw),
    };
    ++version;
}

// ---- Speed / cadence (profiles/speed_cadence.py) ---------------------------

void SpeedCadenceProfile::on_message(const DecodedMessage& message) {
    if (!message.payload || message.payload->size() < 8) {
        return;
    }
    // Every message counts towards dropping a held rate (exact repeats too,
    // which is how a stopped sensor sounds); a new rate resets it below
    ++messages_since_cadence_;
    ++messages_since_speed_;
    if (device_type_ == kDeviceTypeSpeedCadence) {
        handle_combined(*message.payload, message);
    } else {
        handle_single_function(*message.payload, message);
    }
}

std::optional<double> SpeedCadenceProfile::held_cadence_rpm() const {
    if (!held_cadence_rpm_) {
        return std::nullopt;
    }
    return messages_since_cadence_ > kHoldMessages ? 0.0 : *held_cadence_rpm_;
}

std::optional<double> SpeedCadenceProfile::held_speed_mps() const {
    if (!held_speed_mps_) {
        return std::nullopt;
    }
    return messages_since_speed_ > kHoldMessages ? 0.0 : *held_speed_mps_;
}

void SpeedCadenceProfile::handle_single_function(const std::vector<std::uint8_t>& payload,
                                                 const DecodedMessage& message) {
    if (!message.page) {
        return;
    }
    const int page = *message.page;
    if (!accept_page(page, payload[0], message.page_change_toggle.value_or(false), payload)) {
        return;
    }

    int event_ticks;
    int revolutions;
    if (message.event_ticks) {
        event_ticks = *message.event_ticks;
        revolutions = message.revolution_count.value_or(0);
    } else {
        // The native decode only fills these in for page 0, but every page
        // (0-5, and a legacy sensor's page-less layout) carries event time +
        // cumulative revolutions in bytes 4-7
        event_ticks = u16le(payload, 4);
        revolutions = u16le(payload, 6);
    }
    const double event_time = event_ticks / 1024.0;

    if (!last_event_ticks_) {
        event_time_unwrapped = event_time;
    } else {
        const int dt = (event_ticks - *last_event_ticks_) & 0xFFFF;
        if (dt < 0x8000) {
            *event_time_unwrapped += dt / 1024.0;
        }
    }

    SpeedCadenceReading reading;
    reading.page = page;
    reading.event_time = event_time;
    reading.revolution_count = revolutions;
    reading.event_ticks = event_ticks;
    if (last_event_ticks_ && last_revolutions_) {
        const int dt_ticks = (event_ticks - *last_event_ticks_) & 0xFFFF;
        const int drev = (revolutions - *last_revolutions_) & 0xFFFF;
        if (dt_ticks && dt_ticks < 0x8000) {
            const double dt = dt_ticks / 1024.0;
            if (device_type_ == kDeviceTypeSpeed) {
                reading.speed_mps = drev * wheel_circumference_m / dt;
                reading.speed_kph = *reading.speed_mps * 3.6;
                held_speed_mps_ = reading.speed_mps;
                messages_since_speed_ = 0;
            } else if (device_type_ == kDeviceTypeCadence) {
                reading.cadence_rpm = drev * 60.0 / dt;
                held_cadence_rpm_ = reading.cadence_rpm;
                messages_since_cadence_ = 0;
            }
        }
    }
    last_event_ticks_ = event_ticks;
    last_revolutions_ = revolutions;
    reading.event_time_unwrapped = event_time_unwrapped;
    latest = reading;
    ++version;
}

void SpeedCadenceProfile::handle_combined(const std::vector<std::uint8_t>& payload, const DecodedMessage& message) {
    // No page number in this format - dedup on the raw payload alone
    if (last_combined_key_ && *last_combined_key_ == payload) {
        return;
    }
    last_combined_key_ = payload;
    if (!message.cadence_event_ticks) {
        return;
    }
    last_combined_payload = payload;
    ++pages_received;

    const int cadence_ticks = *message.cadence_event_ticks;
    const double cadence_event_time = message.cadence_event_time.value_or(cadence_ticks / 1024.0);
    const int cadence_revolutions = message.cadence_revolution_count.value_or(0);
    const int speed_ticks = message.speed_event_ticks.value_or(0);
    const double speed_event_time = message.speed_event_time.value_or(speed_ticks / 1024.0);
    const int speed_revolutions = message.speed_revolution_count.value_or(0);

    if (!last_cadence_ticks_) {
        cadence_event_time_unwrapped = cadence_event_time;
    } else {
        const int dt = (cadence_ticks - *last_cadence_ticks_) & 0xFFFF;
        if (dt < 0x8000) {
            *cadence_event_time_unwrapped += dt / 1024.0;
        }
    }
    if (!last_speed_ticks_) {
        speed_event_time_unwrapped = speed_event_time;
    } else {
        const int dt = (speed_ticks - *last_speed_ticks_) & 0xFFFF;
        if (dt < 0x8000) {
            *speed_event_time_unwrapped += dt / 1024.0;
        }
    }

    CombinedSpeedCadenceReading reading;
    reading.cadence_event_time = cadence_event_time;
    reading.cadence_revolution_count = cadence_revolutions;
    reading.speed_event_time = speed_event_time;
    reading.speed_revolution_count = speed_revolutions;
    reading.cadence_event_ticks = cadence_ticks;
    reading.speed_event_ticks = speed_ticks;
    if (last_cadence_ticks_ && last_cadence_revs_) {
        const int dt_ticks = (cadence_ticks - *last_cadence_ticks_) & 0xFFFF;
        const int drev = (cadence_revolutions - *last_cadence_revs_) & 0xFFFF;
        if (dt_ticks && dt_ticks < 0x8000) {
            reading.cadence_rpm = drev * 60.0 / (dt_ticks / 1024.0);
            held_cadence_rpm_ = reading.cadence_rpm;
            messages_since_cadence_ = 0;
        }
    }
    if (last_speed_ticks_ && last_speed_revs_) {
        const int dt_ticks = (speed_ticks - *last_speed_ticks_) & 0xFFFF;
        const int drev = (speed_revolutions - *last_speed_revs_) & 0xFFFF;
        if (dt_ticks && dt_ticks < 0x8000) {
            reading.speed_mps = drev * wheel_circumference_m / (dt_ticks / 1024.0);
            reading.speed_kph = *reading.speed_mps * 3.6;
            held_speed_mps_ = reading.speed_mps;
            messages_since_speed_ = 0;
        }
    }
    last_cadence_ticks_ = cadence_ticks;
    last_cadence_revs_ = cadence_revolutions;
    last_speed_ticks_ = speed_ticks;
    last_speed_revs_ = speed_revolutions;
    reading.cadence_event_time_unwrapped = cadence_event_time_unwrapped;
    reading.speed_event_time_unwrapped = speed_event_time_unwrapped;
    latest_combined = reading;
    ++version;
}

// ---- Fitness equipment (profiles/fitness_equipment.py) ---------------------

void FitnessEquipmentProfile::on_message(const DecodedMessage& message) {
    if (!message.payload || message.payload->size() < 8) {
        return;
    }
    const std::vector<std::uint8_t>& payload = *message.payload;
    const int raw_page = payload[0];
    const int page = raw_page & 0x7F;
    if (!accept_page(page, raw_page, (raw_page & 0x80) != 0, payload)) {
        return;
    }

    if (page == 0x19) {
        // Specific Trainer Data: byte 2 cadence (RPM), bytes 5-6 bits 0-11
        // instantaneous power (W, 0xFFF invalid), byte 6 bits 4-7 trainer status
        const int watts = payload[5] | ((payload[6] & 0x0F) << 8);
        if (watts == 0xFFF) {
            return;
        }
        const int cadence = payload[2];
        ++readings_received;
        latest = FitnessEquipmentReading{
            0x19, watts, cadence == 0xFF ? std::nullopt : std::optional<int>(cadence), (payload[6] >> 4) & 0x0F};
        ++version;
    } else if (page == 0x36) {
        const int max_resistance = u16le(payload, 5);
        const int flags = payload[7];
        capabilities = FitnessEquipmentCapabilities{
            max_resistance == 0xFFFF ? std::nullopt : std::optional<int>(max_resistance),
            (flags & 0x01) != 0, (flags & 0x02) != 0, (flags & 0x04) != 0};
        ++capabilities_version;
    }
}

// ---- Lookup ----------------------------------------------------------------

bool has_profile(int device_type) {
    switch (device_type) {
        case kDeviceTypeHeartRate:
        case kDeviceTypePower:
        case kDeviceTypeFitnessEquipment:
        case kDeviceTypeSpeedCadence:
        case kDeviceTypeCadence:
        case kDeviceTypeSpeed:
            return true;
        default:
            return false;
    }
}

std::shared_ptr<Profile> make_profile(int device_type) {
    switch (device_type) {
        case kDeviceTypeHeartRate:
            return std::make_shared<HeartRateProfile>();
        case kDeviceTypePower:
            return std::make_shared<PowerMeterProfile>();
        case kDeviceTypeFitnessEquipment:
            return std::make_shared<FitnessEquipmentProfile>();
        case kDeviceTypeSpeedCadence:
        case kDeviceTypeCadence:
        case kDeviceTypeSpeed:
            return std::make_shared<SpeedCadenceProfile>(device_type);
        default:
            return nullptr;
    }
}

}  // namespace antnative

#pragma once

// ANT+ battery status and the per-sensor profiles: what a sensor's stream of
// messages adds up to. Ports of the Python ant package's battery.py and
// profiles/*.py, which are now thin layers over these. The profiles work from
// a DecodedMessage's fields exactly as the Python ones worked from the message
// dict (a field that's absent there is an empty optional here), so a message
// built by hand - as the tests' fakes do - is handled the same way.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "ant_core.hpp"
#include "ant_pages.hpp"

namespace antnative {

// ---- Battery status (battery.py) ------------------------------------------

constexpr int kBatteryStatusNew = 1;
constexpr int kBatteryStatusGood = 2;
constexpr int kBatteryStatusOk = 3;
constexpr int kBatteryStatusLow = 4;
constexpr int kBatteryStatusCritical = 5;
constexpr int kBatteryStatusInvalid = 7;
// Only used when a device reports a percentage but no usable status
constexpr int kLowBatteryPercent = 15;

struct BatteryStatus {
    int status{0};                      // kBatteryStatus* (0/6/7 = unknown)
    std::optional<int> level_percent;
    std::optional<double> voltage;
    int battery_id{0};

    bool known_status() const;
    bool is_low() const;
    // Higher is worse, for picking the weakest of several batteries
    int severity() const;
};

// The battery status a payload carries for `device_type` (common page 82,
// HRM page 7, BSC page 4), if it's one of those pages
std::optional<BatteryStatus> decode_battery(int device_type, const std::vector<std::uint8_t>* payload);

// ---- Profiles --------------------------------------------------------------

// What every profile tracks; `version` changes whenever `latest` does, so a
// caller can tell a new reading from the same one (the Python layer hands out
// the same reading object until it does)
class Profile {
public:
    virtual ~Profile() = default;

    // The device type this profile instance decodes for
    virtual int device_type() const = 0;
    // Callers that share a profile between threads hold `mutex` around this
    // and around reading the members (Sensor::update() does)
    virtual void on_message(const DecodedMessage& message) = 0;

    mutable std::mutex mutex;

    int pages_received{0};
    std::optional<int> last_page;
    std::map<int, int> page_counts;
    // Readings only (pages_received counts every new page)
    int readings_received{0};
    std::uint64_t version{0};

    // The last page received of each page number, decoded (see ant_pages.hpp)
    std::map<int, PageData> pages;

    // The last page's raw form: page number, the raw page byte, its page change
    // toggle and the payload. Empty until a page is received.
    struct RawPage {
        int page{0};
        int raw_page{0};
        bool page_change_toggle{false};
        std::vector<std::uint8_t> payload;
    };
    std::optional<RawPage> last_raw;

protected:
    // Counts a new page and stores it decoded; false for an exact repeat of
    // the last one (sensors repeat pages), which is otherwise ignored
    bool accept_page(int page, int raw_page, bool toggle, const std::vector<std::uint8_t>& payload);

    std::optional<std::pair<int, std::vector<std::uint8_t>>> last_payload_key_;
};

struct HeartRateReading {
    int page{0};
    int heart_beat_count{0};
    double heart_beat_event_time{0};
    int computed_heart_rate{0};
    std::optional<double> previous_heart_beat_event_time;
    std::optional<double> rr_interval;
    std::optional<double> event_time_unwrapped;
};

class HeartRateProfile : public Profile {
public:
    int device_type() const override { return kDeviceTypeHeartRate; }
    void on_message(const DecodedMessage& message) override;

    std::optional<HeartRateReading> latest;
    std::optional<double> event_time_unwrapped;

private:
    std::optional<int> last_event_ticks_;
};

struct PowerReading {
    int page{0};
    int instantaneous_power_watts{0};
    std::optional<int> power_meter_cadence_rpm;
};

class PowerMeterProfile : public Profile {
public:
    int device_type() const override { return kDeviceTypePower; }
    void on_message(const DecodedMessage& message) override;

    std::optional<PowerReading> latest;
};

// Speed-only (123) / cadence-only (122)
struct SpeedCadenceReading {
    int page{0};
    double event_time{0};
    int revolution_count{0};
    int event_ticks{0};
    std::optional<double> speed_mps;
    std::optional<double> speed_kph;
    std::optional<double> cadence_rpm;
    std::optional<double> event_time_unwrapped;
};

// Combined speed and cadence (121)
struct CombinedSpeedCadenceReading {
    double cadence_event_time{0};
    int cadence_revolution_count{0};
    double speed_event_time{0};
    int speed_revolution_count{0};
    int cadence_event_ticks{0};
    int speed_event_ticks{0};
    std::optional<double> cadence_rpm;
    std::optional<double> speed_mps;
    std::optional<double> speed_kph;
    std::optional<double> cadence_event_time_unwrapped;
    std::optional<double> speed_event_time_unwrapped;
};

class SpeedCadenceProfile : public Profile {
public:
    explicit SpeedCadenceProfile(int device_type, double wheel_circumference_m = 2.199)
        : device_type_(device_type), wheel_circumference_m(wheel_circumference_m) {}

    int device_type() const override { return device_type_; }
    void on_message(const DecodedMessage& message) override;

    double wheel_circumference_m;
    // The combined sensor's (121) payload of its last new message
    std::optional<std::vector<std::uint8_t>> last_combined_payload;
    std::optional<SpeedCadenceReading> latest;
    std::optional<CombinedSpeedCadenceReading> latest_combined;

    std::optional<double> event_time_unwrapped;
    std::optional<double> cadence_event_time_unwrapped;
    std::optional<double> speed_event_time_unwrapped;

    // The rates as a display shows them: `latest` has none for a message that
    // repeats the last wheel/crank event (no new interval to measure), so
    // these hold the last computed rate - until kHoldMessages messages pass
    // without a new event (about 3 s at 4 Hz: stopped), when they drop to 0.
    // Empty until a rate has been computed. Used by the C API's snapshots.
    static constexpr int kHoldMessages = 12;
    std::optional<double> held_cadence_rpm() const;
    std::optional<double> held_speed_mps() const;

private:
    void handle_single_function(const std::vector<std::uint8_t>& payload, const DecodedMessage& message);
    void handle_combined(const std::vector<std::uint8_t>& payload, const DecodedMessage& message);

    int device_type_;
    std::optional<double> held_cadence_rpm_, held_speed_mps_;
    int messages_since_cadence_{0}, messages_since_speed_{0};
    std::optional<std::vector<std::uint8_t>> last_combined_key_;
    std::optional<int> last_event_ticks_, last_revolutions_;
    std::optional<int> last_cadence_ticks_, last_cadence_revs_, last_speed_ticks_, last_speed_revs_;
};

struct FitnessEquipmentReading {
    int page{0x19};
    int instantaneous_power_watts{0};
    std::optional<int> power_meter_cadence_rpm;
    int trainer_status{0};
};

struct FitnessEquipmentCapabilities {
    std::optional<int> maximum_resistance_newtons;
    bool supports_basic_resistance{false};
    bool supports_target_power{false};
    bool supports_simulation{false};
};

class FitnessEquipmentProfile : public Profile {
public:
    int device_type() const override { return kDeviceTypeFitnessEquipment; }
    void on_message(const DecodedMessage& message) override;

    std::optional<FitnessEquipmentReading> latest;
    std::optional<FitnessEquipmentCapabilities> capabilities;
    std::uint64_t capabilities_version{0};
};

// The profile for a device type, or nullptr for one there isn't a profile for
std::shared_ptr<Profile> make_profile(int device_type);
bool has_profile(int device_type);

}  // namespace antnative

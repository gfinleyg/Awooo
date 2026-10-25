// AntNative C++ API: a header-only wrapper over the C API (antnative.h).
//
// Built on the C API rather than exporting C++ classes from the DLL, so it
// works whatever compiler or runtime library the program uses. Link
// antnative.lib and ship antnative.dll as for the C API.
//
//   antnative::Device stick;
//   stick.open();                          // stick 0
//   stick.reset();
//   stick.set_network_key(0, key);
//   ... assign / configure / open channels ...
//   stick.start_messages();
//   for (const ant_message& m : stick.poll()) {
//       if (m.fields & ANT_FIELD_HEART_RATE) { ... m.computed_heart_rate ... }
//   }
//
// Calls returning bool return false when the stick refused or didn't answer
// in time; misuse and failures throw antnative::Error.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "antnative.h"

namespace antnative {

class Error : public std::runtime_error {
public:
    Error(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
    int code() const { return code_; }

private:
    int code_;
};

namespace detail {

// ANT_OK -> true, ANT_FAILED -> false, anything negative throws
inline bool check(int rc) {
    if (rc < 0) {
        throw Error(rc, ant_last_error());
    }
    return rc == ANT_OK;
}

}  // namespace detail

using Payload = std::array<std::uint8_t, 8>;

struct UsbDevice {
    std::uint32_t index{};
    std::uint32_t vid{};
    std::uint32_t pid{};
    std::uint32_t serial_number{};
    std::uint32_t device_type{};
    std::optional<std::string> product_description;
    std::optional<std::string> serial_string;
};

inline std::vector<UsbDevice> list_devices() {
    const int count = ant_list_devices(nullptr, 0);
    detail::check(count < 0 ? count : ANT_OK);
    std::vector<ant_usb_device> raw(static_cast<std::size_t>(count));
    const int filled = raw.empty() ? 0 : ant_list_devices(raw.data(), count);
    detail::check(filled < 0 ? filled : ANT_OK);

    std::vector<UsbDevice> devices;
    // (parenthesized so windows.h's min macro can't get in the way)
    for (int i = 0; i < (std::min)(count, filled); ++i) {
        const ant_usb_device& item = raw[static_cast<std::size_t>(i)];
        UsbDevice device;
        device.index = item.index;
        device.vid = item.vid;
        device.pid = item.pid;
        device.serial_number = item.serial_number;
        device.device_type = item.device_type;
        if (item.has_product_description) {
            device.product_description = item.product_description;
        }
        if (item.has_serial_string) {
            device.serial_string = item.serial_string;
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

// Decodes one raw message the way poll() does
inline ant_message decode_message(std::uint8_t message_id, const std::vector<std::uint8_t>& data,
                                  int channel = -1) {
    ant_message message{};
    detail::check(ant_decode_message(message_id, data.data(), data.size(), channel, &message));
    return message;
}

// One ANT USB stick. Destroying it stops messages and closes the stick.
class Device {
public:
    Device() : handle_(ant_create()) {
        if (handle_ == nullptr) {
            throw Error(ANT_ERROR, ant_last_error());
        }
    }

    ~Device() { ant_destroy(handle_); }

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Device(Device&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Device& operator=(Device&& other) noexcept {
        if (this != &other) {
            ant_destroy(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    bool open(std::uint32_t baudrate = 57600, std::uint8_t device_number = 0) {
        return detail::check(ant_open(handle_, baudrate, device_number));
    }
    void close() { ant_close(handle_); }
    bool is_open() const { return ant_is_open(handle_) != 0; }

    bool reset(std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_reset(handle_, timeout_ms));
    }
    bool set_network_key(std::uint8_t network_number, const Payload& key, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_network_key(handle_, network_number, key.data(), timeout_ms));
    }
    bool assign_channel(std::uint8_t channel, std::uint8_t channel_type, std::uint8_t network_number,
                        std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_assign_channel(handle_, channel, channel_type, network_number, timeout_ms));
    }
    bool unassign_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_unassign_channel(handle_, channel, timeout_ms));
    }
    bool set_channel_id(std::uint8_t channel, std::uint16_t device_number, std::uint8_t device_type,
                        std::uint8_t transmission_type, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_channel_id(handle_, channel, device_number, device_type,
                                                transmission_type, timeout_ms));
    }
    bool set_channel_period(std::uint8_t channel, std::uint16_t period, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_channel_period(handle_, channel, period, timeout_ms));
    }
    bool set_channel_rf_frequency(std::uint8_t channel, std::uint8_t rf_frequency, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_channel_rf_frequency(handle_, channel, rf_frequency, timeout_ms));
    }
    bool set_search_priority(std::uint8_t channel, std::uint8_t priority, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_search_priority(handle_, channel, priority, timeout_ms));
    }
    bool set_search_timeout(std::uint8_t channel, std::uint8_t timeout, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_search_timeout(handle_, channel, timeout, timeout_ms));
    }
    bool set_low_priority_search_timeout(std::uint8_t channel, std::uint8_t timeout, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_low_priority_search_timeout(handle_, channel, timeout, timeout_ms));
    }
    bool set_rx_extended_messages_enabled(bool enabled, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_set_rx_extended_messages_enabled(handle_, enabled ? 1 : 0, timeout_ms));
    }
    bool open_rx_scan_mode(std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_open_rx_scan_mode(handle_, timeout_ms));
    }
    bool open_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_open_channel(handle_, channel, timeout_ms));
    }
    bool close_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000) {
        return detail::check(ant_close_channel(handle_, channel, timeout_ms));
    }
    bool request_message(std::uint8_t channel, std::uint8_t message_id) {
        return detail::check(ant_request_message(handle_, channel, message_id));
    }
    // Empty when the request fails or times out
    std::optional<ant_channel_id> get_channel_id(std::uint8_t channel, std::uint32_t timeout_ms = 1000) {
        ant_channel_id id{};
        if (!detail::check(ant_get_channel_id(handle_, channel, timeout_ms, &id))) {
            return std::nullopt;
        }
        return id;
    }

    bool send_broadcast(std::uint8_t channel, const Payload& payload) {
        return detail::check(ant_send_broadcast(handle_, channel, payload.data()));
    }
    // Returns the ANT_TRANSFER_* result (ANT_TRANSFER_PASS on success)
    int send_acknowledged(std::uint8_t channel, const Payload& payload, std::uint32_t timeout_ms = 1000) {
        int transfer = ANT_TRANSFER_FAIL;
        detail::check(ant_send_acknowledged(handle_, channel, payload.data(), timeout_ms, &transfer));
        return transfer;
    }
    bool send_burst_packet(std::uint8_t channel_sequence, const Payload& payload) {
        return detail::check(ant_send_burst_packet(handle_, channel_sequence, payload.data()));
    }

    void start_messages() { detail::check(ant_start_messages(handle_)); }
    void stop_messages() { ant_stop_messages(handle_); }
    // Up to `max` queued messages, oldest first
    std::vector<ant_message> poll(int max = 64) {
        std::vector<ant_message> messages(static_cast<std::size_t>(max));
        const int count = max > 0 ? ant_poll(handle_, messages.data(), max) : 0;
        detail::check(count < 0 ? count : ANT_OK);
        messages.resize(static_cast<std::size_t>(count));
        return messages;
    }
    std::uint64_t dropped_messages() const { return ant_dropped_messages(handle_); }

    ant_device* handle() const { return handle_; }

private:
    ant_device* handle_;
};

// A decoded page's field by name, if it has it
inline std::optional<double> page_field(const ant_page& page, const char* name) {
    double value = 0;
    if (ant_page_get_field(&page, name, &value) != ANT_OK) {
        return std::nullopt;
    }
    return value;
}

// Every ANT+ page of a sensor type, decoded; empty for a combined speed &
// cadence sensor, which has no pages
inline std::optional<ant_page> decode_page(int device_type, const Payload& payload) {
    ant_page page{};
    if (!detail::check(ant_decode_page(device_type, payload.data(), &page))) {
        return std::nullopt;
    }
    return page;
}

// Control pages (antnative.h); std::nullopt sends a field's default
namespace pages {

inline double or_default(std::optional<double> value) {
    return value ? *value : std::numeric_limits<double>::quiet_NaN();
}

inline Payload basic_resistance(double percent) {
    Payload p{};
    ant_page_basic_resistance(percent, p.data());
    return p;
}
inline Payload target_power(double watts) {
    Payload p{};
    ant_page_target_power(watts, p.data());
    return p;
}
inline Payload wind_resistance(std::optional<double> coefficient_kg_per_m, std::optional<double> wind_speed_kmh,
                               std::optional<double> drafting_factor) {
    Payload p{};
    ant_page_wind_resistance(or_default(coefficient_kg_per_m), or_default(wind_speed_kmh),
                             or_default(drafting_factor), p.data());
    return p;
}
inline Payload track_resistance(std::optional<double> grade_percent,
                                std::optional<double> rolling_resistance = std::nullopt) {
    Payload p{};
    ant_page_track_resistance(or_default(grade_percent), or_default(rolling_resistance), p.data());
    return p;
}
inline Payload user_configuration(std::optional<double> user_weight_kg, std::optional<double> bicycle_weight_kg,
                                  std::optional<double> wheel_diameter_m, std::optional<double> gear_ratio) {
    Payload p{};
    ant_page_user_configuration(or_default(user_weight_kg), or_default(bicycle_weight_kg),
                                or_default(wheel_diameter_m), or_default(gear_ratio), p.data());
    return p;
}
inline Payload request_page(std::uint8_t page, std::uint8_t times = 1, bool acknowledged = false) {
    Payload p{};
    ant_page_request(page, times, acknowledged ? 1 : 0, p.data());
    return p;
}

}  // namespace pages

// The sensors on one stick (ant_manager in antnative.h): scanning, dedicated
// channels, each sensor's profile, and the watchdog.
class Manager {
public:
    Manager() : handle_(ant_manager_create()) {
        if (handle_ == nullptr) {
            throw Error(ANT_ERROR, ant_last_error());
        }
    }
    ~Manager() { ant_manager_destroy(handle_); }

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Manager& operator=(Manager&& other) noexcept {
        if (this != &other) {
            ant_manager_destroy(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    // `device_number` < 0: the first stick that opens
    bool open(int device_number = -1) { return detail::check(ant_manager_open(handle_, device_number)); }
    void close() { ant_manager_close(handle_); }
    // Throws when the stick refuses the scan set-up
    void configure_scan(const Payload& network_key, std::uint8_t network_number = 0, std::uint8_t channel = 0,
                        std::uint8_t rf_frequency = 57) {
        require(ant_manager_configure_scan(handle_, network_key.data(), network_number, channel, rf_frequency));
    }
    void start() { detail::check(ant_manager_start(handle_)); }
    void stop() { ant_manager_stop(handle_); }

    void open_device_channels(const std::vector<ant_sensor_id>& sensors) {
        require(ant_manager_open_device_channels(handle_, sensors.data(), static_cast<int>(sensors.size())));
    }
    void return_to_scan() { require(ant_manager_return_to_scan(handle_)); }
    std::optional<int> channel_for(std::uint8_t device_type, std::uint16_t device_number) {
        const int channel = ant_manager_channel_for(handle_, device_type, device_number);
        return channel < 0 ? std::nullopt : std::optional<int>(channel);
    }
    // Returns the ANT_TRANSFER_* result (ANT_TRANSFER_PASS on success)
    int send(std::uint8_t device_type, std::uint16_t device_number, const Payload& payload,
             std::uint32_t timeout_ms = 500) {
        int transfer = ANT_TRANSFER_FAIL;
        detail::check(ant_manager_send(handle_, device_type, device_number, payload.data(), timeout_ms, &transfer));
        return transfer;
    }

    std::vector<ant_sensor> sensors() {
        const int count = ant_manager_sensors(handle_, nullptr, 0);
        detail::check(count < 0 ? count : ANT_OK);
        std::vector<ant_sensor> result(static_cast<std::size_t>(count));
        const int filled = result.empty() ? 0 : ant_manager_sensors(handle_, result.data(), count);
        detail::check(filled < 0 ? filled : ANT_OK);
        result.resize(static_cast<std::size_t>((std::min)(count, filled)));
        return result;
    }
    std::optional<ant_page> sensor_page(std::uint8_t device_type, std::uint16_t device_number, int page) {
        ant_page result{};
        if (!detail::check(ant_manager_sensor_page(handle_, device_type, device_number, page, &result))) {
            return std::nullopt;
        }
        return result;
    }
    bool set_wheel_circumference(std::uint16_t device_number, double meters) {
        return detail::check(ant_manager_set_wheel_circumference(handle_, device_number, meters));
    }
    std::vector<ant_message> poll(int max = 64) {
        std::vector<ant_message> messages(static_cast<std::size_t>(max));
        const int count = max > 0 ? ant_manager_poll(handle_, messages.data(), max) : 0;
        detail::check(count < 0 ? count : ANT_OK);
        messages.resize(static_cast<std::size_t>(count));
        return messages;
    }

    void start_recovery(const ant_recovery_options* options = nullptr) {
        detail::check(ant_manager_start_recovery(handle_, options));
    }
    void stop_recovery() { ant_manager_stop_recovery(handle_); }
    void watch(const std::vector<ant_sensor_id>& sensors) {
        detail::check(ant_manager_watch(handle_, sensors.data(), static_cast<int>(sensors.size())));
    }
    void watch_all() { detail::check(ant_manager_watch(handle_, nullptr, -1)); }
    // The next line of the manager's and watchdog's log, if there is one
    std::optional<std::string> next_log() {
        char line[512];
        return ant_manager_next_log(handle_, line, sizeof line) > 0 ? std::optional<std::string>(line) : std::nullopt;
    }

    ant_manager* handle() const { return handle_; }

private:
    // A refusal (ANT_FAILED) is an error too for these
    static void require(int rc) {
        if (!detail::check(rc)) {
            throw Error(rc, ant_last_error());
        }
    }

    ant_manager* handle_;
};

}  // namespace antnative

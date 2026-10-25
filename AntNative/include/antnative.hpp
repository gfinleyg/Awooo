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

}  // namespace antnative

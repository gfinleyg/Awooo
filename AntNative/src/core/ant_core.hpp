#pragma once

// AntNative core: the ANT SDK transport (USB stick, channels, message pump)
// and the per-message decode, with nothing language-specific in it. The
// Python module (src/python/ant_native_module.cpp) and the C library
// (src/c_api/antnative_c.cpp, include/antnative.h) are thin layers over
// this, so both see exactly the same behavior.
//
// Threading: the blocking calls (anything taking a timeout, plus open/close)
// run one at a time across threads and block until the stick answers. The
// message pump runs on its own thread and hands every message to the
// handler given to start_message_pump(), on that thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ant_pages.hpp"

class DSISerialGeneric;
class DSIFramerANT;

namespace antnative {

// ANT SDK framer result codes (DSIFramerANT's ANTFRAMER_*), as returned by
// Stick::send_acknowledged()
constexpr int kFramerFail = 0;
constexpr int kFramerPass = 1;
constexpr int kFramerTimeout = 2;
constexpr int kFramerCancelled = 3;
constexpr int kFramerInvalidParam = 4;

// One USB ANT stick, copied out of the SDK's enumeration
struct UsbDeviceInfo {
    unsigned int index{};
    unsigned int vid{};
    unsigned int pid{};
    unsigned long serial_number{};
    unsigned int device_type{};
    std::optional<std::string> product_description;
    std::optional<std::string> serial_string;
};

struct ChannelId {
    unsigned int device_number{};
    unsigned int device_type{};
    unsigned int transmission_type{};
};

// A message as the SDK's framer hands it over
struct RawMessage {
    std::uint8_t message_id{};
    std::vector<std::uint8_t> data;
    std::uint8_t channel{0xFF};
    bool has_channel{false};
    bool is_error{false};
    std::uint8_t error_code{0};
};

// Everything decode_message() reads out of a RawMessage. Fields that only
// some messages carry are optional, and are filled together in the groups
// commented below. Field names match the Python message dict's keys.
struct DecodedMessage {
    int message_id{};
    std::vector<std::uint8_t> data;
    int message_size{};
    const char* message_name{""};
    bool is_error{false};
    std::optional<int> error_code;

    // Data messages (0x4E/0x4F/0x50 and legacy 0x5D/0x5E/0x5F); also set by
    // Channel ID (0x51) and RESPONSE_EVENT (0x40) messages
    std::optional<int> channel;
    // The 8-byte ANT RF payload of a data message (shorter only if the
    // message itself is)
    std::optional<std::vector<std::uint8_t>> payload;

    // Flagged extended data (standard data message IDs, 10+ bytes)
    bool has_extended_data{false};
    std::optional<int> extended_bitfield;
    std::optional<bool> has_device_id;
    std::optional<bool> has_rssi;
    std::optional<bool> has_timestamp;
    std::optional<bool> has_extension;
    std::optional<std::vector<std::uint8_t>> extended_data;
    std::optional<std::vector<std::uint8_t>> rssi_data;
    std::optional<bool> has_timestamp_field;

    // Device identity: from extended data, or a Channel ID (0x51) response
    std::optional<unsigned int> device_number;
    std::optional<int> device_type;
    std::optional<int> transmission_type;

    // RESPONSE_EVENT (0x40)
    std::optional<int> response_to_message_id;
    std::optional<int> event_code;
    const char* event_name{nullptr};

    // The payload's page decoded field by field (ant_pages.hpp), once the
    // device type is known from the extended data
    std::optional<PageData> page_data;

    // ANT+ profile pages, decoded once the device type is known (see
    // decode_profile in ant_core.cpp)
    std::optional<int> page;
    std::optional<bool> page_change_toggle;
    // Heart rate (120), every page
    std::optional<double> heart_beat_event_time;
    std::optional<int> heart_beat_event_ticks;
    std::optional<int> heart_beat_count;
    std::optional<int> computed_heart_rate;
    // Heart rate page 4
    std::optional<double> previous_heart_beat_event_time;
    std::optional<double> rr_interval;
    // Bicycle power (11) Standard Power-Only page
    std::optional<int> instantaneous_power_watts;
    std::optional<int> power_meter_cadence_rpm;
    // Combined speed/cadence (121)
    std::optional<double> cadence_event_time;
    std::optional<int> cadence_event_ticks;
    std::optional<int> cadence_revolution_count;
    std::optional<double> speed_event_time;
    std::optional<int> speed_event_ticks;
    std::optional<int> speed_revolution_count;
    // Speed-only (123) / cadence-only (122) page 0
    std::optional<double> event_time;
    std::optional<int> event_ticks;
    std::optional<int> revolution_count;
};

DecodedMessage decode_message(const RawMessage& message);

// Name of an ANT RESPONSE_EVENT code ("UNKNOWN" for codes not listed)
const char* response_event_name(std::uint8_t code);

class Stick {
public:
    using MessageHandler = std::function<void(const RawMessage&)>;

    static std::vector<UsbDeviceInfo> list_devices();

    Stick() = default;
    ~Stick();
    Stick(const Stick&) = delete;
    Stick& operator=(const Stick&) = delete;

    bool open(std::uint32_t baudrate = 57600, std::uint8_t device_number = 0);
    void close();
    bool is_open() const;

    // Throws std::runtime_error for a channel or message ID outside 0..255
    bool request_message(int channel, int message_id);
    // Throws std::runtime_error for a channel outside 0..255; empty when the
    // request fails or times out
    std::optional<ChannelId> get_channel_id(int channel, std::uint32_t response_timeout_ms = 1000);

    // Starts the pump thread, which hands every message to `handler` on
    // that thread. Throws std::runtime_error if the device isn't open. A
    // pump already running keeps its handler.
    void start_message_pump(MessageHandler handler);
    // Stops and joins the pump thread. The handler must never start or stop
    // the pump (it would wait on its own thread).
    void stop_message_pump();

    bool reset(std::uint32_t response_timeout_ms = 1000);
    // Throws std::invalid_argument unless `key` is 8 bytes
    bool set_network_key(std::uint8_t network_number, const std::vector<std::uint8_t>& key,
                         std::uint32_t response_timeout_ms = 1000);
    bool assign_channel(std::uint8_t channel, std::uint8_t channel_type,
                        std::uint8_t network_number, std::uint32_t timeout_ms = 1000);
    bool unassign_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000);
    bool set_channel_id(std::uint8_t channel, std::uint16_t device_number,
                        std::uint8_t device_type, std::uint8_t transmission_type,
                        std::uint32_t timeout_ms = 1000);
    bool set_channel_period(std::uint8_t channel, std::uint16_t period,
                            std::uint32_t timeout_ms = 1000);
    bool set_channel_rf_frequency(std::uint8_t channel, std::uint8_t rf_frequency,
                                  std::uint32_t timeout_ms = 1000);
    bool set_search_priority(std::uint8_t channel, std::uint8_t priority,
                             std::uint32_t timeout_ms = 1000);
    // Search timeouts in 2.5 s units, 0xFF = search forever. A dedicated
    // slave channel whose device drops out goes back to searching on its
    // own; an infinite timeout keeps the stick from closing the channel.
    bool set_search_timeout(std::uint8_t channel, std::uint8_t timeout,
                            std::uint32_t timeout_ms = 1000);
    bool set_low_priority_search_timeout(std::uint8_t channel, std::uint8_t timeout,
                                         std::uint32_t timeout_ms = 1000);
    bool set_rx_extended_messages_enabled(bool enabled, std::uint32_t timeout_ms = 1000);
    bool open_rx_scan_mode(std::uint32_t timeout_ms = 1000);
    bool open_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000);
    bool close_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000);

    // The payload sends throw std::invalid_argument unless `payload` is 8 bytes
    bool send_broadcast(std::uint8_t channel, const std::vector<std::uint8_t>& payload);
    // Returns the framer result (kFramerPass on success)
    int send_acknowledged(std::uint8_t channel, const std::vector<std::uint8_t>& payload,
                          std::uint32_t timeout_ms = 1000);
    bool send_burst_packet(std::uint8_t channel_sequence, const std::vector<std::uint8_t>& payload);

    static unsigned int get_message_buffer_capacity();
    // Throws std::invalid_argument if `data` is longer than an ANT message
    unsigned int get_channel_number(std::uint8_t message_id, const std::vector<std::uint8_t>& data);

private:
    // Runs one blocking ANT SDK call (these wait up to their timeout for the
    // stick's response). operation_mutex_ keeps these calls one at a time
    // across threads and stops close() deleting the framer mid-call.
    template <typename Call>
    auto blocking_call(Call&& call) {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        return call(require_framer());
    }

    DSIFramerANT* require_framer() const;
    void message_pump();

    mutable std::mutex state_mutex_;
    DSISerialGeneric* serial_{nullptr};
    DSIFramerANT* framer_{nullptr};
    bool opened_{false};

    std::mutex operation_mutex_;

    std::mutex pump_mutex_;
    MessageHandler handler_;
    std::thread pump_thread_;
    bool pump_running_{false};
    std::atomic<bool> cancel_{false};
};

}  // namespace antnative

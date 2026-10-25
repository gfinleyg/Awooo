#pragma once

// The sensor-level layer: ANT+ sensors heard on the stick (Sensor, a port of
// the Python ant package's device.py) and the stick's channel set-up and
// message routing (Manager, a port of manager.py). RecoveryManager
// (ant_recovery.hpp) watches a Manager.
//
// The stick and the clock are interfaces so every language can supply its
// own: the C API drives a Stick directly; the Python layer hands in whatever
// `native` object it was given (the real module or the tests' fake) and its
// modules' monotonic()/sleep() (which the tests replace with a fake clock).
//
// Locking: nothing here calls out (stick, clock, log) while holding a lock
// that a caller could be waiting on from the other side of such a call - only
// the configuration lock is held across stick calls, and callers never take it
// from inside a stick call or a message callback. That keeps a scripting
// language's global lock (Python's GIL) from deadlocking against these.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "ant_core.hpp"
#include "ant_profiles.hpp"

namespace antnative {

class ConfigurationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class NotOpenError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using ClockFn = std::function<double()>;
using SleepFn = std::function<void(double)>;
using LogFn = std::function<void(const std::string&)>;

// monotonic seconds
double steady_seconds();
void sleep_seconds(double seconds);

// ---- Transport -----------------------------------------------------------

// What Manager and RecoveryManager need from the stick. The configuration
// calls return false when the stick refused them; the message pump hands
// each message to whatever the transport was set up to deliver them to.
class Transport {
public:
    virtual ~Transport() = default;

    virtual std::vector<UsbDeviceInfo> list_devices() = 0;
    virtual bool open(std::uint32_t baudrate, int device_number) = 0;
    virtual void close() = 0;
    virtual bool is_open() = 0;
    virtual bool reset() = 0;
    virtual bool set_network_key(int network_number, const std::vector<std::uint8_t>& key) = 0;
    virtual bool assign_channel(int channel, int channel_type, int network_number) = 0;
    virtual bool unassign_channel(int channel) = 0;
    virtual bool set_channel_id(int channel, int device_number, int device_type, int transmission_type) = 0;
    virtual bool set_channel_period(int channel, int period) = 0;
    virtual bool set_channel_rf_frequency(int channel, int frequency) = 0;
    virtual bool set_search_timeout(int channel, int timeout) = 0;
    virtual bool set_low_priority_search_timeout(int channel, int timeout) = 0;
    virtual bool set_rx_extended_messages_enabled(bool enabled) = 0;
    virtual bool open_rx_scan_mode() = 0;
    virtual bool open_channel(int channel) = 0;
    virtual bool close_channel(int channel) = 0;
    virtual void start_message_pump() = 0;
    virtual void stop_message_pump() = 0;
    // The framer result (kFramerPass on success)
    virtual int send_acknowledged(int channel, const std::vector<std::uint8_t>& payload, std::uint32_t timeout_ms) = 0;
};

// A Transport over a Stick (the C API's). Messages go to `sink`.
class StickTransport : public Transport {
public:
    StickTransport(std::shared_ptr<Stick> stick, Stick::MessageHandler sink)
        : stick_(std::move(stick)), sink_(std::move(sink)) {}

    std::vector<UsbDeviceInfo> list_devices() override { return Stick::list_devices(); }
    bool open(std::uint32_t baudrate, int device_number) override {
        return stick_->open(baudrate, static_cast<std::uint8_t>(device_number));
    }
    void close() override { stick_->close(); }
    bool is_open() override { return stick_->is_open(); }
    bool reset() override { return stick_->reset(); }
    bool set_network_key(int network_number, const std::vector<std::uint8_t>& key) override {
        return stick_->set_network_key(static_cast<std::uint8_t>(network_number), key);
    }
    bool assign_channel(int channel, int channel_type, int network_number) override {
        return stick_->assign_channel(static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(channel_type),
                                      static_cast<std::uint8_t>(network_number));
    }
    bool unassign_channel(int channel) override { return stick_->unassign_channel(static_cast<std::uint8_t>(channel)); }
    bool set_channel_id(int channel, int device_number, int device_type, int transmission_type) override {
        return stick_->set_channel_id(static_cast<std::uint8_t>(channel), static_cast<std::uint16_t>(device_number),
                                      static_cast<std::uint8_t>(device_type),
                                      static_cast<std::uint8_t>(transmission_type));
    }
    bool set_channel_period(int channel, int period) override {
        return stick_->set_channel_period(static_cast<std::uint8_t>(channel), static_cast<std::uint16_t>(period));
    }
    bool set_channel_rf_frequency(int channel, int frequency) override {
        return stick_->set_channel_rf_frequency(static_cast<std::uint8_t>(channel),
                                                static_cast<std::uint8_t>(frequency));
    }
    bool set_search_timeout(int channel, int timeout) override {
        return stick_->set_search_timeout(static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(timeout));
    }
    bool set_low_priority_search_timeout(int channel, int timeout) override {
        return stick_->set_low_priority_search_timeout(static_cast<std::uint8_t>(channel),
                                                       static_cast<std::uint8_t>(timeout));
    }
    bool set_rx_extended_messages_enabled(bool enabled) override {
        return stick_->set_rx_extended_messages_enabled(enabled);
    }
    bool open_rx_scan_mode() override { return stick_->open_rx_scan_mode(); }
    bool open_channel(int channel) override { return stick_->open_channel(static_cast<std::uint8_t>(channel)); }
    bool close_channel(int channel) override { return stick_->close_channel(static_cast<std::uint8_t>(channel)); }
    void start_message_pump() override { stick_->start_message_pump(sink_); }
    void stop_message_pump() override { stick_->stop_message_pump(); }
    int send_acknowledged(int channel, const std::vector<std::uint8_t>& payload, std::uint32_t timeout_ms) override {
        return stick_->send_acknowledged(static_cast<std::uint8_t>(channel), payload, timeout_ms);
    }

    Stick& stick() { return *stick_; }

private:
    std::shared_ptr<Stick> stick_;
    Stick::MessageHandler sink_;
};

// ---- Sensor (device.py) --------------------------------------------------

// One ANT+ sensor: what's been heard from it, its profile, and the state
// RecoveryManager keeps on it. Every member is read and written under the
// sensor's own lock, so it can be shared between the message pump, the
// watchdog and the caller.
class Sensor {
public:
    Sensor(int device_number, int device_type, int transmission_type, int channel,
           std::shared_ptr<Profile> profile, ClockFn clock, double now);

    int device_number() const { return device_number_; }
    int device_type() const { return device_type_; }
    int transmission_type() const { return transmission_type_; }
    const std::shared_ptr<Profile>& profile() const { return profile_; }

    // Feeds a message from this sensor to its profile. True if it brought new
    // data (a page that isn't a repeat; every message, with no profile).
    bool update(const DecodedMessage& message);

    void mark_error();
    void mark_state(const std::string& state);
    void mark_recovered();
    void mark_stalled();
    void clear_stalled();

    // The weakest battery (a dual-sided power meter reports each side)
    std::optional<BatteryStatus> battery() const;
    std::map<int, BatteryStatus> batteries() const;

    // Every field below, under the lock
    template <typename T>
    T get(T Sensor::*field) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return this->*field;
    }
    template <typename T>
    void set(T Sensor::*field, T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        this->*field = std::move(value);
    }

    int channel{0};
    double last_seen{0};
    std::uint64_t packet_count{0};
    // Set when a message actually changes the decoded data, unlike last_seen
    // (every packet): the gap between them tells a frozen reading from a
    // sensor gone silent
    double last_new_data_seen{0};

    std::string state{"ACTIVE"};
    int error_count{0};
    int recovery_attempts{0};
    int recovery_count{0};
    double last_state_change{0};
    double lost_time{0};
    double stale_time{0};

    // The decoded reading stopped changing although packets still arrive;
    // separate from `state`, which only follows packet silence
    bool data_stalled{false};
    double stalled_since{0};
    // New data cleared data_stalled; RecoveryManager logs it and clears this
    bool data_resumed{false};

private:
    const int device_number_;
    const int device_type_;
    const int transmission_type_;
    const std::shared_ptr<Profile> profile_;
    const ClockFn clock_;
    mutable std::mutex mutex_;
    std::map<int, BatteryStatus> batteries_;
};

// ---- Manager (manager.py) ------------------------------------------------

enum class ChannelMode { None, Scan, Channels };

// (device_type, device_number, transmission_type), transmission type 0
// matching any
using ChannelIdentity = std::tuple<int, int, int>;

struct ScanConfig {
    std::vector<std::uint8_t> network_key;
    int network_number{0};
    int channel{0};
    int rf_frequency{57};
};

class Manager {
public:
    // ANT+ message periods (1/32768 s) per device profile
    static int channel_period(int device_type);

    Manager(std::shared_ptr<Transport> transport, ClockFn clock, SleepFn sleep, ClockFn sensor_clock,
            LogFn on_log = nullptr);

    Transport& transport() { return *transport_; }

    // device_number empty: the first stick that opens
    bool open(std::uint32_t baudrate = 57600, std::optional<int> device_number = std::nullopt);
    void close();
    void start();
    void stop();

    // Throws NotOpenError, ConfigurationError
    void configure_scan(const std::vector<std::uint8_t>& network_key, int network_number = 0, int channel = 0,
                        int rf_frequency = 57);
    // One dedicated slave channel per device, on channels 0, 1, ... in order.
    // Throws ConfigurationError, leaving the stick on the scan channel.
    void open_device_channels(const std::vector<ChannelIdentity>& identities);
    void return_to_scan();
    // Dedicated channels were asked for but the stick is on the scan channel
    // (opening them failed); false while another thread is reconfiguring
    bool wants_device_channels();
    bool retry_device_channels();
    void reopen_channel(int channel);
    void reopen_scan_channel();
    // Rebuilds the whole set-up after a stick reset
    void reapply_configuration();

    std::optional<int> channel_for(int device_type, int device_number);
    std::map<int, ChannelIdentity> channel_devices();
    // Whether a channel event on `channel` (empty = unknown channel) is within
    // the grace after our own reconfiguration
    bool events_suppressed(std::optional<int> channel, std::optional<double> now = std::nullopt);

    struct MessageResult {
        std::shared_ptr<Sensor> sensor;   // empty when the message isn't from a sensor
        bool changed{false};
        bool dropped{false};              // a drop_devices sensor: not passed on
    };
    // Routes one message to the sensor it came from, creating the sensor the
    // first time it's heard
    MessageResult on_message(const DecodedMessage& message);

    std::vector<std::shared_ptr<Sensor>> get_devices();
    std::shared_ptr<Sensor> find_sensor(int device_type, int device_number);

    ChannelMode mode();
    bool needs_reset();
    std::optional<ScanConfig> scan_config();
    std::optional<int> device_number();
    // Test hook: sensors whose messages are dropped before they're counted
    void set_drop_devices(std::set<int> device_numbers);

    void set_log(LogFn on_log);

private:
    void configure_scan_locked(const ScanConfig& config);
    void suppress_events(std::optional<int> channel = std::nullopt);
    void close_all_channels();
    void open_scan_channel();
    void open_slave_channel(int channel, const ChannelIdentity& identity);
    void log(const std::string& message);

    std::shared_ptr<Transport> transport_;
    ClockFn clock_;
    SleepFn sleep_;
    ClockFn sensor_clock_;

    std::mutex log_mutex_;
    LogFn on_log_;

    // Held through every channel (re)configuration, stick calls included
    std::recursive_mutex config_mutex_;

    // Guards the state below; never held across a stick, clock or log call
    std::mutex state_mutex_;
    ChannelMode mode_{ChannelMode::None};
    std::map<int, ChannelIdentity> channel_ids_;
    std::optional<ScanConfig> scan_config_;
    std::optional<std::vector<ChannelIdentity>> wanted_channels_;
    double scan_opened_at_{0};
    bool needs_reset_{false};
    double suppress_events_until_{0};
    std::map<int, double> suppress_channel_until_;
    std::optional<int> device_number_;

    std::mutex devices_mutex_;
    std::map<std::tuple<int, int, int>, std::shared_ptr<Sensor>> devices_;   // (number, type, transmission)
    std::vector<std::shared_ptr<Sensor>> device_order_;
    std::set<int> drop_devices_;
};

const char* mode_name(ChannelMode mode);

}  // namespace antnative

#pragma once

// The sensor watchdog (a port of the Python ant package's recovery.py, now a
// thin layer over this). Each check() moves every watched sensor through
// ACTIVE -> STALE -> LOST on packet silence (and back), flags readings that
// froze while packets still arrive, and escalates when sensors are lost:
//
//   dedicated channels - reopen the sensor's channel once, then reset the
//                        stick once if it's still silent and no other watched
//                        sensor is being heard
//   scan channel       - channel retries, then a stick reset
//
// check() runs on the watchdog's own thread (start()) or wherever the caller
// calls it. notify_channel_event() may be called from any thread, the message
// pump's included: it only records the event for the next check().

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ant_manager.hpp"

namespace antnative {

struct RecoveryOptions {
    double stale_timeout{1.5};
    double lost_timeout{3};
    double interval{1};
    int channel_retry_limit{2};
    double channel_retry_cooldown{5};
    double device_reset_timeout{7};
    double device_reset_cooldown{7};
    double channel_reopen_wait{5};
    double channel_stick_reset_cooldown{30};
    double channel_open_retry_interval{10};
    double hr_frozen_timeout{10};
    double value_frozen_timeout{6};
};

class Recovery {
public:
    // One dedicated channel's loss episode
    struct ChannelEpisode {
        double reopened{0};
        bool stick_reset{false};
    };

    Recovery(Manager& manager, RecoveryOptions options, ClockFn clock, LogFn on_event = nullptr);
    ~Recovery();
    Recovery(const Recovery&) = delete;
    Recovery& operator=(const Recovery&) = delete;

    void start();
    // Stops the watchdog thread and waits for it, unless called from it
    void stop();
    bool running() const;

    void check();
    void notify_channel_event(const std::string& event_name, std::optional<int> channel);
    // (device_type, device_number) pairs to watch, or empty for every sensor
    void set_watched_devices(std::optional<std::set<std::pair<int, int>>> identities);
    std::optional<std::set<std::pair<int, int>>> watched_devices();

    void enable_escalation();
    void disable_escalation();
    bool escalation_enabled();

    std::map<std::string, long long> report();
    std::map<int, ChannelEpisode> channel_reopens();

    RecoveryOptions options();
    void set_options(const RecoveryOptions& options);
    void set_log(LogFn on_event);

    static bool actionable_event(const std::string& event_name);

private:
    void run();
    bool is_watched(const Sensor& sensor);
    void check_data_freshness(Sensor& sensor, double now);
    bool frozen_value_is_suspicious(Sensor& sensor);
    void log_data_recovered(Sensor& sensor);
    void handle_escalation();
    void handle_channel_escalation(double now, const std::set<std::pair<std::string, std::optional<int>>>& pending);
    void attempt_channel_retry(const std::vector<std::shared_ptr<Sensor>>& lost, const std::string& reason);
    bool attempt_device_reset(const std::vector<std::shared_ptr<Sensor>>& lost, const std::string& reason);
    bool retry_device_channels();
    void reopen_channel(int channel, const std::string& reason);
    void count(const char* stat);
    void log(const std::string& message);

    Manager& manager_;
    ClockFn clock_;

    std::mutex options_mutex_;
    RecoveryOptions options_;

    std::mutex log_mutex_;
    LogFn on_event_;

    // The watchdog thread
    std::mutex run_mutex_;
    std::condition_variable wake_;
    bool wake_requested_{false};
    bool stop_requested_{false};
    bool running_{false};
    std::thread thread_;

    // Only one escalation at a time (the watchdog thread and a check() called
    // directly can overlap)
    std::mutex escalation_mutex_;

    // Guards the bookkeeping below; never held across a manager, clock or log call
    std::mutex state_mutex_;
    bool escalation_enabled_{true};
    std::optional<double> lost_since_;
    int channel_retry_count_{0};
    double last_channel_retry_{0};
    double last_device_reset_{0};
    double last_channel_open_retry_{0};
    std::optional<std::set<std::pair<int, int>>> watched_;
    std::set<std::pair<std::string, std::optional<int>>> pending_events_;
    std::map<int, ChannelEpisode> channel_reopens_;
    std::map<std::string, long long> stats_;
};

}  // namespace antnative

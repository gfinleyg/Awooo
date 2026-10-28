#include "ant_recovery.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>

namespace antnative {

namespace {

const char* const kStatNames[] = {
    "checks", "stale", "lost", "recoveries",
    "channel_retries", "channel_retry_failures", "device_resets", "device_reset_failures",
    "channel_open_retries", "channel_open_retry_failures", "stalled", "stalled_recovered",
};

// RESPONSE_EVENT names worth reacting to sooner than the silence timer would
// notice. EVENT_SERIAL_QUE_OVERFLOW/EVENT_QUE_OVERFLOW are deliberately not
// here: a queue that briefly filled up (it does right as several devices are
// discovered at once) isn't a broken link, and reopening the channel only
// adds churn.
bool urgent_retry_event(const std::string& name) {
    return name == "EVENT_RX_FAIL_GO_TO_SEARCH" || name == "EVENT_RX_SEARCH_TIMEOUT";
}

bool urgent_reset_event(const std::string& name) {
    return name == "EVENT_CHANNEL_CLOSED";
}

// Dedicated channels: a channel that closes on its own gets reopened.
// EVENT_RX_FAIL_GO_TO_SEARCH is routine there (the channel lost its device
// and is searching again by itself), so it's never acted on in that mode.
bool channel_closed_event(const std::string& name) {
    return name == "EVENT_CHANNEL_CLOSED" || name == "EVENT_RX_SEARCH_TIMEOUT";
}

std::string fixed(double value, int decimals) {
    char text[64];
    std::snprintf(text, sizeof text, "%.*f", decimals, value);
    return text;
}

// A number as Python prints an int or a float that holds one: 5 or 5.5
std::string number(double value) {
    char text[64];
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(value));
    } else {
        std::snprintf(text, sizeof text, "%g", value);
    }
    return text;
}

// A list of device numbers as Python prints it: [100, 200]
std::string id_list(const std::vector<int>& ids) {
    std::string text = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) text += ", ";
        text += std::to_string(ids[i]);
    }
    return text + "]";
}

std::vector<int> numbers_of(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    std::vector<int> ids;
    for (const auto& sensor : sensors) {
        ids.push_back(sensor->device_number());
    }
    return ids;
}

std::string sensor_text(const Sensor& sensor) {
    return "id=" + std::to_string(sensor.device_number()) + " type=" + std::to_string(sensor.device_type());
}

}  // namespace

bool Recovery::actionable_event(const std::string& event_name) {
    return urgent_retry_event(event_name) || urgent_reset_event(event_name);
}

Recovery::Recovery(Manager& manager, RecoveryOptions options, ClockFn clock, LogFn on_event)
    : manager_(manager), clock_(std::move(clock)), options_(options), on_event_(std::move(on_event)) {
    for (const char* name : kStatNames) {
        stats_[name] = 0;
    }
}

Recovery::~Recovery() {
    stop();
}

// ---- The watchdog thread ---------------------------------------------------

void Recovery::start() {
    std::lock_guard<std::mutex> lock(run_mutex_);
    if (running_) {
        return;
    }
    if (thread_.joinable()) {
        // stopped from its own thread earlier
        thread_.detach();
    }
    running_ = true;
    stop_requested_ = false;
    wake_requested_ = false;
    thread_ = std::thread(&Recovery::run, this);
}

void Recovery::stop() {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        running_ = false;
        stop_requested_ = true;
        wake_requested_ = true;
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
            thread = std::move(thread_);
        }
    }
    wake_.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

bool Recovery::running() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(run_mutex_));
    return running_;
}

void Recovery::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(run_mutex_);
            const double interval = options().interval;
            // Sleeps `interval` unless notify_channel_event() wakes it, so an
            // urgent channel event is acted on right away
            wake_.wait_for(lock, std::chrono::duration<double>(interval),
                           [this] { return wake_requested_ || stop_requested_; });
            if (stop_requested_) {
                return;
            }
            wake_requested_ = false;
        }
        try {
            check();
        } catch (const std::exception& error) {
            log(std::string("RECOVERY internal error: ") + error.what());
        } catch (...) {
            log("RECOVERY internal error");
        }
    }
}

void Recovery::notify_channel_event(const std::string& event_name, std::optional<int> channel) {
    if (!actionable_event(event_name)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_events_.insert({event_name, channel});
    }
    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        wake_requested_ = true;
    }
    wake_.notify_all();
}

// ---- Settings and state ----------------------------------------------------

void Recovery::set_watched_devices(std::optional<std::set<std::pair<int, int>>> identities) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    watched_ = std::move(identities);
    // A new selection reuses channel numbers 0, 1, ... for different devices -
    // don't carry a loss episode over to them
    channel_reopens_.clear();
}

std::optional<std::set<std::pair<int, int>>> Recovery::watched_devices() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return watched_;
}

bool Recovery::is_watched(const Sensor& sensor) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return !watched_ || watched_->count({sensor.device_type(), sensor.device_number()}) > 0;
}

void Recovery::enable_escalation() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    escalation_enabled_ = true;
}

void Recovery::disable_escalation() {
    // Only suppresses new attempts; the cooldown/attempt bookkeeping is kept,
    // so re-enabling resumes where it was
    std::lock_guard<std::mutex> lock(state_mutex_);
    escalation_enabled_ = false;
}

bool Recovery::escalation_enabled() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return escalation_enabled_;
}

std::map<std::string, long long> Recovery::report() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return stats_;
}

std::map<int, Recovery::ChannelEpisode> Recovery::channel_reopens() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return channel_reopens_;
}

RecoveryOptions Recovery::options() {
    std::lock_guard<std::mutex> lock(options_mutex_);
    return options_;
}

void Recovery::set_options(const RecoveryOptions& options) {
    std::lock_guard<std::mutex> lock(options_mutex_);
    options_ = options;
}

void Recovery::set_log(LogFn on_event) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    on_event_ = std::move(on_event);
}

void Recovery::count(const char* stat) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++stats_[stat];
}

void Recovery::log(const std::string& message) {
    LogFn on_event;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        on_event = on_event_;
    }
    if (on_event) {
        try {
            on_event(message);
        } catch (...) {
            // a caller's logging never breaks recovery
        }
    }
}

// ---- check() ---------------------------------------------------------------

void Recovery::check() {
    const double now = clock_();
    const RecoveryOptions opts = options();
    count("checks");

    for (const std::shared_ptr<Sensor>& sensor : manager_.get_devices()) {
        // Not part of this session (or nothing selected yet): never enters
        // STALE/LOST and never drives escalation - a bystander device going
        // quiet isn't this session's problem
        if (!is_watched(*sensor)) {
            continue;
        }
        const double age = now - sensor->get(&Sensor::last_seen);
        const std::string state = sensor->get(&Sensor::state);

        if (age < opts.stale_timeout) {
            if (state == "STALE" || state == "LOST") {
                count("recoveries");
                log("DEVICE_RECOVERED " + sensor_text(*sensor) + " age=" + fixed(age, 2) + "s");
            }
            sensor->mark_state("ACTIVE");
        } else if (age < opts.lost_timeout) {
            if (state != "STALE") {
                count("stale");
                sensor->set(&Sensor::stale_time, now);
                log("DEVICE_STALE " + sensor_text(*sensor) + " silent=" + fixed(age, 2) + "s");
            }
            sensor->mark_state("STALE");
        } else {
            if (state != "LOST") {
                count("lost");
                sensor->set(&Sensor::lost_time, now);
                log("DEVICE_LOST " + sensor_text(*sensor) + " silent=" + fixed(age, 2) + "s packets=" +
                    std::to_string(sensor->get(&Sensor::packet_count)));
            }
            sensor->mark_state("LOST");
        }

        // The data plausibility check only means something while the link
        // itself is fine (a STALE/LOST sensor is already flagged by its state)
        if (sensor->get(&Sensor::state) == "ACTIVE") {
            check_data_freshness(*sensor, now);
        } else {
            sensor->clear_stalled();
        }
    }

    handle_escalation();
}

void Recovery::check_data_freshness(Sensor& sensor, double now) {
    const RecoveryOptions opts = options();
    // A stalled reading that moved again was cleared by Sensor::update() the
    // moment it did - reported here
    if (sensor.get(&Sensor::data_resumed)) {
        sensor.set(&Sensor::data_resumed, false);
        log_data_recovered(sensor);
    }

    const double data_age = now - sensor.get(&Sensor::last_new_data_seen);
    const double timeout =
        sensor.device_type() == kDeviceTypeHeartRate ? opts.hr_frozen_timeout : opts.value_frozen_timeout;

    if (data_age >= timeout && frozen_value_is_suspicious(sensor, now)) {
        if (!sensor.get(&Sensor::data_stalled)) {
            count("stalled");
            log("DEVICE_DATA_STALLED " + sensor_text(sensor) + " frozen=" + fixed(data_age, 1) +
                "s - sensor is still transmitting but its reading hasn't changed; "
                "check battery/contact (no channel action taken)");
        }
        sensor.mark_stalled();
    } else if (sensor.get(&Sensor::data_stalled)) {
        log_data_recovered(sensor);
        sensor.clear_stalled();
    }
}

void Recovery::log_data_recovered(Sensor& sensor) {
    count("stalled_recovered");
    log("DEVICE_DATA_RECOVERED " + sensor_text(sensor));
}

bool Recovery::frozen_value_is_suspicious(Sensor& sensor, double now) {
    const std::shared_ptr<Profile>& profile = sensor.profile();
    switch (sensor.device_type()) {
        case kDeviceTypeHeartRate:
            // Beat count/event time advance every heartbeat even at a slow
            // resting rate: a page that hasn't changed at all this long isn't
            // physiologically possible, whatever the bpm says
            return true;
        case kDeviceTypePower: {
            auto* power = dynamic_cast<PowerMeterProfile*>(profile.get());
            if (!power) return false;
            std::lock_guard<std::mutex> lock(power->mutex);
            return power->latest && power->latest->instantaneous_power_watts != 0;
        }
        case kDeviceTypeSpeedCadence:
        case kDeviceTypeCadence:
        case kDeviceTypeSpeed:
            // Stopped cranks and wheel sound exactly like a stuck sensor: the
            // last event repeated, its last rate still non-zero. Only a power
            // meter or trainer saying the rider is pedaling tells them apart
            // (and without one there's no fallback reading to switch to anyway)
            return rider_pedaling(now);
        default:
            // e.g. a trainer: no validated field to judge frozen-at-zero by yet
            return false;
    }
}

bool Recovery::rider_pedaling(double now) {
    // Changing well within the frozen timeout, so clearly more recently than
    // the sensor it vouches for - not one that stopped a message after it and
    // holds its last values (its own freshness rather than its data_stalled
    // flag, which it may not have been given yet this check)
    const double fresh = options().value_frozen_timeout / 2;
    for (const std::shared_ptr<Sensor>& sensor : manager_.get_devices()) {
        if (!is_watched(*sensor) || sensor->get(&Sensor::state) != "ACTIVE" ||
            now - sensor->get(&Sensor::last_new_data_seen) >= fresh) {
            continue;
        }
        const std::shared_ptr<Profile>& profile = sensor->profile();
        int watts = 0;
        int cadence = 0;
        if (auto* power = dynamic_cast<PowerMeterProfile*>(profile.get())) {
            std::lock_guard<std::mutex> lock(power->mutex);
            if (power->latest) {
                watts = power->latest->instantaneous_power_watts;
                cadence = power->latest->power_meter_cadence_rpm.value_or(0);
            }
        } else if (auto* trainer = dynamic_cast<FitnessEquipmentProfile*>(profile.get())) {
            std::lock_guard<std::mutex> lock(trainer->mutex);
            if (trainer->latest) {
                watts = trainer->latest->instantaneous_power_watts;
                cadence = trainer->latest->power_meter_cadence_rpm.value_or(0);
            }
        }
        if (watts > 0 || cadence > 0) {
            return true;
        }
    }
    return false;
}

// ---- Escalation --------------------------------------------------------------

void Recovery::handle_escalation() {
    if (!escalation_enabled()) {
        return;
    }
    // Only one escalation attempt in flight at a time
    std::unique_lock<std::mutex> escalating(escalation_mutex_, std::try_to_lock);
    if (!escalating.owns_lock()) {
        return;
    }

    const RecoveryOptions opts = options();
    const double now = clock_();
    std::set<std::pair<std::string, std::optional<int>>> pending;
    double last_device_reset;
    double last_channel_open_retry;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending.swap(pending_events_);
        last_device_reset = last_device_reset_;
        last_channel_open_retry = last_channel_open_retry_;
    }

    // A stick that refused a channel change and the fall back to the scan
    // channel takes no channel changes at all until it's reset - a reset
    // (which reapplies them) is the only thing that helps
    if (manager_.needs_reset() && now - last_device_reset >= opts.device_reset_cooldown) {
        attempt_device_reset({}, "the stick refused a channel change");
        return;
    }

    if (manager_.wants_device_channels() && now - last_channel_open_retry >= opts.channel_open_retry_interval &&
        retry_device_channels()) {
        return;
    }

    if (manager_.mode() == ChannelMode::Channels) {
        handle_channel_escalation(now, pending);
        return;
    }

    // Our own channel closes come right back as EVENT_CHANNEL_CLOSED - only
    // one outside the manager's reconfiguration grace for its channel is
    // unexpected
    bool urgent_reset = false;
    bool urgent_retry = false;
    for (const auto& [name, channel] : pending) {
        if (urgent_reset_event(name) && !manager_.events_suppressed(channel, now)) {
            urgent_reset = true;
        }
        if (urgent_retry_event(name)) {
            urgent_retry = true;
        }
    }

    // Watched sensors only, as in check(): a sensor that was LOST when the
    // selection changed would otherwise stay LOST forever and keep driving
    // retries and stick resets
    std::vector<std::shared_ptr<Sensor>> lost;
    for (const auto& sensor : manager_.get_devices()) {
        if (sensor->get(&Sensor::state) == "LOST" && is_watched(*sensor)) {
            lost.push_back(sensor);
        }
    }

    double time_lost;
    int retry_count;
    double last_channel_retry;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (lost.empty() && !urgent_reset && !urgent_retry) {
            lost_since_.reset();
            channel_retry_count_ = 0;
            return;
        }
        if (!lost_since_) {
            lost_since_ = now;
        }
        time_lost = now - *lost_since_;
        retry_count = channel_retry_count_;
        last_channel_retry = last_channel_retry_;
    }

    // An unexpected EVENT_CHANNEL_CLOSED means the channel is actually closed,
    // so a channel retry doesn't apply: straight to the stick-level path,
    // still gated by device_reset_cooldown so a flapping channel can't
    // reset-storm the stick
    if (urgent_reset && now - last_device_reset >= opts.device_reset_cooldown) {
        attempt_device_reset(lost, "EVENT_CHANNEL_CLOSED");
        return;
    }

    // Tier 1: channel retry, for lost sensors or an urgent event, throttled
    // by the same limit and cooldown either way
    if ((!lost.empty() || urgent_retry) && retry_count < opts.channel_retry_limit &&
        now - last_channel_retry >= opts.channel_retry_cooldown) {
        attempt_channel_retry(lost, !lost.empty() ? "silence timeout" : "urgent event");
        return;
    }

    // Tier 2: stick reset, once channel retries are exhausted and sensors have
    // stayed LOST past device_reset_timeout
    if (!lost.empty() && time_lost >= opts.device_reset_timeout &&
        now - last_device_reset >= opts.device_reset_cooldown) {
        attempt_device_reset(lost, "timeout");
    }
}

void Recovery::attempt_channel_retry(const std::vector<std::shared_ptr<Sensor>>& lost, const std::string& reason) {
    int retry_count;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        retry_count = ++channel_retry_count_;
    }
    const double now = clock_();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_channel_retry_ = now;
    }
    count("channel_retries");
    log("RECOVERY channel retry #" + std::to_string(retry_count) + "/" +
        std::to_string(options().channel_retry_limit) + " (reason=" + reason + ", lost=" + id_list(numbers_of(lost)) +
        ")");
    try {
        // Our own close comes back as EVENT_CHANNEL_CLOSED; the manager's
        // reconfiguration grace covers it, so it can't trigger a stick reset
        manager_.reopen_scan_channel();
        log("RECOVERY channel retry succeeded, channel reopened");
    } catch (const std::exception& error) {
        count("channel_retry_failures");
        log(std::string("RECOVERY channel retry failed: ") + error.what());
    }
}

bool Recovery::attempt_device_reset(const std::vector<std::shared_ptr<Sensor>>& lost, const std::string& reason) {
    const double now = clock_();
    std::optional<double> lost_since;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_device_reset_ = now;
        lost_since = lost_since_;
    }
    count("device_resets");
    const std::string ids = id_list(numbers_of(lost));

    if (reason == "timeout") {
        std::string after = "None";
        if (lost_since) {
            after = *lost_since == 0 ? "0.0" : fixed(clock_() - *lost_since, 1);
        }
        log("RECOVERY channel retries exhausted after " + after + "s, escalating to device-level reset (lost=" + ids +
            ")");
    } else {
        log("RECOVERY escalating to device-level reset (reason=" + reason + ", lost=" + ids + ")");
    }

    if (!manager_.scan_config()) {
        count("device_reset_failures");
        log("RECOVERY device-level reset skipped: no prior scan configuration to reapply");
        return false;
    }

    try {
        manager_.transport().stop_message_pump();
        // Resets the stick (the first step of the scan set-up) and, since a
        // reset clears the network key and channel assignment, reapplies the
        // whole set-up: the scan channel, or every dedicated channel
        manager_.reapply_configuration();
        manager_.transport().start_message_pump();
        {
            // A fresh recovery window rather than re-escalating right away
            std::lock_guard<std::mutex> lock(state_mutex_);
            channel_retry_count_ = 0;
            lost_since_.reset();
        }
        log("RECOVERY device-level reset complete, channel configuration reapplied");
        return true;
    } catch (const std::exception& error) {
        count("device_reset_failures");
        log(std::string("RECOVERY device-level reset FAILED: ") + error.what());
        return false;
    }
}

bool Recovery::retry_device_channels() {
    const double now = clock_();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_channel_open_retry_ = now;
    }
    count("channel_open_retries");
    log("RECOVERY retrying dedicated channel open (stuck on the scan channel)");
    try {
        if (!manager_.retry_device_channels()) {
            return false;
        }
    } catch (const std::exception& error) {
        count("channel_open_retry_failures");
        log(std::string("RECOVERY dedicated channel open failed again: ") + error.what());
        return false;
    }
    {
        // Scan-mode escalation bookkeeping doesn't carry over
        std::lock_guard<std::mutex> lock(state_mutex_);
        lost_since_.reset();
        channel_retry_count_ = 0;
        channel_reopens_.clear();
    }
    log("RECOVERY dedicated channels opened");
    return true;
}

void Recovery::handle_channel_escalation(double now,
                                         const std::set<std::pair<std::string, std::optional<int>>>& pending) {
    const RecoveryOptions opts = options();
    std::vector<std::shared_ptr<Sensor>> sensors;
    for (const auto& sensor : manager_.get_devices()) {
        if (is_watched(*sensor)) {
            sensors.push_back(sensor);
        }
    }

    // Channels the stick closed on its own - ignored right after our own
    // reconfiguration of that channel, which closes it deliberately
    std::set<int> closed;
    for (const auto& [name, channel] : pending) {
        if (channel_closed_event(name) && channel && !manager_.events_suppressed(channel, now)) {
            closed.insert(*channel);
        }
    }
    for (int channel : closed) {
        reopen_channel(channel, "channel closed");
    }

    std::vector<std::shared_ptr<Sensor>> lost;
    for (const auto& sensor : sensors) {
        if (sensor->get(&Sensor::state) == "LOST") {
            lost.push_back(sensor);
        }
    }

    std::optional<int> reset_channel;
    for (const auto& sensor : sensors) {
        const std::optional<int> channel = manager_.channel_for(sensor->device_type(), sensor->device_number());
        if (!channel) {
            continue;
        }
        // Heard again: the loss episode is over
        if (sensor->get(&Sensor::state) != "LOST") {
            std::lock_guard<std::mutex> lock(state_mutex_);
            channel_reopens_.erase(*channel);
            continue;
        }
        std::optional<ChannelEpisode> episode;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto found = channel_reopens_.find(*channel);
            if (found == channel_reopens_.end()) {
                channel_reopens_[*channel] = ChannelEpisode{now, false};
            } else {
                episode = found->second;
            }
        }
        if (!episode) {
            reopen_channel(*channel, "silent " + fixed(now - sensor->get(&Sensor::last_seen), 1) + "s, device=" +
                                         std::to_string(sensor->device_number()) +
                                         " type=" + std::to_string(sensor->device_type()));
            continue;
        }
        if (!episode->stick_reset && now - episode->reopened >= opts.channel_reopen_wait) {
            reset_channel = *channel;
        }
    }

    if (!reset_channel) {
        return;
    }

    // Another watched sensor on its own channel still coming through: the
    // stick is fine, and a reset would only cut off the sensors that are
    // (an FE-C trainer's control included). The lost sensors' episodes are
    // closed off as if reset, so this is decided (and logged) once per episode.
    std::vector<std::shared_ptr<Sensor>> heard;
    for (const auto& sensor : sensors) {
        if (sensor->get(&Sensor::state) == "ACTIVE" &&
            manager_.channel_for(sensor->device_type(), sensor->device_number())) {
            heard.push_back(sensor);
        }
    }

    if (!heard.empty()) {
        std::vector<int> waiting;
        for (const auto& sensor : lost) {
            const std::optional<int> channel = manager_.channel_for(sensor->device_type(), sensor->device_number());
            if (!channel) continue;
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto found = channel_reopens_.find(*channel);
            if (found != channel_reopens_.end() && !found->second.stick_reset) {
                found->second.stick_reset = true;
                waiting.push_back(sensor->device_number());
            }
        }
        log("RECOVERY not resetting the stick for lost=" + id_list(waiting) + ": still hearing " +
            id_list(numbers_of(heard)) + ", so the stick is working - leaving the channel searching");
        return;
    }

    double last_device_reset;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_device_reset = last_device_reset_;
    }
    if (now - last_device_reset < opts.channel_stick_reset_cooldown) {
        return;
    }

    if (attempt_device_reset(lost, "channel " + std::to_string(*reset_channel) + " still silent " +
                                       number(opts.channel_reopen_wait) + "s after reopen")) {
        // The reset reopened every channel, so it counts as the one stick reset
        // for every channel that was down. A failed reset isn't counted; it's
        // retried after channel_stick_reset_cooldown.
        for (const auto& sensor : lost) {
            const std::optional<int> channel = manager_.channel_for(sensor->device_type(), sensor->device_number());
            if (!channel) continue;
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto found = channel_reopens_.find(*channel);
            if (found != channel_reopens_.end()) {
                found->second.stick_reset = true;
            }
        }
    }
}

void Recovery::reopen_channel(int channel, const std::string& reason) {
    count("channel_retries");
    log("RECOVERY reopening channel " + std::to_string(channel) + " (" + reason + ")");
    try {
        manager_.reopen_channel(channel);
    } catch (const std::exception& error) {
        count("channel_retry_failures");
        log("RECOVERY channel " + std::to_string(channel) + " reopen failed: " + error.what());
    }
}

}  // namespace antnative

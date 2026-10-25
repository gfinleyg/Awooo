#include "ant_manager.hpp"

#include <chrono>
#include <exception>
#include <thread>

namespace antnative {

namespace {

constexpr int kChannelTypeScan = 1;
constexpr int kChannelTypeSlave = 0x00;
constexpr int kDefaultChannelPeriod = 8192;

// Search timeouts, 2.5 s units. A dedicated channel whose device drops out
// returns to searching on its own; searching forever (0xFF) in low priority
// keeps the stick from closing the channel and doesn't disturb the other
// channels' reception the way a high priority search would.
constexpr int kLowPrioritySearchForever = 0xFF;
constexpr int kHighPrioritySearchOff = 0x00;
constexpr int kSearchForever = 0xFF;

// Our own channel closes (switching modes, reopening a channel) make the
// stick report EVENT_CHANNEL_CLOSED right back; RecoveryManager ignores those
// for this long after a reconfiguration (see events_suppressed())
constexpr double kReconfigureEventGraceS = 3.0;

// Closing the scan channel right after opening it (both within the same
// millisecond, as a full reapply after a stick reset used to do) leaves the
// stick refusing every later channel change until it's reset again. Seen on
// an ANT USBStick2: 0 ms fails every time, 10 ms already works; this leaves a
// wide margin.
constexpr double kScanMinOpenS = 0.1;

std::string identity_text(const ChannelIdentity& identity) {
    return "device type=" + std::to_string(std::get<0>(identity)) + " number=" +
           std::to_string(std::get<1>(identity));
}

}  // namespace

double steady_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void sleep_seconds(double seconds) {
    if (seconds > 0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }
}

const char* mode_name(ChannelMode mode) {
    switch (mode) {
        case ChannelMode::Scan: return "scan";
        case ChannelMode::Channels: return "channels";
        default: return nullptr;
    }
}

// ---- Sensor ----------------------------------------------------------------

Sensor::Sensor(int device_number, int device_type, int transmission_type, int channel,
               std::shared_ptr<Profile> profile, ClockFn clock, double now)
    : channel(channel),
      last_seen(now),
      last_new_data_seen(now),
      last_state_change(now),
      device_number_(device_number),
      device_type_(device_type),
      transmission_type_(transmission_type),
      profile_(std::move(profile)),
      clock_(std::move(clock)) {}

bool Sensor::update(const DecodedMessage& message) {
    const double now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);

    // Every packet refreshes the watchdog
    last_seen = now;
    ++packet_count;

    if (std::optional<BatteryStatus> battery =
            decode_battery(device_type_, message.payload ? &*message.payload : nullptr)) {
        batteries_[battery->battery_id] = *battery;
    }

    if (profile_) {
        bool changed;
        {
            std::lock_guard<std::mutex> profile_lock(profile_->mutex);
            const int before = profile_->pages_received;
            profile_->on_message(message);
            changed = profile_->pages_received != before;
        }
        if (changed) {
            // Genuinely new decoded data: also what clears a stalled reading
            last_new_data_seen = now;
            if (data_stalled) {
                data_resumed = true;
                data_stalled = false;
                stalled_since = 0;
            }
        }
        return changed;
    }

    // No profile: nothing to compare payloads against, so every packet
    // counts as new data
    last_new_data_seen = now;
    return true;
}

void Sensor::mark_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++error_count;
}

void Sensor::mark_state(const std::string& new_state) {
    const double now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    if (state != new_state) {
        state = new_state;
        last_state_change = now;
    }
}

void Sensor::mark_recovered() {
    const double now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    state = "ACTIVE";
    ++recovery_count;
    last_state_change = now;
    lost_time = 0;
    stale_time = 0;
}

void Sensor::mark_stalled() {
    const double now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data_stalled) {
        data_stalled = true;
        stalled_since = now;
    }
}

void Sensor::clear_stalled() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_stalled) {
        data_stalled = false;
        stalled_since = 0;
    }
}

std::optional<BatteryStatus> Sensor::battery() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<BatteryStatus> weakest;
    for (const auto& entry : batteries_) {
        // (the first of equally weak ones, as Python's max() picks)
        if (!weakest || entry.second.severity() > weakest->severity()) {
            weakest = entry.second;
        }
    }
    return weakest;
}

std::map<int, BatteryStatus> Sensor::batteries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return batteries_;
}

// ---- Manager ---------------------------------------------------------------

int Manager::channel_period(int device_type) {
    switch (device_type) {
        case kDeviceTypeHeartRate: return 8070;
        case kDeviceTypePower: return 8182;
        case kDeviceTypeFitnessEquipment: return 8192;
        case kDeviceTypeSpeedCadence: return 8086;
        case kDeviceTypeCadence: return 8102;
        case kDeviceTypeSpeed: return 8118;
        default: return kDefaultChannelPeriod;
    }
}

Manager::Manager(std::shared_ptr<Transport> transport, ClockFn clock, SleepFn sleep, ClockFn sensor_clock,
                 LogFn on_log)
    : transport_(std::move(transport)),
      clock_(std::move(clock)),
      sleep_(std::move(sleep)),
      sensor_clock_(std::move(sensor_clock)),
      on_log_(std::move(on_log)) {}

void Manager::set_log(LogFn on_log) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    on_log_ = std::move(on_log);
}

void Manager::log(const std::string& message) {
    LogFn on_log;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        on_log = on_log_;
    }
    if (on_log) {
        try {
            on_log(message);
        } catch (...) {
            // a caller's logging never breaks the manager
        }
    }
}

bool Manager::open(std::uint32_t baudrate, std::optional<int> device_number) {
    if (device_number) {
        const bool result = transport_->open(baudrate, *device_number);
        if (result) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            device_number_ = device_number;
        }
        return result;
    }

    // The first stick that opens
    for (const UsbDeviceInfo& device : transport_->list_devices()) {
        const int index = static_cast<int>(device.index);
        log("Trying ANT device " + std::to_string(index) + ": " +
            device.product_description.value_or("None"));
        if (transport_->open(baudrate, index)) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                device_number_ = index;
            }
            log("Selected ANT device " + std::to_string(index));
            return true;
        }
    }
    return false;
}

void Manager::close() {
    stop();
    transport_->close();
}

void Manager::start() {
    transport_->start_message_pump();
}

void Manager::stop() {
    transport_->stop_message_pump();
}

void Manager::configure_scan(const std::vector<std::uint8_t>& network_key, int network_number, int channel,
                             int rf_frequency) {
    if (!transport_->is_open()) {
        throw NotOpenError("ANT device is not open");
    }
    std::lock_guard<std::recursive_mutex> config(config_mutex_);
    configure_scan_locked(ScanConfig{network_key, network_number, channel, rf_frequency});
}

void Manager::configure_scan_locked(const ScanConfig& config) {
    const bool ok = transport_->reset() &&
                    transport_->set_network_key(config.network_number, config.network_key) &&
                    transport_->assign_channel(config.channel, kChannelTypeScan, config.network_number) &&
                    transport_->set_channel_id(config.channel, 0, 0, 0) &&
                    transport_->set_channel_rf_frequency(config.channel, config.rf_frequency) &&
                    transport_->set_rx_extended_messages_enabled(true) &&
                    transport_->open_rx_scan_mode();
    if (!ok) {
        throw ConfigurationError("ANT scan configuration failed");
    }
    const double now = clock_();
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Kept to reopen the same channel on a retry, or to reapply it all after
    // a stick reset (which wipes the network key and channel assignment)
    scan_config_ = config;
    mode_ = ChannelMode::Scan;
    channel_ids_.clear();
    scan_opened_at_ = now;
    needs_reset_ = false;
}

void Manager::open_device_channels(const std::vector<ChannelIdentity>& identities) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!scan_config_) {
            throw ConfigurationError("open_device_channels() needs configure_scan() first");
        }
    }

    std::lock_guard<std::recursive_mutex> config(config_mutex_);
    ChannelMode mode;
    double opened_at;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        wanted_channels_ = identities;
        mode = mode_;
        opened_at = scan_opened_at_;
    }

    if (mode == ChannelMode::Scan) {
        const double wait = kScanMinOpenS - (clock_() - opened_at);
        if (wait > 0) {
            sleep_(wait);
        }
    }

    suppress_events();
    close_all_channels();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mode_ = ChannelMode::Channels;
    }

    try {
        for (std::size_t channel = 0; channel < identities.size(); ++channel) {
            open_slave_channel(static_cast<int>(channel), identities[channel]);
            std::lock_guard<std::mutex> lock(state_mutex_);
            channel_ids_[static_cast<int>(channel)] = identities[channel];
        }
    } catch (const std::exception& error) {
        const std::exception_ptr original = std::current_exception();
        const std::string first = error.what();
        close_all_channels();
        try {
            open_scan_channel();
        } catch (const ConfigurationError& scan_error) {
            // The stick won't take any channel change now - left looking like
            // scan mode (so the dedicated channels are still wanted, see
            // wants_device_channels()), flagged for RecoveryManager to reset it
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                mode_ = ChannelMode::Scan;
                channel_ids_.clear();
                needs_reset_ = true;
            }
            throw ConfigurationError(first + "; falling back to the scan channel also failed (" +
                                     scan_error.what() + "), the stick needs a reset");
        }
        std::rethrow_exception(original);
    }
}

void Manager::return_to_scan() {
    std::lock_guard<std::recursive_mutex> config(config_mutex_);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        wanted_channels_.reset();
        if (mode_ != ChannelMode::Channels) {
            return;
        }
    }
    suppress_events();
    close_all_channels();
    try {
        open_scan_channel();
    } catch (const ConfigurationError&) {
        // Same as a failed fall back in open_device_channels()
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mode_ = ChannelMode::Scan;
            channel_ids_.clear();
            needs_reset_ = true;
        }
        throw;
    }
}

bool Manager::wants_device_channels() {
    std::unique_lock<std::recursive_mutex> config(config_mutex_, std::try_to_lock);
    if (!config.owns_lock()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    return mode_ == ChannelMode::Scan && wanted_channels_.has_value();
}

bool Manager::retry_device_channels() {
    std::lock_guard<std::recursive_mutex> config(config_mutex_);
    // Re-checked under the lock - a disconnect (return_to_scan) on another
    // thread may have landed since the caller looked
    if (!wants_device_channels()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return mode_ == ChannelMode::Channels;
    }
    std::vector<ChannelIdentity> wanted;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        wanted = *wanted_channels_;
    }
    open_device_channels(wanted);
    return true;
}

void Manager::reopen_channel(int channel) {
    std::lock_guard<std::recursive_mutex> config(config_mutex_);
    ChannelIdentity identity;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto found = channel_ids_.find(channel);
        if (mode_ != ChannelMode::Channels || found == channel_ids_.end()) {
            return;
        }
        identity = found->second;
    }
    suppress_events(channel);
    transport_->close_channel(channel);
    transport_->unassign_channel(channel);
    open_slave_channel(channel, identity);
}

void Manager::reopen_scan_channel() {
    std::lock_guard<std::recursive_mutex> config(config_mutex_);
    int channel;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (mode_ != ChannelMode::Scan) {
            return;
        }
        channel = scan_config_ ? scan_config_->channel : 0;
    }
    suppress_events(channel);
    transport_->close_channel(channel);
    transport_->unassign_channel(channel);
    open_scan_channel();
}

void Manager::reapply_configuration() {
    std::lock_guard<std::recursive_mutex> config(config_mutex_);
    // The wanted channels, not just the open ones - a reset is also a chance
    // to get dedicated channels that failed to open earlier going
    std::optional<std::vector<ChannelIdentity>> identities;
    std::optional<ScanConfig> scan;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        identities = wanted_channels_;
        scan = scan_config_;
    }
    if (!scan) {
        throw ConfigurationError("no scan configuration to reapply");
    }
    configure_scan_locked(*scan);
    if (identities && !identities->empty()) {
        // A failure here leaves the stick working in scan mode - not raised
        // out of the reset (the caller still has to restart the message
        // pump); retry_device_channels() picks it up from there
        try {
            open_device_channels(*identities);
        } catch (const ConfigurationError& error) {
            log(std::string("Dedicated channels failed to reopen after reset, staying on the scan channel: ") +
                error.what());
        }
    }
}

std::optional<int> Manager::channel_for(int device_type, int device_number) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& entry : channel_ids_) {
        if (std::get<0>(entry.second) == device_type && std::get<1>(entry.second) == device_number) {
            return entry.first;
        }
    }
    return std::nullopt;
}

std::map<int, ChannelIdentity> Manager::channel_devices() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return channel_ids_;
}

void Manager::suppress_events(std::optional<int> channel) {
    const double until = clock_() + kReconfigureEventGraceS;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!channel) {
        suppress_events_until_ = until;
        suppress_channel_until_.clear();
    } else {
        suppress_channel_until_[*channel] = until;
    }
}

bool Manager::events_suppressed(std::optional<int> channel, std::optional<double> now) {
    const double at = now ? *now : clock_();
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (at < suppress_events_until_) {
        return true;
    }
    if (!channel) {
        for (const auto& entry : suppress_channel_until_) {
            if (at < entry.second) {
                return true;
            }
        }
        return false;
    }
    auto found = suppress_channel_until_.find(*channel);
    return found != suppress_channel_until_.end() && at < found->second;
}

void Manager::close_all_channels() {
    // Best-effort: a channel may already be closed/unassigned
    std::set<int> channels;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& entry : channel_ids_) {
            channels.insert(entry.first);
        }
        if (scan_config_) {
            channels.insert(scan_config_->channel);
        }
    }
    for (int channel : channels) {
        transport_->close_channel(channel);
        transport_->unassign_channel(channel);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    channel_ids_.clear();
}

void Manager::open_scan_channel() {
    ScanConfig config;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!scan_config_) {
            throw ConfigurationError("the scan channel needs configure_scan() first");
        }
        config = *scan_config_;
    }
    const bool ok = transport_->assign_channel(config.channel, kChannelTypeScan, config.network_number) &&
                    transport_->set_channel_id(config.channel, 0, 0, 0) &&
                    transport_->set_channel_rf_frequency(config.channel, config.rf_frequency) &&
                    transport_->set_rx_extended_messages_enabled(true) &&
                    transport_->open_rx_scan_mode();
    if (!ok) {
        throw ConfigurationError("ANT scan channel configuration failed");
    }
    const double now = clock_();
    std::lock_guard<std::mutex> lock(state_mutex_);
    mode_ = ChannelMode::Scan;
    channel_ids_.clear();
    scan_opened_at_ = now;
}

void Manager::open_slave_channel(int channel, const ChannelIdentity& identity) {
    const auto [device_type, device_number, transmission_type] = identity;
    ScanConfig config;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!scan_config_) {
            throw ConfigurationError("dedicated channels need configure_scan() first");
        }
        config = *scan_config_;
    }
    const bool ok = transport_->assign_channel(channel, kChannelTypeSlave, config.network_number) &&
                    transport_->set_channel_id(channel, device_number, device_type, transmission_type) &&
                    transport_->set_channel_period(channel, channel_period(device_type)) &&
                    transport_->set_channel_rf_frequency(channel, config.rf_frequency);
    if (!ok) {
        throw ConfigurationError("ANT channel " + std::to_string(channel) + " configuration failed for " +
                                 identity_text(identity));
    }
    // Search forever in low priority. A stick without low priority search
    // support falls back to searching forever in high priority.
    if (!(transport_->set_low_priority_search_timeout(channel, kLowPrioritySearchForever) &&
          transport_->set_search_timeout(channel, kHighPrioritySearchOff))) {
        transport_->set_search_timeout(channel, kSearchForever);
    }
    if (!transport_->open_channel(channel)) {
        throw ConfigurationError("ANT channel " + std::to_string(channel) + " failed to open for " +
                                 identity_text(identity));
    }
}

Manager::MessageResult Manager::on_message(const DecodedMessage& message) {
    MessageResult result;

    std::optional<int> number, type, transmission;
    if (message.has_device_id.value_or(false)) {
        if (message.device_number) number = static_cast<int>(*message.device_number);
        type = message.device_type;
        transmission = message.transmission_type;
    } else if (message.payload && message.channel) {
        // A dedicated channel only ever receives its own device, so a data
        // message without extended device ID still belongs to it (as long as
        // its full ID, transmission type included, is known)
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto found = channel_ids_.find(*message.channel);
        if (found != channel_ids_.end() && std::get<2>(found->second) != 0) {
            type = std::get<0>(found->second);
            number = std::get<1>(found->second);
            transmission = std::get<2>(found->second);
        }
    }
    if (!number || !type || !transmission) {
        return result;
    }

    const std::tuple<int, int, int> key{*number, *type, *transmission};
    const double now = sensor_clock_();
    std::shared_ptr<Sensor> sensor;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        auto found = devices_.find(key);
        if (found == devices_.end()) {
            sensor = std::make_shared<Sensor>(*number, *type, *transmission, message.channel.value_or(0),
                                              make_profile(*type), sensor_clock_, now);
            devices_[key] = sensor;
            device_order_.push_back(sensor);
        } else {
            sensor = found->second;
        }
        // Recovery test hook: drop the message before update(), simulating a
        // device going silent on the ANT link
        if (drop_devices_.count(*number)) {
            result.dropped = true;
            return result;
        }
    }

    // A device heard on the scan channel moves to its own channel in
    // channels mode
    if (message.channel) {
        sensor->set(&Sensor::channel, *message.channel);
    }
    result.sensor = sensor;
    result.changed = sensor->update(message);
    return result;
}

std::vector<std::shared_ptr<Sensor>> Manager::get_devices() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return device_order_;
}

std::shared_ptr<Sensor> Manager::find_sensor(int device_type, int device_number) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    for (const auto& sensor : device_order_) {
        if (sensor->device_type() == device_type && sensor->device_number() == device_number) {
            return sensor;
        }
    }
    return nullptr;
}

ChannelMode Manager::mode() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return mode_;
}

bool Manager::needs_reset() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return needs_reset_;
}

std::optional<ScanConfig> Manager::scan_config() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return scan_config_;
}

std::optional<int> Manager::device_number() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return device_number_;
}

void Manager::set_drop_devices(std::set<int> device_numbers) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    drop_devices_ = std::move(device_numbers);
}

}  // namespace antnative

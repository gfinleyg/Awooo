#pragma once

// A stand-in for the ANT USB stick (tests/fakes.py's FakeNative), for running the bridge, AntNative's real Manager
// and Recovery, and the reading pipeline without hardware. Every call is recorded; each configuration call succeeds
// unless its name is in `fail` (or `failOnce`); messages are real ANT messages, raw bytes as the stick sends them
// (extended device ID on, as on the scan channel), decoded by AntNative's own decoder on the way in.

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "AntBridge.hpp"
#include "ant_core.hpp"
#include "ant_manager.hpp"

namespace awotest {

class FakeTransport : public antnative::Transport {
public:
    struct Send {
        int channel;
        std::vector<std::uint8_t> payload;
        std::uint32_t timeoutMs;
    };

    std::vector<std::string> calls;
    std::set<std::string> fail;
    std::set<std::string> failOnce;
    bool opened = false;
    bool pumping = false;
    antnative::Stick::MessageHandler sink;
    std::vector<antnative::UsbDeviceInfo> sticks{{0, 0x0FCF, 0x1008, 1, 0, std::string("Fake stick"), std::nullopt}};
    std::function<std::vector<antnative::UsbDeviceInfo>()> listDevicesHook;
    std::deque<int> sendResults;
    int defaultSendResult = antnative::kFramerPass;
    std::vector<Send> sends;

    int count(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(std::count(calls.begin(), calls.end(), name));
    }
    // (copies, for tests where the bridge's thread is running)
    std::vector<std::string> callsCopy() const {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
    std::vector<Send> sendsCopy() const {
        std::lock_guard<std::mutex> lock(mutex);
        return sends;
    }

    // ---- Transport ----
    std::vector<antnative::UsbDeviceInfo> list_devices() override {
        return listDevicesHook ? listDevicesHook() : sticks;
    }
    bool open(std::uint32_t, int) override {
        opened = call("open");
        return opened;
    }
    void close() override {
        call("close");
        opened = false;
    }
    bool is_open() override { return opened; }
    bool reset() override { return call("reset"); }
    bool set_network_key(int, const std::vector<std::uint8_t>&) override { return call("set_network_key"); }
    bool assign_channel(int, int, int) override { return call("assign_channel"); }
    bool unassign_channel(int) override { return call("unassign_channel"); }
    bool set_channel_id(int, int, int, int) override { return call("set_channel_id"); }
    bool set_channel_period(int, int) override { return call("set_channel_period"); }
    bool set_channel_rf_frequency(int, int) override { return call("set_channel_rf_frequency"); }
    bool set_search_timeout(int, int) override { return call("set_search_timeout"); }
    bool set_low_priority_search_timeout(int, int) override { return call("set_low_priority_search_timeout"); }
    bool set_rx_extended_messages_enabled(bool) override { return call("set_rx_extended_messages_enabled"); }
    bool open_rx_scan_mode() override { return call("open_rx_scan_mode"); }
    bool open_channel(int) override { return call("open_channel"); }
    bool close_channel(int) override { return call("close_channel"); }
    void start_message_pump() override {
        call("start_message_pump");
        pumping = true;
    }
    void stop_message_pump() override {
        call("stop_message_pump");
        pumping = false;
    }
    int send_acknowledged(int channel, const std::vector<std::uint8_t>& payload, std::uint32_t timeoutMs) override {
        std::lock_guard<std::mutex> lock(mutex);
        sends.push_back({channel, payload, timeoutMs});
        if (!sendResults.empty()) {
            const int result = sendResults.front();
            sendResults.pop_front();
            return result;
        }
        return defaultSendResult;
    }

    // A message as the stick would hand it over, through the message pump
    void deliver(const antnative::RawMessage& message) {
        if (pumping && sink) {
            sink(message);
        }
    }

private:
    mutable std::mutex mutex;

    bool call(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(name);
        if (failOnce.erase(name)) {
            return false;
        }
        return !fail.contains(name);
    }
};

// The bridge's clock: time only moves when told to
struct FakeClock {
    std::shared_ptr<double> now = std::make_shared<double>(1000.0);

    double operator()() const { return *now; }
    void advance(double seconds) const { *now += seconds; }
};

// ---- Messages ----

// A broadcast data message with the extended device ID (flag 0x80): [channel, payload x 8, flag, number lo, number
// hi, type, transmission type]
inline antnative::RawMessage message(int deviceType, int deviceNumber, const std::vector<std::uint8_t>& payload,
                                     int channel = 0, int transmissionType = 1) {
    antnative::RawMessage raw;
    raw.message_id = 0x4E;
    raw.data.push_back(static_cast<std::uint8_t>(channel));
    raw.data.insert(raw.data.end(), payload.begin(), payload.end());
    raw.data.push_back(0x80);
    raw.data.push_back(static_cast<std::uint8_t>(deviceNumber & 0xFF));
    raw.data.push_back(static_cast<std::uint8_t>((deviceNumber >> 8) & 0xFF));
    raw.data.push_back(static_cast<std::uint8_t>(deviceType));
    raw.data.push_back(static_cast<std::uint8_t>(transmissionType));
    raw.channel = static_cast<std::uint8_t>(channel);
    raw.has_channel = true;
    return raw;
}

// A channel event (RESPONSE_EVENT, 0x40): a response to message respondingTo (1: an RF event) with this code
inline antnative::RawMessage channelEvent(int channel, int code, int respondingTo = 0x01) {
    antnative::RawMessage raw;
    raw.message_id = 0x40;
    raw.data = {static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(respondingTo),
                static_cast<std::uint8_t>(code)};
    raw.channel = static_cast<std::uint8_t>(channel);
    raw.has_channel = true;
    return raw;
}

inline std::uint8_t lo(int value) {
    return static_cast<std::uint8_t>(value & 0xFF);
}
inline std::uint8_t hi(int value) {
    return static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

// Something to vary in otherwise identical pages (the profiles drop exact repeats)
inline std::uint8_t uniqueByte() {
    static int counter = 0;
    return static_cast<std::uint8_t>(counter++ & 0xFF);
}

// Standard Power-Only page (0x10)
inline antnative::RawMessage powerPage(int deviceNumber, int watts, int cadence = 90, int eventCount = uniqueByte(),
                                       int channel = 0, int transmissionType = 5) {
    return message(11, deviceNumber, {0x10, lo(eventCount), 0xFF, static_cast<std::uint8_t>(cadence), 0, 0, lo(watts), hi(watts)},
                   channel, transmissionType);
}

// Heart rate page (4 by default): event time and beat count in bytes 4-6, the heart rate in byte 7
inline antnative::RawMessage hrPage(int deviceNumber, int bpm, int beatCount, int eventTicks, int channel = 0,
                                    int transmissionType = 1, int page = 4) {
    return message(120, deviceNumber,
                   {static_cast<std::uint8_t>(page), 0xFF, 0, 0, lo(eventTicks), hi(eventTicks), lo(beatCount),
                    static_cast<std::uint8_t>(bpm)},
                   channel, transmissionType);
}

// Speed-only (123) / cadence-only (122) page: event time and cumulative revolutions in bytes 4-7
inline antnative::RawMessage bscPage(int deviceType, int deviceNumber, int eventTicks, int revolutions, int channel = 0,
                                     int transmissionType = 1, int page = 0) {
    return message(deviceType, deviceNumber,
                   {static_cast<std::uint8_t>(page), 0xFF, 0xFF, 0xFF, lo(eventTicks), hi(eventTicks), lo(revolutions),
                    hi(revolutions)},
                   channel, transmissionType);
}

// Combined speed and cadence (121), page-less
inline antnative::RawMessage combinedPage(int deviceNumber, int cadenceTicks, int cadenceRevolutions, int speedTicks,
                                          int speedRevolutions, int channel = 0, int transmissionType = 1) {
    return message(121, deviceNumber,
                   {lo(cadenceTicks), hi(cadenceTicks), lo(cadenceRevolutions), hi(cadenceRevolutions), lo(speedTicks),
                    hi(speedTicks), lo(speedRevolutions), hi(speedRevolutions)},
                   channel, transmissionType);
}

// FE-C Specific Trainer Data (0x19)
inline antnative::RawMessage trainerPage(int deviceNumber, int watts, int cadence = 85, int status = 0, int channel = 0,
                                         int transmissionType = 5) {
    return message(17, deviceNumber,
                   {0x19, uniqueByte(), static_cast<std::uint8_t>(cadence), 0, 0, lo(watts),
                    static_cast<std::uint8_t>(((watts >> 8) & 0x0F) | (status << 4)), 0},
                   channel, transmissionType);
}

// Battery status: the heart rate monitor's page 7, otherwise common page 82
inline antnative::RawMessage batteryPage(int deviceType, int deviceNumber, int status, std::optional<int> level = {},
                                         int channel = 0, int transmissionType = 5) {
    if (deviceType == 120) {
        return message(deviceType, deviceNumber,
                       {0x07, static_cast<std::uint8_t>(level.value_or(0xFF)), 0,
                        static_cast<std::uint8_t>((status << 4) | 3), 0, 0, 0, 0},
                       channel, transmissionType);
    }
    return message(deviceType, deviceNumber, {0x52, 0xFF, 0xFF, 0, 0, 0, 0, static_cast<std::uint8_t>((status << 4) | 3)},
                   channel, transmissionType);
}

// A bridge on a fake stick, its clock, and the stick
struct FakeAnt {
    std::shared_ptr<FakeTransport> stick = std::make_shared<FakeTransport>();
    FakeClock clock;

    std::unique_ptr<awo::ant::AntBridge> makeBridge() {
        auto transport = stick;
        return std::make_unique<awo::ant::AntBridge>(
            [transport](antnative::Stick::MessageHandler sink) {
                transport->sink = std::move(sink);
                return transport;
            },
            clock, [clock = clock](double seconds) { clock.advance(seconds); });
    }
};

}  // namespace awotest

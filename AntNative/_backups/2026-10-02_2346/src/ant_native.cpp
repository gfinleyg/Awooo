#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#include "dsi_framer_ant.hpp"
#include "dsi_serial_generic.hpp"
#include "usb_device_handle.hpp"

namespace py = pybind11;

namespace {

constexpr unsigned short kFramerTimedOut = DSI_FRAMER_TIMEDOUT;
constexpr unsigned short kFramerError = DSI_FRAMER_ERROR;

// ANT RESPONSE_EVENT (message ID 0x40) codes. These are the wire-protocol
// byte values documented in the public ANT Message Protocol and Usage
// guide - stable across ANT SDK versions, so they're named here directly
// rather than pulled from whatever SDK header this build happens to have.
// A 0x40 message's 3-byte payload is: data[0] = channel, data[1] = the
// message ID this is responding to (0x01 for an unsolicited asynchronous
// channel event, not a reply to anything we sent), data[2] = the code
// below.
constexpr std::uint8_t kEventRxSearchTimeout = 0x01;
constexpr std::uint8_t kEventRxFail = 0x02;
constexpr std::uint8_t kEventTx = 0x03;
constexpr std::uint8_t kEventTransferRxFailed = 0x04;
constexpr std::uint8_t kEventTransferTxCompleted = 0x05;
constexpr std::uint8_t kEventTransferTxFailed = 0x06;
constexpr std::uint8_t kEventChannelClosed = 0x07;
constexpr std::uint8_t kEventRxFailGoToSearch = 0x08;
constexpr std::uint8_t kEventChannelCollision = 0x09;
constexpr std::uint8_t kEventTransferTxStart = 0x0A;
constexpr std::uint8_t kChannelInWrongState = 0x15;
constexpr std::uint8_t kChannelNotOpened = 0x16;
constexpr std::uint8_t kChannelIdNotSet = 0x18;
constexpr std::uint8_t kTransferInProgress = 0x1F;
constexpr std::uint8_t kTransferSequenceNumberError = 0x20;
constexpr std::uint8_t kTransferInError = 0x21;
constexpr std::uint8_t kMessageSizeExceedsLimit = 0x27;
constexpr std::uint8_t kInvalidMessage = 0x28;
constexpr std::uint8_t kInvalidNetworkNumber = 0x29;
constexpr std::uint8_t kInvalidListId = 0x30;
constexpr std::uint8_t kInvalidScanTxChannel = 0x31;
constexpr std::uint8_t kInvalidParameterProvided = 0x33;
constexpr std::uint8_t kEventSerialQueOverflow = 0x34;
constexpr std::uint8_t kEventQueOverflow = 0x35;

// Named lookup for the code above - decoded here (once, in the transport
// layer) so Python never has to hardcode ANT protocol byte values to
// decide how to react to one. Policy (which of these matter, and what to
// do about them) stays in Python (ant/recovery.py); this just names them.
const char* response_event_name(std::uint8_t code) {
    switch (code) {
        case 0x00: return "RESPONSE_NO_ERROR";
        case kEventRxSearchTimeout: return "EVENT_RX_SEARCH_TIMEOUT";
        case kEventRxFail: return "EVENT_RX_FAIL";
        case kEventTx: return "EVENT_TX";
        case kEventTransferRxFailed: return "EVENT_TRANSFER_RX_FAILED";
        case kEventTransferTxCompleted: return "EVENT_TRANSFER_TX_COMPLETED";
        case kEventTransferTxFailed: return "EVENT_TRANSFER_TX_FAILED";
        case kEventChannelClosed: return "EVENT_CHANNEL_CLOSED";
        case kEventRxFailGoToSearch: return "EVENT_RX_FAIL_GO_TO_SEARCH";
        case kEventChannelCollision: return "EVENT_CHANNEL_COLLISION";
        case kEventTransferTxStart: return "EVENT_TRANSFER_TX_START";
        case kChannelInWrongState: return "CHANNEL_IN_WRONG_STATE";
        case kChannelNotOpened: return "CHANNEL_NOT_OPENED";
        case kChannelIdNotSet: return "CHANNEL_ID_NOT_SET";
        case kTransferInProgress: return "TRANSFER_IN_PROGRESS";
        case kTransferSequenceNumberError: return "TRANSFER_SEQUENCE_NUMBER_ERROR";
        case kTransferInError: return "TRANSFER_IN_ERROR";
        case kMessageSizeExceedsLimit: return "MESSAGE_SIZE_EXCEEDS_LIMIT";
        case kInvalidMessage: return "INVALID_MESSAGE";
        case kInvalidNetworkNumber: return "INVALID_NETWORK_NUMBER";
        case kInvalidListId: return "INVALID_LIST_ID";
        case kInvalidScanTxChannel: return "INVALID_SCAN_TX_CHANNEL";
        case kInvalidParameterProvided: return "INVALID_PARAMETER_PROVIDED";
        case kEventSerialQueOverflow: return "EVENT_SERIAL_QUE_OVERFLOW";
        case kEventQueOverflow: return "EVENT_QUE_OVERFLOW";
        default: return "UNKNOWN";
    }
}

struct NativeMessage {
    std::uint8_t message_id{};
    std::vector<std::uint8_t> data;
    std::uint8_t channel{0xFF};
    bool has_channel{false};
    bool is_error{false};
    std::uint8_t error_code{0};
};

// ANT+ profile page decoding. Moved here from the Python "ant.profiles"
// layer (heart_rate.py / power_meter.py / speed_cadence.py) so page-format
// knowledge lives in exactly one place and per-message parsing runs in C++
// instead of Python. Byte offsets below were validated against, and where
// noted are a direct port of, the previous C/C++ receiver's live decode
// path (AntWorkout.cpp ProcessMessage), which is the field-tested ground
// truth for this hardware.
//
// What stays in Python: anything *stateful* across messages - rollover-
// safe rate computation (speed_mps/cadence_rpm), wheel-circumference-
// dependent math (the wheel size is a runtime UI setting, not something
// this transport layer knows about), and redundant-message dedup. Those
// still live in ant/profiles/*.py, now operating on the fields decoded
// here instead of re-parsing raw payload bytes themselves.
namespace profile_decode {

constexpr int kDeviceTypeHeartRate = 120;
constexpr int kDeviceTypePower = 11;
constexpr int kDeviceTypeSpeedCadenceCombined = 121;
constexpr int kDeviceTypeCadence = 122;
constexpr int kDeviceTypeSpeed = 123;

// ANT+ Bicycle Power Profile "Standard Power-Only" page.
constexpr std::uint8_t kPowerOnlyPage = 0x10;

inline unsigned u16le(const std::uint8_t* payload, std::size_t offset) {
    return static_cast<unsigned>(payload[offset]) |
           (static_cast<unsigned>(payload[offset + 1]) << 8);
}

// ANT+ HRM pages 0-4 all carry the same event-time/beat-count/computed-HR
// fields in bytes 4-7; page 4 additionally carries the previous beat's
// event time in bytes 2-3, from which the RR interval is derived.
void decode_heart_rate(const std::uint8_t* payload, py::dict& message) {
    const std::uint8_t raw_page = payload[0];
    const std::uint8_t page = raw_page & 0x7F;
    message["page"] = static_cast<int>(page);
    message["page_change_toggle"] = (raw_page & 0x80) != 0;

    if (page > 4) {
        return;
    }

    const unsigned event_ticks = u16le(payload, 4);
    message["heart_beat_event_time"] = event_ticks / 1024.0;
    // Raw ticks alongside the seconds value so Python's rollover-safe
    // unwrap can work with exact integers instead of reversing a
    // divided float back into ticks.
    message["heart_beat_event_ticks"] = static_cast<int>(event_ticks);
    message["heart_beat_count"] = static_cast<int>(payload[6]);
    message["computed_heart_rate"] = static_cast<int>(payload[7]);

    if (page == 4) {
        const unsigned previous_ticks = u16le(payload, 2);
        message["previous_heart_beat_event_time"] = previous_ticks / 1024.0;
        const unsigned rr_ticks = (event_ticks - previous_ticks) & 0xFFFF;
        message["rr_interval"] = rr_ticks / 1024.0;
    }
}

// Only the Standard Power-Only page (0x10) is decoded, matching exactly
// what the previous C/C++ receiver's live decode path actually read from
// real hardware (it ignored every other power page). Instantaneous power
// is a little-endian u16 in payload bytes 6-7.
//
// Byte 3 of this same page is Instantaneous Cadence (crank RPM, 0xFF =
// invalid/not supported by this power meter) - most crank/pedal-based
// power meters already have to sense crank rotation to compute power at
// all, so this is normally populated even though it's a power page, not
// a dedicated cadence sensor. Decoded here so a missing/stale dedicated
// cadence sensor has something to fall back to (see RecoveryManager's
// STALE/LOST/data_stalled state and ClassAntDevicesPoller's fallback
// logic) - there is no equivalent field for speed, since a power meter
// has no way to know wheel size or gear ratio.
void decode_power(const std::uint8_t* payload, py::dict& message) {
    const std::uint8_t raw_page = payload[0];
    const std::uint8_t page = raw_page & 0x7F;
    message["page"] = static_cast<int>(page);
    message["page_change_toggle"] = (raw_page & 0x80) != 0;

    if (page != kPowerOnlyPage) {
        return;
    }

    message["instantaneous_power_watts"] = static_cast<int>(u16le(payload, 6));

    // byte 2 is pedal power balance, byte 3 instantaneous cadence
    const std::uint8_t cadence_raw = payload[3];
    if (cadence_raw != 0xFF) {
        message["power_meter_cadence_rpm"] = static_cast<int>(cadence_raw);
    }
}

// Combined speed+cadence (121) uses a page-less layout: every message
// carries both readings unconditionally. This is NOT the same layout as
// the single-function BSC page 0 below, and is a direct port of the
// previous C/C++ receiver's live decode path (AntWorkout.cpp
// ProcessMessage), which read every 121 message this way with no page
// check at all:
//   cadence event time       = payload[0:2] (LE, 1/1024s ticks)
//   cadence revolution count = payload[2:4] (LE)
//   speed event time         = payload[4:6] (LE, 1/1024s ticks)
//   speed revolution count   = payload[6:8] (LE)
void decode_speed_cadence_combined(const std::uint8_t* payload, py::dict& message) {
    const unsigned cadence_ticks = u16le(payload, 0);
    const unsigned speed_ticks = u16le(payload, 4);
    message["cadence_event_time"] = cadence_ticks / 1024.0;
    message["cadence_event_ticks"] = static_cast<int>(cadence_ticks);
    message["cadence_revolution_count"] = static_cast<int>(u16le(payload, 2));
    message["speed_event_time"] = speed_ticks / 1024.0;
    message["speed_event_ticks"] = static_cast<int>(speed_ticks);
    message["speed_revolution_count"] = static_cast<int>(u16le(payload, 6));
}

// Single-function speed (123) / cadence (122): standard ANT+ BSC page 0
// carries event time + cumulative revolution count in bytes 4-7.
void decode_speed_cadence_single(const std::uint8_t* payload, py::dict& message) {
    const std::uint8_t raw_page = payload[0];
    const std::uint8_t page = raw_page & 0x7F;
    message["page"] = static_cast<int>(page);
    message["page_change_toggle"] = (raw_page & 0x80) != 0;

    if (page != 0) {
        return;
    }

    const unsigned event_ticks = u16le(payload, 4);
    message["event_time"] = event_ticks / 1024.0;
    message["event_ticks"] = static_cast<int>(event_ticks);
    message["revolution_count"] = static_cast<int>(u16le(payload, 6));
}

void decode(int device_type, const std::uint8_t* payload, std::size_t payload_size,
            py::dict& message) {
    if (payload == nullptr || payload_size < 8) {
        return;
    }
    switch (device_type) {
        case kDeviceTypeHeartRate:
            decode_heart_rate(payload, message);
            break;
        case kDeviceTypePower:
            decode_power(payload, message);
            break;
        case kDeviceTypeSpeedCadenceCombined:
            decode_speed_cadence_combined(payload, message);
            break;
        case kDeviceTypeCadence:
        case kDeviceTypeSpeed:
            decode_speed_cadence_single(payload, message);
            break;
        default:
            break;
    }
}

}  // namespace profile_decode

class ANTNative {
public:
    static py::list list_devices() {
        // GetAllDevices() returns the SDK's current USB ANT device enumeration.
        // The returned USBDevice pointers are owned by the local ANTDeviceList and
        // must not be retained after this function returns. We copy only metadata.
        // USB enumeration can block, so it runs without the GIL; the Python
        // objects below are only built once the GIL is back.
        const ANTDeviceList devices = [] {
            py::gil_scoped_release release;
            return USBDeviceHandle::GetAllDevices();
        }();
        py::list result;

        for (ULONG i = 0; i < devices.GetSize(); ++i) {
            const USBDevice* device = devices[i];
            if (device == nullptr) {
                continue;
            }

            py::dict item;
            item["index"] = static_cast<unsigned int>(i);
            item["vid"] = static_cast<unsigned int>(device->GetVid());
            item["pid"] = static_cast<unsigned int>(device->GetPid());
            item["serial_number"] = static_cast<unsigned long>(device->GetSerialNumber());
            item["device_type"] = static_cast<unsigned int>(device->GetDeviceType());

            UCHAR product[USB_MAX_STRLEN] = {};
            UCHAR serial[USB_MAX_STRLEN] = {};
            if (device->GetProductDescription(product, USB_MAX_STRLEN)) {
                item["product_description"] =
                    std::string(reinterpret_cast<const char*>(product));
            } else {
                item["product_description"] = py::none();
            }
            if (device->GetSerialString(serial, USB_MAX_STRLEN)) {
                item["serial_string"] =
                    std::string(reinterpret_cast<const char*>(serial));
            } else {
                item["serial_string"] = py::none();
            }

            result.append(item);
        }

        return result;
    }

    ANTNative() = default;

    ~ANTNative() {
        close();
    }

    ANTNative(const ANTNative&) = delete;
    ANTNative& operator=(const ANTNative&) = delete;

    bool open(std::uint32_t baudrate = 57600, std::uint8_t device_number = 0) {
        // Opening the USB stick can block; nothing here touches Python.
        py::gil_scoped_release release;
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (opened_) {
            return true;
        }

        serial_ = new DSISerialGeneric();
        if (!serial_->Init(static_cast<ULONG>(baudrate), device_number)) {
            delete serial_;
            serial_ = nullptr;
            return false;
        }

        framer_ = new DSIFramerANT(serial_);
        if (!framer_->Init()) {
            delete framer_;
            framer_ = nullptr;
            delete serial_;
            serial_ = nullptr;
            return false;
        }

        serial_->SetCallback(framer_);
        if (!serial_->Open()) {
            delete framer_;
            framer_ = nullptr;
            delete serial_;
            serial_ = nullptr;
            return false;
        }

        cancel_ = FALSE;
        opened_ = true;
        return true;
    }

    void close() {
        stop_message_pump();

        // Blocking USB close, without the GIL. operation_mutex_ keeps another
        // thread's in-flight blocking_call() from using framer_ while it is
        // deleted here.
        py::gil_scoped_release release;
        std::lock_guard<std::mutex> operation_lock(operation_mutex_);
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (serial_) {
            serial_->Close();
        }
        delete framer_;
        framer_ = nullptr;
        delete serial_;
        serial_ = nullptr;
        opened_ = false;
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return opened_;
    }

    bool request_message(int channel, int message_id) {
        if (channel < 0 || channel > 255) {
            throw std::runtime_error("channel must be 0..255");
        }
        if (message_id < 0 || message_id > 255) {
            throw std::runtime_error("message_id must be 0..255");
        }

        py::gil_scoped_release release;
        std::lock_guard<std::mutex> lock(operation_mutex_);
        DSIFramerANT* framer = nullptr;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            framer = opened_ ? framer_ : nullptr;
        }
        if (!framer) {
            return false;
        }

        // Use the vendor SDK's native RequestMessage() implementation.
        // This sends ANT message 0x4D with the requested channel/message ID.
        return framer->RequestMessage(
            static_cast<UCHAR>(channel),
            static_cast<UCHAR>(message_id));
    }

    py::dict get_channel_id(int channel, std::uint32_t response_timeout_ms = 1000) {
        if (channel < 0 || channel > 255) {
            throw std::runtime_error("channel must be 0..255");
        }

        USHORT device_number = 0;
        UCHAR device_type = 0;
        UCHAR transmission_type = 0;

        const BOOL ok = blocking_call([&](DSIFramerANT* framer) {
            return framer->GetChannelID(
                static_cast<UCHAR>(channel),
                &device_number,
                &device_type,
                &transmission_type,
                response_timeout_ms);
        });

        if (!ok) {
            throw std::runtime_error("ANT GetChannelID request failed or timed out");
        }

        py::dict result;
        result["channel"] = channel;
        result["device_number"] = static_cast<unsigned int>(device_number);
        result["device_type"] = static_cast<unsigned int>(device_type);
        result["transmission_type"] = static_cast<unsigned int>(transmission_type);
        return result;
    }

    void start_message_pump(py::object callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_ = std::move(callback);
        start_message_pump_locked();
    }

    void stop_message_pump() {
        std::thread thread_to_join;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (!pump_running_) {
                callback_ = py::none();
                return;
            }
            cancel_ = TRUE;
            pump_running_ = false;
            thread_to_join = std::move(pump_thread_);
        }

        // Do not hold callback_mutex_ while joining: the receiver thread may
        // need that mutex while it is dispatching its final callback.
        //
        // Nor the GIL: the receiver thread takes the GIL to dispatch each
        // message, so joining it while holding the GIL deadlocks the whole
        // process the moment a message arrives during the join.
        if (thread_to_join.joinable()) {
            py::gil_scoped_release release;
            thread_to_join.join();
        }

        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_ = py::none();
    }

    std::string last_callback_error() const {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        return callback_error_;
    }

    void clear_callback_error() {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_error_.clear();
    }

    bool reset(std::uint32_t response_timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->ResetSystem(response_timeout_ms != 0 ? response_timeout_ms : 0);
        });
    }

    bool set_network_key(std::uint8_t network_number, py::bytes key,
                         std::uint32_t response_timeout_ms = 1000) {
        std::string value = key;
        if (value.size() != 8) {
            throw std::invalid_argument("ANT network key must contain exactly 8 bytes");
        }
        UCHAR key_data[8];
        for (std::size_t i = 0; i < 8; ++i) {
            key_data[i] = static_cast<UCHAR>(static_cast<unsigned char>(value[i]));
        }
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SetNetworkKey(network_number, key_data, response_timeout_ms);
        });
    }

    bool assign_channel(std::uint8_t channel, std::uint8_t channel_type,
                        std::uint8_t network_number, std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->AssignChannel(channel, channel_type, network_number, timeout_ms);
        });
    }

    bool unassign_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->UnAssignChannel(channel, timeout_ms);
        });
    }

    bool set_channel_id(std::uint8_t channel, std::uint16_t device_number,
                        std::uint8_t device_type, std::uint8_t transmission_type,
                        std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SetChannelID(channel, device_number, device_type,
                                        transmission_type, timeout_ms);
        });
    }

    bool set_channel_period(std::uint8_t channel, std::uint16_t period,
                            std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SetChannelPeriod(channel, period, timeout_ms);
        });
    }

    bool set_channel_rf_frequency(std::uint8_t channel, std::uint8_t rf_frequency,
                                  std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SetChannelRFFrequency(channel, rf_frequency, timeout_ms);
        });
    }

    bool set_search_priority(std::uint8_t channel, std::uint8_t priority,
                             std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SetChannelSearchPriority(channel, priority, timeout_ms);
        });
    }

    // Search timeouts in 2.5 s units, 0xFF = search forever. A dedicated
    // slave channel whose device drops out goes back to searching on its
    // own; an infinite timeout keeps the stick from closing the channel.
    bool set_search_timeout(std::uint8_t channel, std::uint8_t timeout,
                            std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SetChannelSearchTimeout(channel, timeout, timeout_ms);
        });
    }

    bool set_low_priority_search_timeout(std::uint8_t channel, std::uint8_t timeout,
                                         std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SetLowPriorityChannelSearchTimeout(channel, timeout, timeout_ms);
        });
    }

    bool set_rx_extended_messages_enabled(bool enabled,
                                          std::uint32_t timeout_ms = 1000) {
        // Vendor SDK API: RxExtMesgsEnable() forces the module to emit
        // extended receive messages when enabled.
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->RxExtMesgsEnable(enabled ? 1 : 0, timeout_ms);
        });
    }

    bool open_rx_scan_mode(std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->OpenRxScanMode(timeout_ms);
        });
    }

    bool open_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->OpenChannel(channel, timeout_ms);
        });
    }

    bool close_channel(std::uint8_t channel, std::uint32_t timeout_ms = 1000) {
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->CloseChannel(channel, timeout_ms);
        });
    }

    bool send_broadcast(std::uint8_t channel, py::bytes payload) {
        std::string value = payload;
        require_payload_8(value, "broadcast payload");
        UCHAR data[8];
        copy_bytes(value, data, 8);
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SendBroadcastData(channel, data);
        });
    }

    int send_acknowledged(std::uint8_t channel, py::bytes payload,
                          std::uint32_t timeout_ms = 1000) {
        std::string value = payload;
        require_payload_8(value, "acknowledged payload");
        UCHAR data[8];
        copy_bytes(value, data, 8);
        return static_cast<int>(blocking_call([&](DSIFramerANT* framer) {
            return framer->SendAcknowledgedData(channel, data, timeout_ms);
        }));
    }

    bool send_burst_packet(std::uint8_t channel_sequence, py::bytes payload) {
        std::string value = payload;
        if (value.size() != 8) {
            throw std::invalid_argument("burst packet payload must contain exactly 8 bytes");
        }
        UCHAR data[8];
        copy_bytes(value, data, 8);
        return blocking_call([&](DSIFramerANT* framer) {
            return framer->SendBurstDataPacket(channel_sequence, data);
        });
    }

    unsigned int get_message_buffer_capacity() const {
        return static_cast<unsigned int>(sizeof(ANT_MESSAGE{}.aucData));
    }

    unsigned int get_channel_number(std::uint8_t message_id, py::bytes data) {
        std::string value = data;
        if (value.size() > MESG_MAX_SIZE_VALUE) {
            throw std::invalid_argument("ANT message data is too large");
        }
        ANT_MESSAGE message{};
        message.ucMessageID = message_id;
        copy_bytes(value, message.aucData, value.size());
        return static_cast<unsigned int>(require_framer()->GetChannelNumber(&message));
    }

private:
    // Runs one blocking ANT SDK call (these wait up to their timeout for the
    // stick's response) with the GIL released, so the UI and the message
    // pump's Python callback keep running meanwhile. operation_mutex_ keeps
    // these calls one at a time across threads, as holding the GIL used to,
    // and stops close() deleting the framer mid-call. `call` must not touch
    // any Python object.
    template <typename Call>
    auto blocking_call(Call&& call) {
        py::gil_scoped_release release;
        std::lock_guard<std::mutex> lock(operation_mutex_);
        return call(require_framer());
    }

    DSIFramerANT* require_framer() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!opened_ || framer_ == nullptr) {
            throw std::runtime_error("ANT device is not open");
        }
        return framer_;
    }

    void start_message_pump_locked() {
        if (pump_running_) {
            return;
        }
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            if (!opened_ || framer_ == nullptr) {
                throw std::runtime_error("ANT device is not open");
            }
        }
        cancel_ = FALSE;
        pump_running_ = true;
        pump_thread_ = std::thread(&ANTNative::message_pump, this);
    }

    void message_pump() {
        while (!cancel_) {
            DSIFramerANT* framer = nullptr;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                framer = framer_;
            }
            if (framer == nullptr) {
                break;
            }

            USHORT available = framer->WaitForMessage(100);
            if (cancel_) {
                break;
            }
            if (available == kFramerTimedOut) {
                continue;
            }

            ANT_MESSAGE message{};
            USHORT size = framer->GetMessage(&message);
            if (size == kFramerTimedOut) {
                continue;
            }

            NativeMessage event;
            if (size != kFramerError) {
                const UCHAR sdk_channel = framer->GetChannelNumber(&message);
                if (sdk_channel != 0xFF) {
                    event.channel = sdk_channel;
                    event.has_channel = true;
                }
            }

            if (size == kFramerError) {
                event.is_error = true;
                event.error_code = message.ucMessageID;
                event.message_id = message.ucMessageID;
                event.data.assign(message.aucData, message.aucData + 1);
            } else {
                event.message_id = message.ucMessageID;
                event.data.assign(message.aucData, message.aucData + size);
                // GetMessage() returns the actual ANT message-data length.
                // Keep all returned bytes: flagged extended data is appended
                // to the ordinary 8-byte payload while retaining the original
                // 0x4E/0x4F/0x50 message ID.
            }

            dispatch_message(event);
        }
    }

    void dispatch_message(const NativeMessage& event) {
        // The GIL comes first: copying (and later releasing) the callback
        // changes its Python refcount, which is only safe while holding it.
        // Lock order is always GIL, then callback_mutex_ (stop_message_pump()
        // takes them in the same order).
        py::gil_scoped_acquire gil;
        py::object callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (callback_.is_none()) {
                return;
            }
            callback = callback_;
        }

        try {
            // Transport framing (channel/device identity/extended fields) is
            // decoded below. ANT+ profile page decoding (HR/power/speed-
            // cadence) happens further down, once device_type is known - see
            // profile_decode above and its call near the end of this
            // function. Stateful, per-session math (rollover-safe rates,
            // wheel-circumference-dependent speed, dedup) stays in Python.
            py::dict message;
            const int message_id = static_cast<int>(event.message_id);
            message["message_id"] = message_id;
            message["data"] = py::bytes(
                reinterpret_cast<const char*>(event.data.data()),
                event.data.size());

            // ANT data messages have: data[0] = channel, data[1:9] =
            // the fixed 8-byte payload. Flagged extended data, when present,
            // starts at data[9] with the extended-message bitfield and then
            // appends the fields selected by that bitfield. The message ID
            // remains 0x4E/0x4F/0x50; it does NOT become 0x5D/0x5E/0x5F.
            const bool is_standard_data =
                event.message_id == 0x4E ||  // broadcast
                event.message_id == 0x4F ||  // acknowledged
                event.message_id == 0x50;    // burst packet

            const bool is_legacy_extended_data =
                event.message_id == 0x5D ||  // legacy extended broadcast
                event.message_id == 0x5E ||  // legacy extended acknowledged
                event.message_id == 0x5F;    // legacy extended burst

            const bool is_data_message = is_standard_data || is_legacy_extended_data;

            // Captured here (rather than re-derived later) so the profile
            // decode call below - which needs both the payload bytes and
            // the device_type extracted further down, once extended data
            // is parsed - doesn't have to recompute anything.
            const std::uint8_t* payload_ptr = nullptr;
            std::size_t payload_len = 0;
            int decoded_device_type = -1;

            if (is_data_message && !event.data.empty()) {
                if (event.has_channel) {
                    message["channel"] = static_cast<int>(event.channel);
                } else {
                    message["channel"] = static_cast<int>(event.data[0]);
                }

                // Only the first 8 bytes after the channel are the ANT RF
                // payload. Do not expose appended extended data as profile data.
                const std::size_t payload_offset = 1;
                const std::size_t payload_size =
                    event.data.size() > payload_offset
                        ? std::min<std::size_t>(8, event.data.size() - payload_offset)
                        : 0;
                message["payload"] = py::bytes(
                    reinterpret_cast<const char*>(event.data.data() + payload_offset),
                    payload_size);

                payload_ptr = event.data.data() + payload_offset;
                payload_len = payload_size;
            }

            message["message_size"] = static_cast<int>(event.data.size());

            // Flagged extended messages use the normal data-message IDs.
            // The ANT protocol specifies that the message is longer than the
            // normal 9-byte message and data[9] is the extended-message bitfield.
            if (is_standard_data && event.data.size() >= 10) {
                const std::uint8_t ext_flags = event.data[9];
                const std::size_t ext_offset = 10;
                const std::size_t ext_size =
                    event.data.size() > ext_offset ? event.data.size() - ext_offset : 0;

                message["has_extended_data"] = true;
                message["extended_bitfield"] = static_cast<int>(ext_flags);
                message["has_device_id"] = (ext_flags & 0x80) != 0;
                message["has_rssi"] = (ext_flags & 0x40) != 0;
                message["has_timestamp"] = (ext_flags & 0x20) != 0;
                message["has_extension"] = (ext_flags & 0x01) != 0;
                message["extended_data"] = py::bytes(
                    reinterpret_cast<const char*>(event.data.data() + ext_offset),
                    ext_size);

                // Extended fields are serialized in the order specified by
                // the ANT extended-message bitfield. Device ID is first,
                // followed by RSSI and timestamp when those flags are present.
                // Only decode fields whose flag is actually set; never assume
                // bytes are present merely because the message is long.
                std::size_t cursor = ext_offset;

                if ((ext_flags & 0x80) != 0 && cursor + 4 <= event.data.size()) {
                    const std::uint8_t* id = event.data.data() + cursor;
                    message["device_number"] =
                        static_cast<unsigned int>(id[0]) |
                        (static_cast<unsigned int>(id[1]) << 8);
                    message["device_type"] = static_cast<int>(id[2]);
                    message["transmission_type"] = static_cast<int>(id[3]);
                    decoded_device_type = static_cast<int>(id[2]);
                    cursor += 4;
                }

                if ((ext_flags & 0x40) != 0) {
                    // RSSI field format is SDK/version dependent (3 or 4 bytes
                    // in published ANT parameter sets). Preserve the complete
                    // remaining extended data rather than guessing its layout.
                    message["rssi_data"] = py::bytes(
                        reinterpret_cast<const char*>(event.data.data() + cursor),
                        event.data.size() > cursor ? event.data.size() - cursor : 0);
                }

                if ((ext_flags & 0x20) != 0) {
                    message["has_timestamp_field"] = true;
                } else {
                    message["has_timestamp_field"] = false;
                }
            } else {
                message["has_extended_data"] = false;
            }

            // Requested Channel ID response (0x51):
            // data[0] = channel, data[1:3] = device number (little endian),
            // data[3] = device type, data[4] = transmission type.
            if (event.message_id == 0x51 && event.data.size() >= 5) {
                message["channel"] = static_cast<int>(event.data[0]);
                message["device_number"] =
                    static_cast<unsigned int>(event.data[1]) |
                    (static_cast<unsigned int>(event.data[2]) << 8);
                message["device_type"] = static_cast<int>(event.data[3]);
                message["transmission_type"] = static_cast<int>(event.data[4]);
            }

            switch (event.message_id) {
                case 0x4D: message["message_name"] = "request_message"; break;
                case 0x4E: message["message_name"] = "broadcast_data"; break;
                case 0x4F: message["message_name"] = "acknowledged_data"; break;
                case 0x50: message["message_name"] = "burst_data"; break;
                case 0x5D: message["message_name"] = "extended_broadcast_data"; break;
                case 0x5E: message["message_name"] = "extended_acknowledged_data"; break;
                case 0x5F: message["message_name"] = "extended_burst_data"; break;
                case 0x40:  message["message_name"] = "response_event"; break;
                default:   message["message_name"] = "ant_message"; break;
            }

            // RESPONSE_EVENT (0x40): channel-level response/event, not a
            // per-device broadcast - never has device identity. data[0] =
            // channel, data[1] = the message ID this responds to (0x01 for
            // an unsolicited channel event), data[2] = the response/event
            // code itself.
            if (event.message_id == 0x40 && event.data.size() >= 3) {
                message["channel"] = static_cast<int>(event.data[0]);
                message["response_to_message_id"] = static_cast<int>(event.data[1]);
                message["event_code"] = static_cast<int>(event.data[2]);
                message["event_name"] = response_event_name(event.data[2]);
            }

            message["is_error"] = event.is_error;
            if (event.is_error) {
                message["error_code"] = static_cast<int>(event.error_code);
            }

            if (decoded_device_type != -1) {
                profile_decode::decode(decoded_device_type, payload_ptr, payload_len, message);
            }

            callback(message);
        } catch (const py::error_already_set& error) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback_error_ = error.what();
        }
    }

    static void require_payload_8(const std::string& value, const char* name) {
        if (value.size() != 8) {
            throw std::invalid_argument(std::string(name) + " must contain exactly 8 bytes");
        }
    }

    static void copy_bytes(const std::string& source, UCHAR* destination, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = static_cast<UCHAR>(static_cast<unsigned char>(source[i]));
        }
    }

    mutable std::mutex state_mutex_;
    DSISerialGeneric* serial_{nullptr};
    DSIFramerANT* framer_{nullptr};
    bool opened_{false};

    mutable std::mutex callback_mutex_;
    std::mutex operation_mutex_;
    py::object callback_{py::none()};
    std::string callback_error_;
    std::thread pump_thread_;
    std::atomic<bool> pump_running_{false};
    volatile BOOL cancel_{FALSE};
};

} // namespace

PYBIND11_MODULE(ant_native, module) {
    module.doc() = "Minimal Python binding for the ANT SDK DSIFramerANT transport layer.";

    module.def("list_devices", &ANTNative::list_devices);

    py::class_<ANTNative>(module, "ANTNative")
        .def(py::init<>())
        .def("open", &ANTNative::open,
             py::arg("baudrate") = 57600, py::arg("device_number") = 0)
        .def("close", &ANTNative::close)
        .def("is_open", &ANTNative::is_open)
        .def("request_message", &ANTNative::request_message,
             py::arg("channel"), py::arg("message_id"))
        .def("get_channel_id", &ANTNative::get_channel_id,
             py::arg("channel"), py::arg("response_timeout_ms") = 1000)
        .def("start_message_pump", &ANTNative::start_message_pump, py::arg("callback"))
        .def("stop_message_pump", &ANTNative::stop_message_pump)
        .def("last_callback_error", &ANTNative::last_callback_error)
        .def("clear_callback_error", &ANTNative::clear_callback_error)
        .def("reset", &ANTNative::reset, py::arg("response_timeout_ms") = 1000)
        .def("set_network_key", &ANTNative::set_network_key,
             py::arg("network_number"), py::arg("key"), py::arg("response_timeout_ms") = 1000)
        .def("assign_channel", &ANTNative::assign_channel,
             py::arg("channel"), py::arg("channel_type"), py::arg("network_number"),
             py::arg("timeout_ms") = 1000)
        .def("unassign_channel", &ANTNative::unassign_channel,
             py::arg("channel"), py::arg("timeout_ms") = 1000)
        .def("set_channel_id", &ANTNative::set_channel_id,
             py::arg("channel"), py::arg("device_number"), py::arg("device_type"),
             py::arg("transmission_type"), py::arg("timeout_ms") = 1000)
        .def("set_channel_period", &ANTNative::set_channel_period,
             py::arg("channel"), py::arg("period"), py::arg("timeout_ms") = 1000)
        .def("set_channel_rf_frequency", &ANTNative::set_channel_rf_frequency,
             py::arg("channel"), py::arg("rf_frequency"), py::arg("timeout_ms") = 1000)
        .def("set_search_priority", &ANTNative::set_search_priority,
             py::arg("channel"), py::arg("priority"), py::arg("timeout_ms") = 1000)
        .def("set_search_timeout", &ANTNative::set_search_timeout,
             py::arg("channel"), py::arg("timeout"), py::arg("timeout_ms") = 1000)
        .def("set_low_priority_search_timeout", &ANTNative::set_low_priority_search_timeout,
             py::arg("channel"), py::arg("timeout"), py::arg("timeout_ms") = 1000)
        .def("set_rx_extended_messages_enabled", &ANTNative::set_rx_extended_messages_enabled,
             py::arg("enabled"), py::arg("timeout_ms") = 1000)
        .def("open_rx_scan_mode", &ANTNative::open_rx_scan_mode,
             py::arg("timeout_ms") = 1000)
        .def("open_channel", &ANTNative::open_channel,
             py::arg("channel"), py::arg("timeout_ms") = 1000)
        .def("close_channel", &ANTNative::close_channel,
             py::arg("channel"), py::arg("timeout_ms") = 1000)
        .def("send_broadcast", &ANTNative::send_broadcast,
             py::arg("channel"), py::arg("payload"))
        .def("send_acknowledged", &ANTNative::send_acknowledged,
             py::arg("channel"), py::arg("payload"), py::arg("timeout_ms") = 1000)
        .def("send_burst_packet", &ANTNative::send_burst_packet,
             py::arg("channel_sequence"), py::arg("payload"))
        .def("get_channel_number", &ANTNative::get_channel_number,
             py::arg("message_id"), py::arg("data"))
        .def("get_message_buffer_capacity", &ANTNative::get_message_buffer_capacity);
}
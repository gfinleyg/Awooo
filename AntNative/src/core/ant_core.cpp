#include "ant_core.hpp"

#include <algorithm>
#include <stdexcept>

#include "dsi_framer_ant.hpp"
#include "dsi_serial_generic.hpp"
#include "usb_device_handle.hpp"

static_assert(antnative::kFramerPass == ANTFRAMER_PASS, "ANT SDK framer result codes changed");
static_assert(antnative::kFramerInvalidParam == ANTFRAMER_INVALIDPARAM, "ANT SDK framer result codes changed");

namespace antnative {

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

// ANT+ profile page decoding. Byte offsets below were validated against,
// and where noted are a direct port of, the previous C/C++ receiver's live
// decode path (AntWorkout.cpp ProcessMessage), which is the field-tested
// ground truth for this hardware.
//
// What stays with the callers: anything *stateful* across messages -
// rollover-safe rate computation (speed_mps/cadence_rpm), wheel-
// circumference-dependent math (the wheel size is a runtime UI setting, not
// something this transport layer knows about), and redundant-message dedup.
// In Python those live in ant/profiles/*.py, operating on the fields
// decoded here.
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
void decode_heart_rate(const std::uint8_t* payload, DecodedMessage& message) {
    const std::uint8_t raw_page = payload[0];
    const std::uint8_t page = raw_page & 0x7F;
    message.page = static_cast<int>(page);
    message.page_change_toggle = (raw_page & 0x80) != 0;

    if (page > 4) {
        return;
    }

    const unsigned event_ticks = u16le(payload, 4);
    message.heart_beat_event_time = event_ticks / 1024.0;
    // Raw ticks alongside the seconds value so the rollover-safe unwrap can
    // work with exact integers instead of reversing a divided float back
    // into ticks.
    message.heart_beat_event_ticks = static_cast<int>(event_ticks);
    message.heart_beat_count = static_cast<int>(payload[6]);
    message.computed_heart_rate = static_cast<int>(payload[7]);

    if (page == 4) {
        const unsigned previous_ticks = u16le(payload, 2);
        message.previous_heart_beat_event_time = previous_ticks / 1024.0;
        const unsigned rr_ticks = (event_ticks - previous_ticks) & 0xFFFF;
        message.rr_interval = rr_ticks / 1024.0;
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
void decode_power(const std::uint8_t* payload, DecodedMessage& message) {
    const std::uint8_t raw_page = payload[0];
    const std::uint8_t page = raw_page & 0x7F;
    message.page = static_cast<int>(page);
    message.page_change_toggle = (raw_page & 0x80) != 0;

    if (page != kPowerOnlyPage) {
        return;
    }

    message.instantaneous_power_watts = static_cast<int>(u16le(payload, 6));

    // byte 2 is pedal power balance, byte 3 instantaneous cadence
    const std::uint8_t cadence_raw = payload[3];
    if (cadence_raw != 0xFF) {
        message.power_meter_cadence_rpm = static_cast<int>(cadence_raw);
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
void decode_speed_cadence_combined(const std::uint8_t* payload, DecodedMessage& message) {
    const unsigned cadence_ticks = u16le(payload, 0);
    const unsigned speed_ticks = u16le(payload, 4);
    message.cadence_event_time = cadence_ticks / 1024.0;
    message.cadence_event_ticks = static_cast<int>(cadence_ticks);
    message.cadence_revolution_count = static_cast<int>(u16le(payload, 2));
    message.speed_event_time = speed_ticks / 1024.0;
    message.speed_event_ticks = static_cast<int>(speed_ticks);
    message.speed_revolution_count = static_cast<int>(u16le(payload, 6));
}

// Single-function speed (123) / cadence (122): standard ANT+ BSC page 0
// carries event time + cumulative revolution count in bytes 4-7.
void decode_speed_cadence_single(const std::uint8_t* payload, DecodedMessage& message) {
    const std::uint8_t raw_page = payload[0];
    const std::uint8_t page = raw_page & 0x7F;
    message.page = static_cast<int>(page);
    message.page_change_toggle = (raw_page & 0x80) != 0;

    if (page != 0) {
        return;
    }

    const unsigned event_ticks = u16le(payload, 4);
    message.event_time = event_ticks / 1024.0;
    message.event_ticks = static_cast<int>(event_ticks);
    message.revolution_count = static_cast<int>(u16le(payload, 6));
}

void decode(int device_type, const std::uint8_t* payload, std::size_t payload_size,
            DecodedMessage& message) {
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

void require_payload_8(const std::vector<std::uint8_t>& value, const char* name) {
    if (value.size() != 8) {
        throw std::invalid_argument(std::string(name) + " must contain exactly 8 bytes");
    }
}

}  // namespace

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

DecodedMessage decode_message(const RawMessage& event) {
    DecodedMessage message;
    message.message_id = static_cast<int>(event.message_id);
    message.data = event.data;

    // ANT data messages have: data[0] = channel, data[1:9] = the fixed
    // 8-byte payload. Flagged extended data, when present, starts at data[9]
    // with the extended-message bitfield and then appends the fields
    // selected by that bitfield. The message ID remains 0x4E/0x4F/0x50; it
    // does NOT become 0x5D/0x5E/0x5F.
    const bool is_standard_data =
        event.message_id == 0x4E ||  // broadcast
        event.message_id == 0x4F ||  // acknowledged
        event.message_id == 0x50;    // burst packet

    const bool is_legacy_extended_data =
        event.message_id == 0x5D ||  // legacy extended broadcast
        event.message_id == 0x5E ||  // legacy extended acknowledged
        event.message_id == 0x5F;    // legacy extended burst

    const bool is_data_message = is_standard_data || is_legacy_extended_data;

    // Captured here so the profile decode at the end - which needs both the
    // payload bytes and the device_type extracted once extended data is
    // parsed - doesn't have to recompute anything.
    const std::uint8_t* payload_ptr = nullptr;
    std::size_t payload_len = 0;
    int decoded_device_type = -1;

    if (is_data_message && !event.data.empty()) {
        message.channel = event.has_channel ? static_cast<int>(event.channel)
                                            : static_cast<int>(event.data[0]);

        // Only the first 8 bytes after the channel are the ANT RF payload.
        // Do not expose appended extended data as profile data.
        const std::size_t payload_offset = 1;
        const std::size_t payload_size =
            event.data.size() > payload_offset
                ? std::min<std::size_t>(8, event.data.size() - payload_offset)
                : 0;
        message.payload = std::vector<std::uint8_t>(
            event.data.begin() + payload_offset,
            event.data.begin() + payload_offset + payload_size);

        payload_ptr = event.data.data() + payload_offset;
        payload_len = payload_size;
    }

    message.message_size = static_cast<int>(event.data.size());

    // Flagged extended messages use the normal data-message IDs. The ANT
    // protocol specifies that the message is longer than the normal 9-byte
    // message and data[9] is the extended-message bitfield.
    if (is_standard_data && event.data.size() >= 10) {
        const std::uint8_t ext_flags = event.data[9];
        const std::size_t ext_offset = 10;

        message.has_extended_data = true;
        message.extended_bitfield = static_cast<int>(ext_flags);
        message.has_device_id = (ext_flags & 0x80) != 0;
        message.has_rssi = (ext_flags & 0x40) != 0;
        message.has_timestamp = (ext_flags & 0x20) != 0;
        message.has_extension = (ext_flags & 0x01) != 0;
        message.extended_data = std::vector<std::uint8_t>(
            event.data.begin() + (std::min)(ext_offset, event.data.size()), event.data.end());

        // Extended fields are serialized in the order specified by the ANT
        // extended-message bitfield. Device ID is first, followed by RSSI
        // and timestamp when those flags are present. Only decode fields
        // whose flag is actually set; never assume bytes are present merely
        // because the message is long.
        std::size_t cursor = ext_offset;

        if ((ext_flags & 0x80) != 0 && cursor + 4 <= event.data.size()) {
            const std::uint8_t* id = event.data.data() + cursor;
            message.device_number =
                static_cast<unsigned int>(id[0]) |
                (static_cast<unsigned int>(id[1]) << 8);
            message.device_type = static_cast<int>(id[2]);
            message.transmission_type = static_cast<int>(id[3]);
            decoded_device_type = static_cast<int>(id[2]);
            cursor += 4;
        }

        if ((ext_flags & 0x40) != 0) {
            // RSSI field format is SDK/version dependent (3 or 4 bytes in
            // published ANT parameter sets). Preserve the complete remaining
            // extended data rather than guessing its layout.
            message.rssi_data = std::vector<std::uint8_t>(
                event.data.begin() + (std::min)(cursor, event.data.size()), event.data.end());
        }

        message.has_timestamp_field = (ext_flags & 0x20) != 0;
    }

    // Requested Channel ID response (0x51):
    // data[0] = channel, data[1:3] = device number (little endian),
    // data[3] = device type, data[4] = transmission type.
    if (event.message_id == 0x51 && event.data.size() >= 5) {
        message.channel = static_cast<int>(event.data[0]);
        message.device_number =
            static_cast<unsigned int>(event.data[1]) |
            (static_cast<unsigned int>(event.data[2]) << 8);
        message.device_type = static_cast<int>(event.data[3]);
        message.transmission_type = static_cast<int>(event.data[4]);
    }

    switch (event.message_id) {
        case 0x4D: message.message_name = "request_message"; break;
        case 0x4E: message.message_name = "broadcast_data"; break;
        case 0x4F: message.message_name = "acknowledged_data"; break;
        case 0x50: message.message_name = "burst_data"; break;
        case 0x5D: message.message_name = "extended_broadcast_data"; break;
        case 0x5E: message.message_name = "extended_acknowledged_data"; break;
        case 0x5F: message.message_name = "extended_burst_data"; break;
        case 0x40: message.message_name = "response_event"; break;
        default:   message.message_name = "ant_message"; break;
    }

    // RESPONSE_EVENT (0x40): channel-level response/event, not a per-device
    // broadcast - never has device identity. data[0] = channel, data[1] =
    // the message ID this responds to (0x01 for an unsolicited channel
    // event), data[2] = the response/event code itself.
    if (event.message_id == 0x40 && event.data.size() >= 3) {
        message.channel = static_cast<int>(event.data[0]);
        message.response_to_message_id = static_cast<int>(event.data[1]);
        message.event_code = static_cast<int>(event.data[2]);
        message.event_name = response_event_name(event.data[2]);
    }

    message.is_error = event.is_error;
    if (event.is_error) {
        message.error_code = static_cast<int>(event.error_code);
    }

    if (decoded_device_type != -1) {
        profile_decode::decode(decoded_device_type, payload_ptr, payload_len, message);
    }

    return message;
}

std::vector<UsbDeviceInfo> Device::list_devices() {
    // GetAllDevices() returns the SDK's current USB ANT device enumeration.
    // The returned USBDevice pointers are owned by the local ANTDeviceList and
    // must not be retained after this function returns. We copy only metadata.
    const ANTDeviceList devices = USBDeviceHandle::GetAllDevices();
    std::vector<UsbDeviceInfo> result;

    for (ULONG i = 0; i < devices.GetSize(); ++i) {
        const USBDevice* device = devices[i];
        if (device == nullptr) {
            continue;
        }

        UsbDeviceInfo item;
        item.index = static_cast<unsigned int>(i);
        item.vid = static_cast<unsigned int>(device->GetVid());
        item.pid = static_cast<unsigned int>(device->GetPid());
        item.serial_number = static_cast<unsigned long>(device->GetSerialNumber());
        item.device_type = static_cast<unsigned int>(device->GetDeviceType());

        UCHAR product[USB_MAX_STRLEN] = {};
        UCHAR serial[USB_MAX_STRLEN] = {};
        if (device->GetProductDescription(product, USB_MAX_STRLEN)) {
            item.product_description = std::string(reinterpret_cast<const char*>(product));
        }
        if (device->GetSerialString(serial, USB_MAX_STRLEN)) {
            item.serial_string = std::string(reinterpret_cast<const char*>(serial));
        }

        result.push_back(std::move(item));
    }

    return result;
}

Device::~Device() {
    close();
}

bool Device::open(std::uint32_t baudrate, std::uint8_t device_number) {
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

    cancel_ = false;
    opened_ = true;
    return true;
}

void Device::close() {
    stop_message_pump();

    // operation_mutex_ keeps another thread's in-flight blocking_call() from
    // using framer_ while it is deleted here.
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

bool Device::is_open() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return opened_;
}

bool Device::request_message(int channel, int message_id) {
    if (channel < 0 || channel > 255) {
        throw std::runtime_error("channel must be 0..255");
    }
    if (message_id < 0 || message_id > 255) {
        throw std::runtime_error("message_id must be 0..255");
    }

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

std::optional<ChannelId> Device::get_channel_id(int channel, std::uint32_t response_timeout_ms) {
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
        return std::nullopt;
    }
    return ChannelId{device_number, device_type, transmission_type};
}

void Device::start_message_pump(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(pump_mutex_);
    if (pump_running_) {
        return;
    }
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (!opened_ || framer_ == nullptr) {
            throw std::runtime_error("ANT device is not open");
        }
    }
    // Only ever set while no pump thread is running (stop_message_pump()
    // holds pump_mutex_ until its thread has finished), so the pump thread
    // reads it without a lock
    handler_ = std::move(handler);
    cancel_ = false;
    pump_running_ = true;
    pump_thread_ = std::thread(&Device::message_pump, this);
}

void Device::stop_message_pump() {
    // Held through the join, so a start can't begin until this pump thread
    // is gone. The handler must not start or stop the pump: it would wait
    // here on the thread it's running on.
    std::lock_guard<std::mutex> lock(pump_mutex_);
    if (!pump_running_) {
        return;
    }
    cancel_ = true;
    if (pump_thread_.joinable()) {
        pump_thread_.join();
    }
    pump_running_ = false;
    handler_ = nullptr;
}

bool Device::reset(std::uint32_t response_timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->ResetSystem(response_timeout_ms != 0 ? response_timeout_ms : 0);
    });
}

bool Device::set_network_key(std::uint8_t network_number, const std::vector<std::uint8_t>& key,
                             std::uint32_t response_timeout_ms) {
    if (key.size() != 8) {
        throw std::invalid_argument("ANT network key must contain exactly 8 bytes");
    }
    UCHAR key_data[8];
    std::copy(key.begin(), key.end(), key_data);
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SetNetworkKey(network_number, key_data, response_timeout_ms);
    });
}

bool Device::assign_channel(std::uint8_t channel, std::uint8_t channel_type,
                            std::uint8_t network_number, std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->AssignChannel(channel, channel_type, network_number, timeout_ms);
    });
}

bool Device::unassign_channel(std::uint8_t channel, std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->UnAssignChannel(channel, timeout_ms);
    });
}

bool Device::set_channel_id(std::uint8_t channel, std::uint16_t device_number,
                            std::uint8_t device_type, std::uint8_t transmission_type,
                            std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SetChannelID(channel, device_number, device_type,
                                    transmission_type, timeout_ms);
    });
}

bool Device::set_channel_period(std::uint8_t channel, std::uint16_t period,
                                std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SetChannelPeriod(channel, period, timeout_ms);
    });
}

bool Device::set_channel_rf_frequency(std::uint8_t channel, std::uint8_t rf_frequency,
                                      std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SetChannelRFFrequency(channel, rf_frequency, timeout_ms);
    });
}

bool Device::set_search_priority(std::uint8_t channel, std::uint8_t priority,
                                 std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SetChannelSearchPriority(channel, priority, timeout_ms);
    });
}

bool Device::set_search_timeout(std::uint8_t channel, std::uint8_t timeout,
                                std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SetChannelSearchTimeout(channel, timeout, timeout_ms);
    });
}

bool Device::set_low_priority_search_timeout(std::uint8_t channel, std::uint8_t timeout,
                                             std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SetLowPriorityChannelSearchTimeout(channel, timeout, timeout_ms);
    });
}

bool Device::set_rx_extended_messages_enabled(bool enabled, std::uint32_t timeout_ms) {
    // Vendor SDK API: RxExtMesgsEnable() forces the module to emit
    // extended receive messages when enabled.
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->RxExtMesgsEnable(enabled ? 1 : 0, timeout_ms);
    });
}

bool Device::open_rx_scan_mode(std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->OpenRxScanMode(timeout_ms);
    });
}

bool Device::open_channel(std::uint8_t channel, std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->OpenChannel(channel, timeout_ms);
    });
}

bool Device::close_channel(std::uint8_t channel, std::uint32_t timeout_ms) {
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->CloseChannel(channel, timeout_ms);
    });
}

bool Device::send_broadcast(std::uint8_t channel, const std::vector<std::uint8_t>& payload) {
    require_payload_8(payload, "broadcast payload");
    UCHAR data[8];
    std::copy(payload.begin(), payload.end(), data);
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SendBroadcastData(channel, data);
    });
}

int Device::send_acknowledged(std::uint8_t channel, const std::vector<std::uint8_t>& payload,
                              std::uint32_t timeout_ms) {
    require_payload_8(payload, "acknowledged payload");
    UCHAR data[8];
    std::copy(payload.begin(), payload.end(), data);
    return static_cast<int>(blocking_call([&](DSIFramerANT* framer) {
        return framer->SendAcknowledgedData(channel, data, timeout_ms);
    }));
}

bool Device::send_burst_packet(std::uint8_t channel_sequence, const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 8) {
        throw std::invalid_argument("burst packet payload must contain exactly 8 bytes");
    }
    UCHAR data[8];
    std::copy(payload.begin(), payload.end(), data);
    return blocking_call([&](DSIFramerANT* framer) {
        return framer->SendBurstDataPacket(channel_sequence, data);
    });
}

unsigned int Device::get_message_buffer_capacity() {
    return static_cast<unsigned int>(sizeof(ANT_MESSAGE{}.aucData));
}

unsigned int Device::get_channel_number(std::uint8_t message_id, const std::vector<std::uint8_t>& data) {
    if (data.size() > MESG_MAX_SIZE_VALUE) {
        throw std::invalid_argument("ANT message data is too large");
    }
    ANT_MESSAGE message{};
    message.ucMessageID = message_id;
    std::copy(data.begin(), data.end(), message.aucData);
    return static_cast<unsigned int>(require_framer()->GetChannelNumber(&message));
}

DSIFramerANT* Device::require_framer() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!opened_ || framer_ == nullptr) {
        throw std::runtime_error("ANT device is not open");
    }
    return framer_;
}

void Device::message_pump() {
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

        RawMessage event;
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

        handler_(event);
    }
}

}  // namespace antnative

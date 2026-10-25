// The AntNative C API (include/antnative.h): a C ABI over the AntNative core
// (src/core/ant_core.hpp), built as antnative.dll. Exceptions never cross
// it: each one becomes a return code plus ant_last_error().

#define ANTNATIVE_BUILDING
#include "antnative.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "../core/ant_core.hpp"

static_assert(ANT_TRANSFER_PASS == antnative::kFramerPass, "transfer result codes out of step with the core");
static_assert(ANT_TRANSFER_INVALID_PARAM == antnative::kFramerInvalidParam, "transfer result codes out of step with the core");

struct ant_device {
    antnative::Device device;
    std::mutex queue_mutex;
    std::deque<antnative::RawMessage> queue;
    std::uint64_t dropped{0};
};

namespace {

constexpr std::size_t kMaxQueuedMessages = 4096;

thread_local std::string last_error;

int fail(int code, const char* message) {
    last_error = message;
    return code;
}

// Runs `call`, turning exceptions into error codes
template <typename Call>
int guarded(Call&& call) {
    try {
        last_error.clear();
        return call();
    } catch (const std::invalid_argument& error) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, error.what());
    } catch (const std::exception& error) {
        return fail(ANT_ERROR, error.what());
    } catch (...) {
        return fail(ANT_ERROR, "unknown error");
    }
}

// For the calls that need the stick open: checked first so a closed device
// gets ANT_ERROR_NOT_OPEN rather than a generic error
template <typename Call>
int with_open_device(ant_device* dev, Call&& call) {
    if (dev == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_device is NULL");
    }
    if (!dev->device.is_open()) {
        return fail(ANT_ERROR_NOT_OPEN, "ANT device is not open");
    }
    return guarded([&] { return call(dev->device); });
}

int result(bool ok) {
    return ok ? ANT_OK : ANT_FAILED;
}

std::vector<std::uint8_t> payload8(const uint8_t* payload) {
    if (payload == nullptr) {
        throw std::invalid_argument("payload is NULL");
    }
    return std::vector<std::uint8_t>(payload, payload + 8);
}

template <std::size_t N>
std::uint8_t copy_bytes(const std::vector<std::uint8_t>& source, std::uint8_t (&destination)[N]) {
    const std::size_t size = std::min(source.size(), N);
    std::copy(source.begin(), source.begin() + size, destination);
    return static_cast<std::uint8_t>(size);
}

void copy_string(const std::string& source, char (&destination)[ANT_USB_STRING_MAX]) {
    const std::size_t size = std::min(source.size(), static_cast<std::size_t>(ANT_USB_STRING_MAX - 1));
    std::memcpy(destination, source.data(), size);
    destination[size] = '\0';
}

void to_message(const antnative::DecodedMessage& decoded, ant_message* out) {
    *out = ant_message{};
    out->message_id = static_cast<std::uint8_t>(decoded.message_id);
    out->message_name = decoded.message_name;
    out->data_size = copy_bytes(decoded.data, out->data);
    out->is_error = decoded.is_error ? 1 : 0;
    if (decoded.error_code) {
        out->fields |= ANT_FIELD_ERROR_CODE;
        out->error_code = static_cast<std::uint8_t>(*decoded.error_code);
    }
    if (decoded.channel) {
        out->fields |= ANT_FIELD_CHANNEL;
        out->channel = static_cast<std::uint8_t>(*decoded.channel);
    }
    if (decoded.payload) {
        out->fields |= ANT_FIELD_PAYLOAD;
        out->payload_size = copy_bytes(*decoded.payload, out->payload);
    }
    if (decoded.has_extended_data) {
        out->fields |= ANT_FIELD_EXTENDED;
        out->extended_bitfield = static_cast<std::uint8_t>(decoded.extended_bitfield.value_or(0));
        if (decoded.extended_data) {
            out->extended_data_size = copy_bytes(*decoded.extended_data, out->extended_data);
        }
    }
    if (decoded.rssi_data) {
        out->fields |= ANT_FIELD_RSSI;
        out->rssi_data_size = copy_bytes(*decoded.rssi_data, out->rssi_data);
    }
    if (decoded.device_number) {
        out->fields |= ANT_FIELD_DEVICE_ID;
        out->device_number = static_cast<std::uint16_t>(*decoded.device_number);
        out->device_type = static_cast<std::uint8_t>(decoded.device_type.value_or(0));
        out->transmission_type = static_cast<std::uint8_t>(decoded.transmission_type.value_or(0));
    }
    if (decoded.event_code) {
        out->fields |= ANT_FIELD_RESPONSE_EVENT;
        out->response_to_message_id = static_cast<std::uint8_t>(decoded.response_to_message_id.value_or(0));
        out->event_code = static_cast<std::uint8_t>(*decoded.event_code);
        out->event_name = decoded.event_name;
    }
    if (decoded.page) {
        out->fields |= ANT_FIELD_PAGE;
        out->page = static_cast<std::uint8_t>(*decoded.page);
        out->page_change_toggle = decoded.page_change_toggle.value_or(false) ? 1 : 0;
    }
    if (decoded.heart_beat_event_time) {
        out->fields |= ANT_FIELD_HEART_RATE;
        out->heart_beat_event_time = *decoded.heart_beat_event_time;
        out->heart_beat_event_ticks = static_cast<std::uint16_t>(decoded.heart_beat_event_ticks.value_or(0));
        out->heart_beat_count = static_cast<std::uint8_t>(decoded.heart_beat_count.value_or(0));
        out->computed_heart_rate = static_cast<std::uint8_t>(decoded.computed_heart_rate.value_or(0));
    }
    if (decoded.rr_interval) {
        out->fields |= ANT_FIELD_HEART_RATE_PREVIOUS;
        out->previous_heart_beat_event_time = decoded.previous_heart_beat_event_time.value_or(0.0);
        out->rr_interval = *decoded.rr_interval;
    }
    if (decoded.instantaneous_power_watts) {
        out->fields |= ANT_FIELD_POWER;
        out->instantaneous_power_watts = static_cast<std::uint16_t>(*decoded.instantaneous_power_watts);
    }
    if (decoded.power_meter_cadence_rpm) {
        out->fields |= ANT_FIELD_POWER_CADENCE;
        out->power_meter_cadence_rpm = static_cast<std::uint8_t>(*decoded.power_meter_cadence_rpm);
    }
    if (decoded.cadence_event_time) {
        out->fields |= ANT_FIELD_SPEED_CADENCE;
        out->cadence_event_time = *decoded.cadence_event_time;
        out->cadence_event_ticks = static_cast<std::uint16_t>(decoded.cadence_event_ticks.value_or(0));
        out->cadence_revolution_count = static_cast<std::uint16_t>(decoded.cadence_revolution_count.value_or(0));
        out->speed_event_time = decoded.speed_event_time.value_or(0.0);
        out->speed_event_ticks = static_cast<std::uint16_t>(decoded.speed_event_ticks.value_or(0));
        out->speed_revolution_count = static_cast<std::uint16_t>(decoded.speed_revolution_count.value_or(0));
    }
    if (decoded.event_time) {
        out->fields |= ANT_FIELD_SPEED_OR_CADENCE;
        out->event_time = *decoded.event_time;
        out->event_ticks = static_cast<std::uint16_t>(decoded.event_ticks.value_or(0));
        out->revolution_count = static_cast<std::uint16_t>(decoded.revolution_count.value_or(0));
    }
}

}  // namespace

extern "C" {

int ant_api_version(void) {
    return ANT_API_VERSION;
}

const char* ant_last_error(void) {
    return last_error.c_str();
}

int ant_list_devices(ant_usb_device* out, int max) {
    if (max < 0 || (max > 0 && out == nullptr)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "out is NULL or max is negative");
    }
    return guarded([&] {
        const std::vector<antnative::UsbDeviceInfo> devices = antnative::Device::list_devices();
        const std::size_t count = std::min(devices.size(), static_cast<std::size_t>(max));
        for (std::size_t i = 0; i < count; ++i) {
            const antnative::UsbDeviceInfo& device = devices[i];
            ant_usb_device& item = out[i];
            item = ant_usb_device{};
            item.index = device.index;
            item.vid = device.vid;
            item.pid = device.pid;
            item.serial_number = static_cast<uint32_t>(device.serial_number);
            item.device_type = device.device_type;
            if (device.product_description) {
                item.has_product_description = 1;
                copy_string(*device.product_description, item.product_description);
            }
            if (device.serial_string) {
                item.has_serial_string = 1;
                copy_string(*device.serial_string, item.serial_string);
            }
        }
        return static_cast<int>(devices.size());
    });
}

ant_device* ant_create(void) {
    try {
        last_error.clear();
        return new ant_device();
    } catch (const std::exception& error) {
        last_error = error.what();
        return nullptr;
    }
}

void ant_destroy(ant_device* dev) {
    // ~Device stops the pump and closes the stick
    delete dev;
}

int ant_open(ant_device* dev, uint32_t baudrate, uint8_t device_number) {
    if (dev == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_device is NULL");
    }
    return guarded([&] { return result(dev->device.open(baudrate, device_number)); });
}

void ant_close(ant_device* dev) {
    if (dev != nullptr) {
        dev->device.close();
    }
}

int ant_is_open(ant_device* dev) {
    return dev != nullptr && dev->device.is_open() ? 1 : 0;
}

int ant_reset(ant_device* dev, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.reset(timeout_ms));
    });
}

int ant_set_network_key(ant_device* dev, uint8_t network_number, const uint8_t key[8], uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        if (key == nullptr) {
            throw std::invalid_argument("key is NULL");
        }
        return result(device.set_network_key(network_number, std::vector<std::uint8_t>(key, key + 8), timeout_ms));
    });
}

int ant_assign_channel(ant_device* dev, uint8_t channel, uint8_t channel_type, uint8_t network_number, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.assign_channel(channel, channel_type, network_number, timeout_ms));
    });
}

int ant_unassign_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.unassign_channel(channel, timeout_ms));
    });
}

int ant_set_channel_id(ant_device* dev, uint8_t channel, uint16_t device_number, uint8_t device_type, uint8_t transmission_type, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.set_channel_id(channel, device_number, device_type, transmission_type, timeout_ms));
    });
}

int ant_set_channel_period(ant_device* dev, uint8_t channel, uint16_t period, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.set_channel_period(channel, period, timeout_ms));
    });
}

int ant_set_channel_rf_frequency(ant_device* dev, uint8_t channel, uint8_t rf_frequency, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.set_channel_rf_frequency(channel, rf_frequency, timeout_ms));
    });
}

int ant_set_search_priority(ant_device* dev, uint8_t channel, uint8_t priority, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.set_search_priority(channel, priority, timeout_ms));
    });
}

int ant_set_search_timeout(ant_device* dev, uint8_t channel, uint8_t timeout, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.set_search_timeout(channel, timeout, timeout_ms));
    });
}

int ant_set_low_priority_search_timeout(ant_device* dev, uint8_t channel, uint8_t timeout, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.set_low_priority_search_timeout(channel, timeout, timeout_ms));
    });
}

int ant_set_rx_extended_messages_enabled(ant_device* dev, int enabled, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.set_rx_extended_messages_enabled(enabled != 0, timeout_ms));
    });
}

int ant_open_rx_scan_mode(ant_device* dev, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.open_rx_scan_mode(timeout_ms));
    });
}

int ant_open_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.open_channel(channel, timeout_ms));
    });
}

int ant_close_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.close_channel(channel, timeout_ms));
    });
}

int ant_request_message(ant_device* dev, uint8_t channel, uint8_t message_id) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.request_message(channel, message_id));
    });
}

int ant_get_channel_id(ant_device* dev, uint8_t channel, uint32_t timeout_ms, ant_channel_id* out) {
    if (out == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "out is NULL");
    }
    return with_open_device(dev, [&](antnative::Device& device) {
        const std::optional<antnative::ChannelId> id = device.get_channel_id(channel, timeout_ms);
        if (!id) {
            return ANT_FAILED;
        }
        out->device_number = static_cast<uint16_t>(id->device_number);
        out->device_type = static_cast<uint8_t>(id->device_type);
        out->transmission_type = static_cast<uint8_t>(id->transmission_type);
        return ANT_OK;
    });
}

int ant_send_broadcast(ant_device* dev, uint8_t channel, const uint8_t payload[8]) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.send_broadcast(channel, payload8(payload)));
    });
}

int ant_send_acknowledged(ant_device* dev, uint8_t channel, const uint8_t payload[8], uint32_t timeout_ms, int* transfer_result) {
    return with_open_device(dev, [&](antnative::Device& device) {
        const int transfer = device.send_acknowledged(channel, payload8(payload), timeout_ms);
        if (transfer_result != nullptr) {
            *transfer_result = transfer;
        }
        return result(transfer == ANT_TRANSFER_PASS);
    });
}

int ant_send_burst_packet(ant_device* dev, uint8_t channel_sequence, const uint8_t payload[8]) {
    return with_open_device(dev, [&](antnative::Device& device) {
        return result(device.send_burst_packet(channel_sequence, payload8(payload)));
    });
}

int ant_start_messages(ant_device* dev) {
    return with_open_device(dev, [&](antnative::Device& device) {
        {
            std::lock_guard<std::mutex> lock(dev->queue_mutex);
            dev->queue.clear();
            dev->dropped = 0;
        }
        device.start_message_pump([dev](const antnative::RawMessage& message) {
            std::lock_guard<std::mutex> lock(dev->queue_mutex);
            if (dev->queue.size() >= kMaxQueuedMessages) {
                dev->queue.pop_front();
                ++dev->dropped;
            }
            dev->queue.push_back(message);
        });
        return ANT_OK;
    });
}

void ant_stop_messages(ant_device* dev) {
    if (dev != nullptr) {
        dev->device.stop_message_pump();
    }
}

int ant_poll(ant_device* dev, ant_message* out, int max) {
    if (dev == nullptr || max < 0 || (max > 0 && out == nullptr)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_device or out is NULL, or max is negative");
    }
    return guarded([&] {
        // Taken off the queue first, so the pump thread isn't kept waiting
        // while they're decoded
        std::vector<antnative::RawMessage> taken;
        {
            std::lock_guard<std::mutex> lock(dev->queue_mutex);
            const std::size_t count = std::min(dev->queue.size(), static_cast<std::size_t>(max));
            taken.assign(std::make_move_iterator(dev->queue.begin()),
                         std::make_move_iterator(dev->queue.begin() + count));
            dev->queue.erase(dev->queue.begin(), dev->queue.begin() + count);
        }
        for (std::size_t i = 0; i < taken.size(); ++i) {
            to_message(antnative::decode_message(taken[i]), &out[i]);
        }
        return static_cast<int>(taken.size());
    });
}

uint64_t ant_dropped_messages(ant_device* dev) {
    if (dev == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(dev->queue_mutex);
    return dev->dropped;
}

int ant_decode_message(uint8_t message_id, const uint8_t* data, size_t data_size,
                       int channel, ant_message* out) {
    if (out == nullptr || (data == nullptr && data_size > 0)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "out or data is NULL");
    }
    if (data_size > ANT_MESSAGE_DATA_MAX) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "data is longer than an ANT message");
    }
    if (channel > 255) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "channel must be 0..255, or negative for unknown");
    }
    return guarded([&] {
        antnative::RawMessage raw;
        raw.message_id = message_id;
        raw.data.assign(data, data + data_size);
        if (channel >= 0) {
            raw.channel = static_cast<std::uint8_t>(channel);
            raw.has_channel = true;
        }
        to_message(antnative::decode_message(raw), out);
        return ANT_OK;
    });
}

}  // extern "C"

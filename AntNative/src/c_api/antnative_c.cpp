// The AntNative C API (include/antnative.h): a C ABI over the AntNative core
// (src/core/ant_core.hpp), built as antnative.dll. Exceptions never cross
// it: each one becomes a return code plus ant_last_error().

#define ANTNATIVE_BUILDING
#include "antnative.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "../core/ant_core.hpp"
#include "../core/ant_manager.hpp"
#include "../core/ant_pages.hpp"
#include "../core/ant_profiles.hpp"
#include "../core/ant_recovery.hpp"

static_assert(ANT_TRANSFER_PASS == antnative::kFramerPass, "transfer result codes out of step with the core");
static_assert(ANT_TRANSFER_INVALID_PARAM == antnative::kFramerInvalidParam, "transfer result codes out of step with the core");

struct ant_device {
    antnative::Stick device;
    std::mutex queue_mutex;
    std::deque<antnative::RawMessage> queue;
    std::uint64_t dropped{0};
};

struct ant_manager {
    std::shared_ptr<antnative::Stick> stick{std::make_shared<antnative::Stick>()};
    std::shared_ptr<antnative::StickTransport> transport;
    std::unique_ptr<antnative::Manager> manager;

    std::mutex recovery_mutex;
    std::shared_ptr<antnative::Recovery> recovery;

    std::mutex queue_mutex;
    std::deque<antnative::RawMessage> queue;

    std::mutex log_mutex;
    std::deque<std::string> log_lines;
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
    } catch (const antnative::NotOpenError& error) {
        return fail(ANT_ERROR_NOT_OPEN, error.what());
    } catch (const antnative::ConfigurationError& error) {
        // the stick refused part of the set-up
        return fail(ANT_FAILED, error.what());
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

void to_page(const antnative::PageData& data, ant_page* out) {
    *out = ant_page{};
    out->page = data.page;
    out->name = data.name;
    for (const antnative::PageField& field : data.fields) {
        if (out->field_count == ANT_PAGE_FIELDS_MAX) {
            break;
        }
        ant_page_field& f = out->fields[out->field_count++];
        f.name = field.name;
        if (const auto* integer = std::get_if<long long>(&field.value)) {
            f.type = ANT_VALUE_INT;
            f.int_value = *integer;
            f.value = static_cast<double>(*integer);
        } else if (const auto* number = std::get_if<double>(&field.value)) {
            f.type = ANT_VALUE_DOUBLE;
            f.value = *number;
        } else {
            f.type = ANT_VALUE_BOOL;
            f.int_value = std::get<bool>(field.value) ? 1 : 0;
            f.value = static_cast<double>(f.int_value);
        }
    }
}

void to_message(const antnative::DecodedMessage& decoded, ant_message* out) {
    *out = ant_message{};
    if (decoded.page_data) {
        out->fields |= ANT_FIELD_PAGE_DATA;
        to_page(*decoded.page_data, &out->page_data);
    } else {
        out->page_data.page = -1;
        out->page_data.name = "unknown";
    }
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

constexpr std::size_t kMaxLogLines = 256;

void copy_page(const std::vector<std::uint8_t>& page, uint8_t* out) {
    if (out != nullptr) {
        std::copy(page.begin(), page.begin() + 8, out);
    }
}

// NAN means "send the field's default"
std::optional<double> given(double value) {
    return std::isnan(value) ? std::nullopt : std::optional<double>(value);
}

// The manager's message pump: each message to its sensor, channel events to
// the watchdog, and into the queue for ant_manager_poll
void manager_message(ant_manager* m, const antnative::RawMessage& raw) {
    const antnative::DecodedMessage decoded = antnative::decode_message(raw);
    m->manager->on_message(decoded);
    if (decoded.event_name != nullptr) {
        std::shared_ptr<antnative::Recovery> recovery;
        {
            std::lock_guard<std::mutex> lock(m->recovery_mutex);
            recovery = m->recovery;
        }
        if (recovery) {
            recovery->notify_channel_event(decoded.event_name, decoded.channel);
        }
    }
    std::lock_guard<std::mutex> lock(m->queue_mutex);
    if (m->queue.size() >= kMaxQueuedMessages) {
        m->queue.pop_front();
    }
    m->queue.push_back(raw);
}

void to_sensor(antnative::Manager& manager, antnative::Sensor& sensor, double now, ant_sensor* out) {
    using namespace antnative;
    *out = ant_sensor{};
    out->device_number = static_cast<uint16_t>(sensor.device_number());
    out->device_type = static_cast<uint8_t>(sensor.device_type());
    out->transmission_type = static_cast<uint8_t>(sensor.transmission_type());
    out->channel = manager.channel_for(sensor.device_type(), sensor.device_number()).value_or(-1);
    const std::string state = sensor.get(&Sensor::state);
    out->state = state == "LOST" ? ANT_SENSOR_LOST : state == "STALE" ? ANT_SENSOR_STALE : ANT_SENSOR_ACTIVE;
    out->seconds_since_heard = now - sensor.get(&Sensor::last_seen);
    out->packet_count = sensor.get(&Sensor::packet_count);
    out->data_stalled = sensor.get(&Sensor::data_stalled) ? 1 : 0;
    out->maximum_resistance_n = -1;
    out->battery_level_percent = -1;
    out->battery_voltage = std::nan("");

    if (const std::optional<BatteryStatus> battery = sensor.battery()) {
        out->fields |= ANT_READING_BATTERY;
        out->battery_status = battery->status;
        out->battery_level_percent = battery->level_percent.value_or(-1);
        out->battery_voltage = battery->voltage.value_or(std::nan(""));
    }

    const std::shared_ptr<Profile>& profile = sensor.profile();
    if (!profile) {
        return;
    }
    std::lock_guard<std::mutex> lock(profile->mutex);
    auto cadence = [out](std::optional<double> rpm) {
        if (rpm) {
            out->fields |= ANT_READING_CADENCE;
            out->cadence_rpm = *rpm;
        }
    };
    auto speed = [out](std::optional<double> mps) {
        if (mps) {
            out->fields |= ANT_READING_SPEED;
            out->speed_mps = *mps;
        }
    };
    if (auto* hr = dynamic_cast<HeartRateProfile*>(profile.get())) {
        if (hr->latest) {
            out->fields |= ANT_READING_HEART_RATE;
            out->heart_rate = hr->latest->computed_heart_rate;
            out->heart_beat_count = hr->latest->heart_beat_count;
            if (hr->latest->rr_interval) {
                out->fields |= ANT_READING_RR_INTERVAL;
                out->rr_interval = *hr->latest->rr_interval;
            }
        }
    } else if (auto* power = dynamic_cast<PowerMeterProfile*>(profile.get())) {
        if (power->latest) {
            out->fields |= ANT_READING_POWER;
            out->power_watts = power->latest->instantaneous_power_watts;
            if (power->latest->power_meter_cadence_rpm) {
                cadence(static_cast<double>(*power->latest->power_meter_cadence_rpm));
            }
        }
    } else if (auto* bsc = dynamic_cast<SpeedCadenceProfile*>(profile.get())) {
        // held: no gaps when a message repeats the last event, 0 once stopped
        cadence(bsc->held_cadence_rpm());
        speed(bsc->held_speed_mps());
    } else if (auto* fe = dynamic_cast<FitnessEquipmentProfile*>(profile.get())) {
        if (fe->latest) {
            out->fields |= ANT_READING_POWER | ANT_READING_TRAINER;
            out->power_watts = fe->latest->instantaneous_power_watts;
            out->trainer_status = fe->latest->trainer_status;
            if (fe->latest->power_meter_cadence_rpm) {
                cadence(static_cast<double>(*fe->latest->power_meter_cadence_rpm));
            }
        }
        if (fe->capabilities) {
            out->fields |= ANT_READING_CAPABILITIES;
            out->supports_basic_resistance = fe->capabilities->supports_basic_resistance;
            out->supports_target_power = fe->capabilities->supports_target_power;
            out->supports_simulation = fe->capabilities->supports_simulation;
            out->maximum_resistance_n = fe->capabilities->maximum_resistance_newtons.value_or(-1);
        }
        // FE-C general data (page 16) carries the trainer's speed
        const auto general = fe->pages.find(0x10);
        if (general != fe->pages.end()) {
            if (const PageField* field = general->second.find("speed_mps")) {
                speed(std::get<double>(field->value));
            }
        }
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
        const std::vector<antnative::UsbDeviceInfo> devices = antnative::Stick::list_devices();
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
    // ~Stick stops the pump and closes the stick
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
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.reset(timeout_ms));
    });
}

int ant_set_network_key(ant_device* dev, uint8_t network_number, const uint8_t key[8], uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        if (key == nullptr) {
            throw std::invalid_argument("key is NULL");
        }
        return result(device.set_network_key(network_number, std::vector<std::uint8_t>(key, key + 8), timeout_ms));
    });
}

int ant_assign_channel(ant_device* dev, uint8_t channel, uint8_t channel_type, uint8_t network_number, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.assign_channel(channel, channel_type, network_number, timeout_ms));
    });
}

int ant_unassign_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.unassign_channel(channel, timeout_ms));
    });
}

int ant_set_channel_id(ant_device* dev, uint8_t channel, uint16_t device_number, uint8_t device_type, uint8_t transmission_type, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.set_channel_id(channel, device_number, device_type, transmission_type, timeout_ms));
    });
}

int ant_set_channel_period(ant_device* dev, uint8_t channel, uint16_t period, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.set_channel_period(channel, period, timeout_ms));
    });
}

int ant_set_channel_rf_frequency(ant_device* dev, uint8_t channel, uint8_t rf_frequency, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.set_channel_rf_frequency(channel, rf_frequency, timeout_ms));
    });
}

int ant_set_search_priority(ant_device* dev, uint8_t channel, uint8_t priority, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.set_search_priority(channel, priority, timeout_ms));
    });
}

int ant_set_search_timeout(ant_device* dev, uint8_t channel, uint8_t timeout, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.set_search_timeout(channel, timeout, timeout_ms));
    });
}

int ant_set_low_priority_search_timeout(ant_device* dev, uint8_t channel, uint8_t timeout, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.set_low_priority_search_timeout(channel, timeout, timeout_ms));
    });
}

int ant_set_rx_extended_messages_enabled(ant_device* dev, int enabled, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.set_rx_extended_messages_enabled(enabled != 0, timeout_ms));
    });
}

int ant_open_rx_scan_mode(ant_device* dev, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.open_rx_scan_mode(timeout_ms));
    });
}

int ant_open_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.open_channel(channel, timeout_ms));
    });
}

int ant_close_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.close_channel(channel, timeout_ms));
    });
}

int ant_request_message(ant_device* dev, uint8_t channel, uint8_t message_id) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.request_message(channel, message_id));
    });
}

int ant_get_channel_id(ant_device* dev, uint8_t channel, uint32_t timeout_ms, ant_channel_id* out) {
    if (out == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "out is NULL");
    }
    return with_open_device(dev, [&](antnative::Stick& device) {
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
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.send_broadcast(channel, payload8(payload)));
    });
}

int ant_send_acknowledged(ant_device* dev, uint8_t channel, const uint8_t payload[8], uint32_t timeout_ms, int* transfer_result) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        const int transfer = device.send_acknowledged(channel, payload8(payload), timeout_ms);
        if (transfer_result != nullptr) {
            *transfer_result = transfer;
        }
        return result(transfer == ANT_TRANSFER_PASS);
    });
}

int ant_send_burst_packet(ant_device* dev, uint8_t channel_sequence, const uint8_t payload[8]) {
    return with_open_device(dev, [&](antnative::Stick& device) {
        return result(device.send_burst_packet(channel_sequence, payload8(payload)));
    });
}

int ant_start_messages(ant_device* dev) {
    return with_open_device(dev, [&](antnative::Stick& device) {
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

// ---- Pages -------------------------------------------------------------------

int ant_decode_page(int device_type, const uint8_t payload[8], ant_page* out) {
    if (payload == nullptr || out == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "payload or out is NULL");
    }
    return guarded([&] {
        const antnative::PageData data = antnative::decode_page(device_type, payload, 8);
        to_page(data, out);
        return data.page < 0 ? ANT_FAILED : ANT_OK;
    });
}

int ant_page_get_field(const ant_page* page, const char* name, double* value) {
    if (page == nullptr || name == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "page or name is NULL");
    }
    for (int i = 0; i < page->field_count && i < ANT_PAGE_FIELDS_MAX; ++i) {
        if (page->fields[i].name != nullptr && std::strcmp(page->fields[i].name, name) == 0) {
            if (value != nullptr) {
                *value = page->fields[i].value;
            }
            return ANT_OK;
        }
    }
    return ANT_FAILED;
}

void ant_page_basic_resistance(double percent, uint8_t out[8]) {
    copy_page(antnative::pages::basic_resistance(percent), out);
}

void ant_page_target_power(double watts, uint8_t out[8]) {
    copy_page(antnative::pages::target_power(watts), out);
}

void ant_page_wind_resistance(double coefficient_kg_per_m, double wind_speed_kmh, double drafting_factor,
                              uint8_t out[8]) {
    copy_page(antnative::pages::wind_resistance(given(coefficient_kg_per_m), given(wind_speed_kmh),
                                                given(drafting_factor)),
              out);
}

void ant_page_track_resistance(double grade_percent, double rolling_resistance, uint8_t out[8]) {
    copy_page(antnative::pages::track_resistance(given(grade_percent), given(rolling_resistance)), out);
}

void ant_page_user_configuration(double user_weight_kg, double bicycle_weight_kg, double wheel_diameter_m,
                                 double gear_ratio, uint8_t out[8]) {
    copy_page(antnative::pages::user_configuration(given(user_weight_kg), given(bicycle_weight_kg),
                                                   given(wheel_diameter_m), given(gear_ratio), std::nullopt),
              out);
}

void ant_page_request(uint8_t page, uint8_t times, int acknowledged, int descriptor_1, int descriptor_2,
                      uint8_t out[8]) {
    auto descriptor = [](int value) { return value < 0 ? std::nullopt : std::optional<int>(value); };
    copy_page(antnative::pages::request_page(page, times, acknowledged != 0, descriptor(descriptor_1),
                                             descriptor(descriptor_2)),
              out);
}

void ant_page_mode_settings(uint8_t sport_mode, uint8_t out[8]) {
    copy_page(antnative::pages::mode_settings(sport_mode), out);
}

void ant_page_heart_rate_feature_command(int gym_mode, uint8_t out[8]) {
    copy_page(antnative::pages::heart_rate_feature_command(gym_mode < 0 ? std::nullopt
                                                                        : std::optional<bool>(gym_mode != 0)),
              out);
}

void ant_page_power_manual_zero(uint8_t out[8]) {
    copy_page(antnative::pages::power_manual_zero(), out);
}

void ant_page_power_auto_zero(int enabled, uint8_t out[8]) {
    copy_page(antnative::pages::power_auto_zero(enabled != 0), out);
}

void ant_page_power_crank_length(double millimetres, uint8_t out[8]) {
    copy_page(antnative::pages::power_crank_length(given(millimetres)), out);
}

void ant_page_power_cycling_dynamics(int eight_hz, int power_phase, int pedal_center_offset, int rider_position,
                                     int torque_barycenter, uint8_t out[8]) {
    copy_page(antnative::pages::power_cycling_dynamics(eight_hz != 0, power_phase != 0, pedal_center_offset != 0,
                                                       rider_position != 0, torque_barycenter != 0),
              out);
}

// ---- Sensors -------------------------------------------------------------------

ant_manager* ant_manager_create(void) {
    try {
        last_error.clear();
        auto* m = new ant_manager();
        antnative::LogFn log = [m](const std::string& line) {
            std::lock_guard<std::mutex> lock(m->log_mutex);
            m->log_lines.push_back(line);
            if (m->log_lines.size() > kMaxLogLines) {
                m->log_lines.pop_front();
            }
        };
        m->transport = std::make_shared<antnative::StickTransport>(
            m->stick, [m](const antnative::RawMessage& raw) { manager_message(m, raw); });
        m->manager = std::make_unique<antnative::Manager>(m->transport, antnative::steady_seconds,
                                                          antnative::sleep_seconds, antnative::steady_seconds, log);
        return m;
    } catch (const std::exception& error) {
        last_error = error.what();
        return nullptr;
    }
}

void ant_manager_destroy(ant_manager* m) {
    if (m == nullptr) {
        return;
    }
    ant_manager_stop_recovery(m);
    try {
        m->manager->close();
    } catch (...) {
    }
    delete m;
}

int ant_manager_open(ant_manager* m, int device_number) {
    if (m == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager is NULL");
    }
    return guarded([&] {
        const std::optional<int> number = device_number < 0 ? std::nullopt : std::optional<int>(device_number);
        return result(m->manager->open(57600, number));
    });
}

void ant_manager_close(ant_manager* m) {
    if (m == nullptr) {
        return;
    }
    ant_manager_stop_recovery(m);
    guarded([&] {
        m->manager->close();
        return ANT_OK;
    });
}

int ant_manager_configure_scan(ant_manager* m, const uint8_t key[8], uint8_t network_number, uint8_t channel,
                               uint8_t rf_frequency) {
    if (m == nullptr || key == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or key is NULL");
    }
    return guarded([&] {
        m->manager->configure_scan(std::vector<std::uint8_t>(key, key + 8), network_number, channel, rf_frequency);
        return ANT_OK;
    });
}

int ant_manager_start(ant_manager* m) {
    if (m == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager is NULL");
    }
    return guarded([&] {
        m->manager->start();
        return ANT_OK;
    });
}

void ant_manager_stop(ant_manager* m) {
    if (m != nullptr) {
        guarded([&] {
            m->manager->stop();
            return ANT_OK;
        });
    }
}

int ant_manager_open_device_channels(ant_manager* m, const ant_sensor_id* sensors, int count) {
    if (m == nullptr || count < 0 || (count > 0 && sensors == nullptr)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or sensors is NULL, or count is negative");
    }
    return guarded([&] {
        std::vector<antnative::ChannelIdentity> identities;
        for (int i = 0; i < count; ++i) {
            identities.emplace_back(sensors[i].device_type, sensors[i].device_number, sensors[i].transmission_type);
        }
        m->manager->open_device_channels(identities);
        return ANT_OK;
    });
}

int ant_manager_return_to_scan(ant_manager* m) {
    if (m == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager is NULL");
    }
    return guarded([&] {
        m->manager->return_to_scan();
        return ANT_OK;
    });
}

int ant_manager_channel_for(ant_manager* m, uint8_t device_type, uint16_t device_number) {
    if (m == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager is NULL");
    }
    return m->manager->channel_for(device_type, device_number).value_or(-1);
}

int ant_manager_send(ant_manager* m, uint8_t device_type, uint16_t device_number, const uint8_t payload[8],
                     uint32_t timeout_ms, int* transfer_result) {
    if (m == nullptr || payload == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or payload is NULL");
    }
    return guarded([&] {
        const std::optional<int> channel = m->manager->channel_for(device_type, device_number);
        if (!channel) {
            return fail(ANT_FAILED, "the sensor has no dedicated channel (ant_manager_open_device_channels)");
        }
        const int transfer = m->transport->send_acknowledged(*channel, payload8(payload), timeout_ms);
        if (transfer_result != nullptr) {
            *transfer_result = transfer;
        }
        return result(transfer == ANT_TRANSFER_PASS);
    });
}

int ant_manager_sensors(ant_manager* m, ant_sensor* out, int max) {
    if (m == nullptr || max < 0 || (max > 0 && out == nullptr)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or out is NULL, or max is negative");
    }
    return guarded([&] {
        const std::vector<std::shared_ptr<antnative::Sensor>> sensors = m->manager->get_devices();
        const double now = antnative::steady_seconds();
        for (std::size_t i = 0; i < sensors.size() && i < static_cast<std::size_t>(max); ++i) {
            to_sensor(*m->manager, *sensors[i], now, &out[i]);
        }
        return static_cast<int>(sensors.size());
    });
}

int ant_manager_sensor_page(ant_manager* m, uint8_t device_type, uint16_t device_number, int page, ant_page* out) {
    if (m == nullptr || out == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or out is NULL");
    }
    return guarded([&] {
        const std::shared_ptr<antnative::Sensor> sensor = m->manager->find_sensor(device_type, device_number);
        if (!sensor || !sensor->profile()) {
            return ANT_FAILED;
        }
        std::lock_guard<std::mutex> lock(sensor->profile()->mutex);
        const auto found = sensor->profile()->pages.find(page);
        if (found == sensor->profile()->pages.end()) {
            return ANT_FAILED;
        }
        to_page(found->second, out);
        return ANT_OK;
    });
}

int ant_manager_set_wheel_circumference(ant_manager* m, uint16_t device_number, double meters) {
    if (m == nullptr || !(meters > 0)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager is NULL or meters isn't positive");
    }
    return guarded([&] {
        for (int type : {antnative::kDeviceTypeSpeed, antnative::kDeviceTypeSpeedCadence}) {
            const std::shared_ptr<antnative::Sensor> sensor = m->manager->find_sensor(type, device_number);
            if (sensor && sensor->profile()) {
                auto* bsc = dynamic_cast<antnative::SpeedCadenceProfile*>(sensor->profile().get());
                std::lock_guard<std::mutex> lock(sensor->profile()->mutex);
                bsc->wheel_circumference_m = meters;
                return ANT_OK;
            }
        }
        return fail(ANT_FAILED, "no speed sensor with that device number has been heard");
    });
}

int ant_manager_poll(ant_manager* m, ant_message* out, int max) {
    if (m == nullptr || max < 0 || (max > 0 && out == nullptr)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or out is NULL, or max is negative");
    }
    return guarded([&] {
        std::vector<antnative::RawMessage> taken;
        {
            std::lock_guard<std::mutex> lock(m->queue_mutex);
            const std::size_t count = std::min(m->queue.size(), static_cast<std::size_t>(max));
            taken.assign(std::make_move_iterator(m->queue.begin()), std::make_move_iterator(m->queue.begin() + count));
            m->queue.erase(m->queue.begin(), m->queue.begin() + count);
        }
        for (std::size_t i = 0; i < taken.size(); ++i) {
            to_message(antnative::decode_message(taken[i]), &out[i]);
        }
        return static_cast<int>(taken.size());
    });
}

void ant_recovery_default_options(ant_recovery_options* out) {
    if (out == nullptr) {
        return;
    }
    const antnative::RecoveryOptions d;
    *out = ant_recovery_options{d.stale_timeout, d.lost_timeout, d.interval, d.channel_retry_limit,
                                d.channel_retry_cooldown, d.device_reset_timeout, d.device_reset_cooldown,
                                d.channel_reopen_wait, d.channel_stick_reset_cooldown, d.channel_open_retry_interval,
                                d.hr_frozen_timeout, d.value_frozen_timeout};
}

int ant_manager_start_recovery(ant_manager* m, const ant_recovery_options* options) {
    if (m == nullptr) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager is NULL");
    }
    return guarded([&] {
        antnative::RecoveryOptions o;
        if (options != nullptr) {
            o = antnative::RecoveryOptions{options->stale_timeout, options->lost_timeout, options->interval,
                                           options->channel_retry_limit, options->channel_retry_cooldown,
                                           options->device_reset_timeout, options->device_reset_cooldown,
                                           options->channel_reopen_wait, options->channel_stick_reset_cooldown,
                                           options->channel_open_retry_interval, options->hr_frozen_timeout,
                                           options->value_frozen_timeout};
        }
        std::lock_guard<std::mutex> lock(m->recovery_mutex);
        if (!m->recovery) {
            m->recovery = std::make_shared<antnative::Recovery>(
                *m->manager, o, antnative::steady_seconds, [m](const std::string& line) {
                    std::lock_guard<std::mutex> log_lock(m->log_mutex);
                    m->log_lines.push_back(line);
                    if (m->log_lines.size() > kMaxLogLines) {
                        m->log_lines.pop_front();
                    }
                });
        } else {
            m->recovery->set_options(o);
        }
        m->recovery->start();
        return ANT_OK;
    });
}

void ant_manager_stop_recovery(ant_manager* m) {
    if (m == nullptr) {
        return;
    }
    std::shared_ptr<antnative::Recovery> recovery;
    {
        std::lock_guard<std::mutex> lock(m->recovery_mutex);
        recovery = m->recovery;
    }
    if (recovery) {
        recovery->stop();
    }
}

int ant_manager_watch(ant_manager* m, const ant_sensor_id* sensors, int count) {
    if (m == nullptr || (count > 0 && sensors == nullptr)) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or sensors is NULL");
    }
    std::lock_guard<std::mutex> lock(m->recovery_mutex);
    if (!m->recovery) {
        return fail(ANT_ERROR, "the watchdog isn't started (ant_manager_start_recovery)");
    }
    if (count < 0) {
        m->recovery->set_watched_devices(std::nullopt);
    } else {
        std::set<std::pair<int, int>> watched;
        for (int i = 0; i < count; ++i) {
            watched.insert({sensors[i].device_type, sensors[i].device_number});
        }
        m->recovery->set_watched_devices(watched);
    }
    return ANT_OK;
}

int ant_manager_next_log(ant_manager* m, char* buffer, int size) {
    if (m == nullptr || buffer == nullptr || size <= 0) {
        return fail(ANT_ERROR_INVALID_ARGUMENT, "ant_manager or buffer is NULL, or size isn't positive");
    }
    std::string line;
    {
        std::lock_guard<std::mutex> lock(m->log_mutex);
        if (m->log_lines.empty()) {
            buffer[0] = '\0';
            return 0;
        }
        line = std::move(m->log_lines.front());
        m->log_lines.pop_front();
    }
    const std::size_t length = std::min(line.size(), static_cast<std::size_t>(size - 1));
    std::memcpy(buffer, line.data(), length);
    buffer[length] = '\0';
    return static_cast<int>(length);
}

}  // extern "C"

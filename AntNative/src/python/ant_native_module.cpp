// The ant_native Python module: a pybind11 layer over the AntNative core
// (src/core/ant_core.hpp). Everything ANT-specific lives in the core; this
// file only converts between Python and C++ types and manages the GIL.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../core/ant_core.hpp"

namespace py = pybind11;

namespace {

using antnative::DecodedMessage;
using antnative::Device;
using antnative::RawMessage;

std::vector<std::uint8_t> to_vector(const py::bytes& value) {
    const std::string text = value;
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

py::bytes to_bytes(const std::vector<std::uint8_t>& value) {
    return py::bytes(reinterpret_cast<const char*>(value.data()), value.size());
}

template <typename T>
void put(py::dict& message, const char* key, const std::optional<T>& value) {
    if (value) {
        message[key] = *value;
    }
}

void put(py::dict& message, const char* key, const std::optional<std::vector<std::uint8_t>>& value) {
    if (value) {
        message[key] = to_bytes(*value);
    }
}

// The message dict the pump's callback gets: a key for every field the
// decode filled
py::dict to_dict(const DecodedMessage& decoded) {
    py::dict message;
    message["message_id"] = decoded.message_id;
    message["data"] = to_bytes(decoded.data);
    put(message, "channel", decoded.channel);
    put(message, "payload", decoded.payload);
    message["message_size"] = decoded.message_size;

    message["has_extended_data"] = decoded.has_extended_data;
    put(message, "extended_bitfield", decoded.extended_bitfield);
    put(message, "has_device_id", decoded.has_device_id);
    put(message, "has_rssi", decoded.has_rssi);
    put(message, "has_timestamp", decoded.has_timestamp);
    put(message, "has_extension", decoded.has_extension);
    put(message, "extended_data", decoded.extended_data);
    put(message, "device_number", decoded.device_number);
    put(message, "device_type", decoded.device_type);
    put(message, "transmission_type", decoded.transmission_type);
    put(message, "rssi_data", decoded.rssi_data);
    put(message, "has_timestamp_field", decoded.has_timestamp_field);

    message["message_name"] = decoded.message_name;
    put(message, "response_to_message_id", decoded.response_to_message_id);
    put(message, "event_code", decoded.event_code);
    if (decoded.event_name != nullptr) {
        message["event_name"] = decoded.event_name;
    }

    message["is_error"] = decoded.is_error;
    put(message, "error_code", decoded.error_code);

    put(message, "page", decoded.page);
    put(message, "page_change_toggle", decoded.page_change_toggle);
    put(message, "heart_beat_event_time", decoded.heart_beat_event_time);
    put(message, "heart_beat_event_ticks", decoded.heart_beat_event_ticks);
    put(message, "heart_beat_count", decoded.heart_beat_count);
    put(message, "computed_heart_rate", decoded.computed_heart_rate);
    put(message, "previous_heart_beat_event_time", decoded.previous_heart_beat_event_time);
    put(message, "rr_interval", decoded.rr_interval);
    put(message, "instantaneous_power_watts", decoded.instantaneous_power_watts);
    put(message, "power_meter_cadence_rpm", decoded.power_meter_cadence_rpm);
    put(message, "cadence_event_time", decoded.cadence_event_time);
    put(message, "cadence_event_ticks", decoded.cadence_event_ticks);
    put(message, "cadence_revolution_count", decoded.cadence_revolution_count);
    put(message, "speed_event_time", decoded.speed_event_time);
    put(message, "speed_event_ticks", decoded.speed_event_ticks);
    put(message, "speed_revolution_count", decoded.speed_revolution_count);
    put(message, "event_time", decoded.event_time);
    put(message, "event_ticks", decoded.event_ticks);
    put(message, "revolution_count", decoded.revolution_count);
    return message;
}

py::list list_devices() {
    // USB enumeration can block, so it runs without the GIL; the Python
    // objects below are only built once the GIL is back.
    const std::vector<antnative::UsbDeviceInfo> devices = [] {
        py::gil_scoped_release release;
        return Device::list_devices();
    }();

    py::list result;
    for (const antnative::UsbDeviceInfo& device : devices) {
        py::dict item;
        item["index"] = device.index;
        item["vid"] = device.vid;
        item["pid"] = device.pid;
        item["serial_number"] = device.serial_number;
        item["device_type"] = device.device_type;
        item["product_description"] =
            device.product_description ? py::object(py::str(*device.product_description)) : py::none();
        item["serial_string"] =
            device.serial_string ? py::object(py::str(*device.serial_string)) : py::none();
        result.append(item);
    }
    return result;
}

// Every blocking call (anything that waits on the stick) runs with the GIL
// released, so the UI and the message pump's callback keep running
// meanwhile.
class ANTNative {
public:
    ANTNative() = default;

    ~ANTNative() {
        close();
    }

    ANTNative(const ANTNative&) = delete;
    ANTNative& operator=(const ANTNative&) = delete;

    bool open(std::uint32_t baudrate, std::uint8_t device_number) {
        py::gil_scoped_release release;
        return device_.open(baudrate, device_number);
    }

    void close() {
        stop_message_pump();
        py::gil_scoped_release release;
        device_.close();
    }

    bool is_open() const {
        return device_.is_open();
    }

    bool request_message(int channel, int message_id) {
        py::gil_scoped_release release;
        return device_.request_message(channel, message_id);
    }

    py::dict get_channel_id(int channel, std::uint32_t response_timeout_ms) {
        std::optional<antnative::ChannelId> id;
        {
            py::gil_scoped_release release;
            id = device_.get_channel_id(channel, response_timeout_ms);
        }
        if (!id) {
            throw std::runtime_error("ANT GetChannelID request failed or timed out");
        }

        py::dict result;
        result["channel"] = channel;
        result["device_number"] = id->device_number;
        result["device_type"] = id->device_type;
        result["transmission_type"] = id->transmission_type;
        return result;
    }

    void start_message_pump(py::object callback) {
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback_ = std::move(callback);
        }
        // Not holding the GIL: a stop_message_pump() on another thread may
        // be joining the pump thread, which needs the GIL to finish its
        // last callback.
        py::gil_scoped_release release;
        device_.start_message_pump([this](const RawMessage& event) { dispatch_message(event); });
    }

    void stop_message_pump() {
        // Nor while joining: the pump thread takes the GIL to dispatch each
        // message, so joining it while holding the GIL deadlocks the whole
        // process the moment a message arrives during the join.
        {
            py::gil_scoped_release release;
            device_.stop_message_pump();
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

    bool reset(std::uint32_t response_timeout_ms) {
        py::gil_scoped_release release;
        return device_.reset(response_timeout_ms);
    }

    bool set_network_key(std::uint8_t network_number, py::bytes key, std::uint32_t response_timeout_ms) {
        const std::vector<std::uint8_t> value = to_vector(key);
        py::gil_scoped_release release;
        return device_.set_network_key(network_number, value, response_timeout_ms);
    }

    bool assign_channel(std::uint8_t channel, std::uint8_t channel_type,
                        std::uint8_t network_number, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.assign_channel(channel, channel_type, network_number, timeout_ms);
    }

    bool unassign_channel(std::uint8_t channel, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.unassign_channel(channel, timeout_ms);
    }

    bool set_channel_id(std::uint8_t channel, std::uint16_t device_number,
                        std::uint8_t device_type, std::uint8_t transmission_type,
                        std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.set_channel_id(channel, device_number, device_type, transmission_type, timeout_ms);
    }

    bool set_channel_period(std::uint8_t channel, std::uint16_t period, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.set_channel_period(channel, period, timeout_ms);
    }

    bool set_channel_rf_frequency(std::uint8_t channel, std::uint8_t rf_frequency, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.set_channel_rf_frequency(channel, rf_frequency, timeout_ms);
    }

    bool set_search_priority(std::uint8_t channel, std::uint8_t priority, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.set_search_priority(channel, priority, timeout_ms);
    }

    bool set_search_timeout(std::uint8_t channel, std::uint8_t timeout, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.set_search_timeout(channel, timeout, timeout_ms);
    }

    bool set_low_priority_search_timeout(std::uint8_t channel, std::uint8_t timeout, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.set_low_priority_search_timeout(channel, timeout, timeout_ms);
    }

    bool set_rx_extended_messages_enabled(bool enabled, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.set_rx_extended_messages_enabled(enabled, timeout_ms);
    }

    bool open_rx_scan_mode(std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.open_rx_scan_mode(timeout_ms);
    }

    bool open_channel(std::uint8_t channel, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.open_channel(channel, timeout_ms);
    }

    bool close_channel(std::uint8_t channel, std::uint32_t timeout_ms) {
        py::gil_scoped_release release;
        return device_.close_channel(channel, timeout_ms);
    }

    bool send_broadcast(std::uint8_t channel, py::bytes payload) {
        const std::vector<std::uint8_t> value = to_vector(payload);
        py::gil_scoped_release release;
        return device_.send_broadcast(channel, value);
    }

    int send_acknowledged(std::uint8_t channel, py::bytes payload, std::uint32_t timeout_ms) {
        const std::vector<std::uint8_t> value = to_vector(payload);
        py::gil_scoped_release release;
        return device_.send_acknowledged(channel, value, timeout_ms);
    }

    bool send_burst_packet(std::uint8_t channel_sequence, py::bytes payload) {
        const std::vector<std::uint8_t> value = to_vector(payload);
        py::gil_scoped_release release;
        return device_.send_burst_packet(channel_sequence, value);
    }

    unsigned int get_message_buffer_capacity() const {
        return Device::get_message_buffer_capacity();
    }

    unsigned int get_channel_number(std::uint8_t message_id, py::bytes data) {
        return device_.get_channel_number(message_id, to_vector(data));
    }

private:
    // Runs on the pump thread, once per message
    void dispatch_message(const RawMessage& event) {
        // The GIL comes first: copying (and later releasing) the callback
        // changes its Python refcount, which is only safe while holding it.
        // Lock order is always GIL, then callback_mutex_.
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
            callback(to_dict(antnative::decode_message(event)));
        } catch (const py::error_already_set& error) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback_error_ = error.what();
        }
    }

    Device device_;
    mutable std::mutex callback_mutex_;
    py::object callback_{py::none()};
    std::string callback_error_;
};

// Test hook: the message dict the pump's callback would get for one raw
// message, without a stick
py::dict decode_message(int message_id, py::bytes data, py::object channel,
                        bool is_error, int error_code) {
    RawMessage event;
    event.message_id = static_cast<std::uint8_t>(message_id);
    event.data = to_vector(data);
    if (!channel.is_none()) {
        event.channel = static_cast<std::uint8_t>(channel.cast<int>());
        event.has_channel = true;
    }
    event.is_error = is_error;
    event.error_code = static_cast<std::uint8_t>(error_code);
    return to_dict(antnative::decode_message(event));
}

}  // namespace

PYBIND11_MODULE(ant_native, module) {
    module.doc() = "Minimal Python binding for the ANT SDK DSIFramerANT transport layer.";

    module.def("list_devices", &list_devices);
    module.def("_decode_message", &decode_message,
               py::arg("message_id"), py::arg("data"), py::arg("channel") = py::none(),
               py::arg("is_error") = false, py::arg("error_code") = 0);

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

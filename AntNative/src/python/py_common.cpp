#include "py_common.hpp"

#include <optional>
#include <string>
#include <variant>

namespace antnative_py {

using antnative::DecodedMessage;

std::vector<std::uint8_t> to_vector(const py::bytes& value) {
    const std::string text = value;
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

py::bytes to_bytes(const std::vector<std::uint8_t>& value) {
    return py::bytes(reinterpret_cast<const char*>(value.data()), value.size());
}

std::vector<std::uint8_t> bytes_of(py::handle value) {
    if (py::isinstance<py::bytes>(value)) {
        return to_vector(py::reinterpret_borrow<py::bytes>(value));
    }
    py::bytes converted = py::module_::import("builtins").attr("bytes")(value);
    return to_vector(converted);
}

py::object field_value(const antnative::FieldValue& value) {
    if (const auto* integer = std::get_if<long long>(&value)) {
        return py::int_(*integer);
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return py::float_(*number);
    }
    return py::bool_(std::get<bool>(value));
}

py::dict page_dict(const antnative::PageData& page) {
    py::dict result;
    result["page"] = page.page;
    result["page_name"] = page.name;
    for (const antnative::PageField& field : page.fields) {
        result[field.name] = field_value(field.value);
    }
    return result;
}

namespace {

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

// The dict's value for `key`, unless it's absent or None
std::optional<py::object> item(const py::dict& message, const char* key) {
    if (!message.contains(key)) {
        return std::nullopt;
    }
    py::object value = message[key];
    if (value.is_none()) {
        return std::nullopt;
    }
    return value;
}

void take(const py::dict& message, const char* key, std::optional<int>& field) {
    if (auto value = item(message, key)) field = value->cast<int>();
}

void take(const py::dict& message, const char* key, std::optional<unsigned int>& field) {
    if (auto value = item(message, key)) field = value->cast<unsigned int>();
}

void take(const py::dict& message, const char* key, std::optional<double>& field) {
    if (auto value = item(message, key)) field = value->cast<double>();
}

void take(const py::dict& message, const char* key, std::optional<bool>& field) {
    if (auto value = item(message, key)) field = py::bool_(*value);
}

void take(const py::dict& message, const char* key, std::optional<std::vector<std::uint8_t>>& field) {
    if (auto value = item(message, key)) field = bytes_of(*value);
}

}  // namespace

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

    if (decoded.page_data) {
        message["page_data"] = page_dict(*decoded.page_data);
    }
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

DecodedMessage message_from_dict(py::handle message_handle) {
    DecodedMessage decoded;
    if (!py::isinstance<py::dict>(message_handle)) {
        return decoded;
    }
    const py::dict message = py::reinterpret_borrow<py::dict>(message_handle);

    std::optional<int> message_id, message_size, error_code;
    take(message, "message_id", message_id);
    decoded.message_id = message_id.value_or(0);
    if (auto data = item(message, "data")) decoded.data = bytes_of(*data);
    take(message, "message_size", message_size);
    decoded.message_size = message_size.value_or(static_cast<int>(decoded.data.size()));
    take(message, "channel", decoded.channel);
    take(message, "payload", decoded.payload);

    std::optional<bool> has_extended_data, is_error;
    take(message, "has_extended_data", has_extended_data);
    decoded.has_extended_data = has_extended_data.value_or(false);
    take(message, "extended_bitfield", decoded.extended_bitfield);
    take(message, "has_device_id", decoded.has_device_id);
    take(message, "has_rssi", decoded.has_rssi);
    take(message, "has_timestamp", decoded.has_timestamp);
    take(message, "has_extension", decoded.has_extension);
    take(message, "extended_data", decoded.extended_data);
    take(message, "device_number", decoded.device_number);
    take(message, "device_type", decoded.device_type);
    take(message, "transmission_type", decoded.transmission_type);
    take(message, "rssi_data", decoded.rssi_data);
    take(message, "has_timestamp_field", decoded.has_timestamp_field);

    take(message, "response_to_message_id", decoded.response_to_message_id);
    take(message, "event_code", decoded.event_code);
    take(message, "is_error", is_error);
    decoded.is_error = is_error.value_or(false);
    take(message, "error_code", error_code);
    decoded.error_code = error_code;

    take(message, "page", decoded.page);
    take(message, "page_change_toggle", decoded.page_change_toggle);
    take(message, "heart_beat_event_time", decoded.heart_beat_event_time);
    take(message, "heart_beat_event_ticks", decoded.heart_beat_event_ticks);
    take(message, "heart_beat_count", decoded.heart_beat_count);
    take(message, "computed_heart_rate", decoded.computed_heart_rate);
    take(message, "previous_heart_beat_event_time", decoded.previous_heart_beat_event_time);
    take(message, "rr_interval", decoded.rr_interval);
    take(message, "instantaneous_power_watts", decoded.instantaneous_power_watts);
    take(message, "power_meter_cadence_rpm", decoded.power_meter_cadence_rpm);
    take(message, "cadence_event_time", decoded.cadence_event_time);
    take(message, "cadence_event_ticks", decoded.cadence_event_ticks);
    take(message, "cadence_revolution_count", decoded.cadence_revolution_count);
    take(message, "speed_event_time", decoded.speed_event_time);
    take(message, "speed_event_ticks", decoded.speed_event_ticks);
    take(message, "speed_revolution_count", decoded.speed_revolution_count);
    take(message, "event_time", decoded.event_time);
    take(message, "event_ticks", decoded.event_ticks);
    take(message, "revolution_count", decoded.revolution_count);
    return decoded;
}

std::shared_ptr<py::object> hold(py::object object) {
    return std::shared_ptr<py::object>(new py::object(std::move(object)), [](py::object* held) {
        py::gil_scoped_acquire gil;
        delete held;
    });
}

antnative::ClockFn py_clock(py::object fn) {
    auto held = hold(std::move(fn));
    return [held]() {
        py::gil_scoped_acquire gil;
        return (*held)().cast<double>();
    };
}

antnative::SleepFn py_sleep(py::object fn) {
    auto held = hold(std::move(fn));
    return [held](double seconds) {
        py::gil_scoped_acquire gil;
        (*held)(seconds);
    };
}

antnative::LogFn py_log(py::object fn) {
    if (fn.is_none()) {
        return nullptr;
    }
    auto held = hold(std::move(fn));
    return [held](const std::string& message) {
        py::gil_scoped_acquire gil;
        (*held)(message);
    };
}

}  // namespace antnative_py

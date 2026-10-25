// The sensor-level classes of the ant_native module: Manager, Sensor, the
// profiles, Recovery, page decoding and building. The Python ant package
// (python/ant) wraps these in its original classes, so its API is unchanged.
//
// GIL rules: a binding that can wait on the core's configuration lock (every
// channel (re)configuration, Recovery.check()) releases the GIL first, since
// the thread holding that lock may be calling back into Python. Everything
// else runs with the GIL held: the core never calls out while holding the
// other locks those take.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../core/ant_manager.hpp"
#include "../core/ant_pages.hpp"
#include "../core/ant_profiles.hpp"
#include "../core/ant_recovery.hpp"
#include "py_common.hpp"

namespace antnative_py {

namespace {

using namespace antnative;
using release_gil = py::call_guard<py::gil_scoped_release>;

// ---- The stick, as the Python layer was given it ---------------------------

// A Transport over whatever `native` object the Python ANTManager was given:
// the real module's NativeTransport or the tests' FakeNative. Every call is
// made exactly as the Python manager made it, with the GIL taken around it.
class PyTransport : public Transport {
public:
    PyTransport(py::object native, py::object sink) : native_(hold(std::move(native))), sink_(hold(std::move(sink))) {}

    std::vector<UsbDeviceInfo> list_devices() override {
        py::gil_scoped_acquire gil;
        std::vector<UsbDeviceInfo> devices;
        py::object listed = native_->attr("list_devices")();
        for (py::handle entry : listed) {
            UsbDeviceInfo info;
            info.index = entry["index"].cast<unsigned int>();
            py::object description = py::reinterpret_borrow<py::object>(entry).attr("get")("product_description");
            if (!description.is_none()) {
                info.product_description = py::str(description).cast<std::string>();
            }
            devices.push_back(std::move(info));
        }
        return devices;
    }
    bool open(std::uint32_t baudrate, int device_number) override { return truthy("open", baudrate, device_number); }
    void close() override { call("close"); }
    bool is_open() override { return truthy("is_open"); }
    bool reset() override { return truthy("reset"); }
    bool set_network_key(int network_number, const std::vector<std::uint8_t>& key) override {
        py::gil_scoped_acquire gil;
        py::object result = native_->attr("set_network_key")(network_number, to_bytes(key));
        return PyObject_IsTrue(result.ptr()) == 1;
    }
    bool assign_channel(int channel, int channel_type, int network_number) override {
        return truthy("assign_channel", channel, channel_type, network_number);
    }
    bool unassign_channel(int channel) override { return truthy("unassign_channel", channel); }
    bool set_channel_id(int channel, int device_number, int device_type, int transmission_type) override {
        return truthy("set_channel_id", channel, device_number, device_type, transmission_type);
    }
    bool set_channel_period(int channel, int period) override { return truthy("set_channel_period", channel, period); }
    bool set_channel_rf_frequency(int channel, int frequency) override {
        return truthy("set_channel_rf_frequency", channel, frequency);
    }
    bool set_search_timeout(int channel, int timeout) override { return truthy("set_search_timeout", channel, timeout); }
    bool set_low_priority_search_timeout(int channel, int timeout) override {
        return truthy("set_low_priority_search_timeout", channel, timeout);
    }
    bool set_rx_extended_messages_enabled(bool enabled) override {
        return truthy("set_rx_extended_messages_enabled", enabled);
    }
    bool open_rx_scan_mode() override { return truthy("open_rx_scan_mode"); }
    bool open_channel(int channel) override { return truthy("open_channel", channel); }
    bool close_channel(int channel) override { return truthy("close_channel", channel); }
    void start_message_pump() override {
        py::gil_scoped_acquire gil;
        native_->attr("start_message_pump")(*sink_);
    }
    void stop_message_pump() override { call("stop_message_pump"); }
    int send_acknowledged(int channel, const std::vector<std::uint8_t>& payload, std::uint32_t timeout_ms) override {
        py::gil_scoped_acquire gil;
        return native_->attr("send_acknowledged")(channel, to_bytes(payload), timeout_ms).cast<int>();
    }

private:
    template <typename... Args>
    void call(const char* name, Args... args) {
        py::gil_scoped_acquire gil;
        native_->attr(name)(args...);
    }

    // The manager tested results for truth (`if not fn(*args)`), so the same here
    template <typename... Args>
    bool truthy(const char* name, Args... args) {
        py::gil_scoped_acquire gil;
        py::object result = native_->attr(name)(args...);
        return PyObject_IsTrue(result.ptr()) == 1;
    }

    std::shared_ptr<py::object> native_;
    std::shared_ptr<py::object> sink_;
};

// ---- Conversions -------------------------------------------------------------

py::object battery_dict(const std::optional<BatteryStatus>& battery) {
    if (!battery) {
        return py::none();
    }
    py::dict result;
    result["status"] = battery->status;
    result["level_percent"] = battery->level_percent;
    result["voltage"] = battery->voltage;
    result["battery_id"] = battery->battery_id;
    return std::move(result);
}

py::dict pages_dict(const std::map<int, PageData>& pages) {
    py::dict result;
    for (const auto& [page, data] : pages) {
        result[py::int_(page)] = page_dict(data);
    }
    return result;
}

py::object raw_page(const std::optional<Profile::RawPage>& raw) {
    if (!raw) {
        return py::none();
    }
    return py::make_tuple(raw->page, raw->raw_page, raw->page_change_toggle, to_bytes(raw->payload));
}

py::object heart_rate_reading(const std::optional<HeartRateReading>& r) {
    if (!r) return py::none();
    py::dict d;
    d["page"] = r->page;
    d["heart_beat_count"] = r->heart_beat_count;
    d["heart_beat_event_time"] = r->heart_beat_event_time;
    d["computed_heart_rate"] = r->computed_heart_rate;
    d["previous_heart_beat_event_time"] = r->previous_heart_beat_event_time;
    d["rr_interval"] = r->rr_interval;
    d["event_time_unwrapped"] = r->event_time_unwrapped;
    return std::move(d);
}

py::object power_reading(const std::optional<PowerReading>& r) {
    if (!r) return py::none();
    py::dict d;
    d["page"] = r->page;
    d["instantaneous_power_watts"] = r->instantaneous_power_watts;
    d["power_meter_cadence_rpm"] = r->power_meter_cadence_rpm;
    return std::move(d);
}

py::object speed_cadence_reading(const SpeedCadenceProfile& p) {
    if (p.device_type() == kDeviceTypeSpeedCadence) {
        if (!p.latest_combined) return py::none();
        const CombinedSpeedCadenceReading& r = *p.latest_combined;
        py::dict d;
        d["cadence_event_time"] = r.cadence_event_time;
        d["cadence_revolution_count"] = r.cadence_revolution_count;
        d["speed_event_time"] = r.speed_event_time;
        d["speed_revolution_count"] = r.speed_revolution_count;
        d["cadence_event_ticks"] = r.cadence_event_ticks;
        d["speed_event_ticks"] = r.speed_event_ticks;
        d["cadence_rpm"] = r.cadence_rpm;
        d["speed_mps"] = r.speed_mps;
        d["speed_kph"] = r.speed_kph;
        d["cadence_event_time_unwrapped"] = r.cadence_event_time_unwrapped;
        d["speed_event_time_unwrapped"] = r.speed_event_time_unwrapped;
        return std::move(d);
    }
    if (!p.latest) return py::none();
    const SpeedCadenceReading& r = *p.latest;
    py::dict d;
    d["page"] = r.page;
    d["event_time"] = r.event_time;
    d["revolution_count"] = r.revolution_count;
    d["event_ticks"] = r.event_ticks;
    d["speed_mps"] = r.speed_mps;
    d["speed_kph"] = r.speed_kph;
    d["cadence_rpm"] = r.cadence_rpm;
    d["event_time_unwrapped"] = r.event_time_unwrapped;
    return std::move(d);
}

py::object fe_reading(const std::optional<FitnessEquipmentReading>& r) {
    if (!r) return py::none();
    py::dict d;
    d["page"] = r->page;
    d["instantaneous_power_watts"] = r->instantaneous_power_watts;
    d["power_meter_cadence_rpm"] = r->power_meter_cadence_rpm;
    d["trainer_status"] = r->trainer_status;
    return std::move(d);
}

py::object fe_capabilities(const std::optional<FitnessEquipmentCapabilities>& c) {
    if (!c) return py::none();
    py::dict d;
    d["maximum_resistance_newtons"] = c->maximum_resistance_newtons;
    d["supports_basic_resistance"] = c->supports_basic_resistance;
    d["supports_target_power"] = c->supports_target_power;
    d["supports_simulation"] = c->supports_simulation;
    return std::move(d);
}

// A getter that reads a profile under its lock
template <typename P, typename F>
auto locked(F read) {
    return [read](const P& profile) {
        std::lock_guard<std::mutex> lock(profile.mutex);
        return read(profile);
    };
}

template <typename T>
void sensor_field(py::class_<Sensor, std::shared_ptr<Sensor>>& cls, const char* name, T Sensor::*field) {
    cls.def_property(
        name, [field](const Sensor& sensor) { return sensor.get(field); },
        [field](Sensor& sensor, T value) { sensor.set(field, std::move(value)); });
}

std::vector<ChannelIdentity> identities_of(py::iterable identities) {
    std::vector<ChannelIdentity> result;
    for (py::handle identity : identities) {
        py::tuple t = py::tuple(py::reinterpret_borrow<py::object>(identity));
        result.emplace_back(t[0].cast<int>(), t[1].cast<int>(), t[2].cast<int>());
    }
    return result;
}

py::object scan_config_dict(const std::optional<ScanConfig>& config) {
    if (!config) return py::none();
    py::dict d;
    d["network_key"] = to_bytes(config->network_key);
    d["network_number"] = config->network_number;
    d["channel"] = config->channel;
    d["rf_frequency"] = config->rf_frequency;
    return std::move(d);
}

RecoveryOptions options_from(const py::dict& values, RecoveryOptions options) {
    auto number = [&](const char* key, double& field) {
        if (values.contains(key)) field = values[key].cast<double>();
    };
    number("stale_timeout", options.stale_timeout);
    number("lost_timeout", options.lost_timeout);
    number("interval", options.interval);
    if (values.contains("channel_retry_limit")) options.channel_retry_limit = values["channel_retry_limit"].cast<int>();
    number("channel_retry_cooldown", options.channel_retry_cooldown);
    number("device_reset_timeout", options.device_reset_timeout);
    number("device_reset_cooldown", options.device_reset_cooldown);
    number("channel_reopen_wait", options.channel_reopen_wait);
    number("channel_stick_reset_cooldown", options.channel_stick_reset_cooldown);
    number("channel_open_retry_interval", options.channel_open_retry_interval);
    number("hr_frozen_timeout", options.hr_frozen_timeout);
    number("value_frozen_timeout", options.value_frozen_timeout);
    return options;
}

py::dict options_dict(const RecoveryOptions& o) {
    py::dict d;
    d["stale_timeout"] = o.stale_timeout;
    d["lost_timeout"] = o.lost_timeout;
    d["interval"] = o.interval;
    d["channel_retry_limit"] = o.channel_retry_limit;
    d["channel_retry_cooldown"] = o.channel_retry_cooldown;
    d["device_reset_timeout"] = o.device_reset_timeout;
    d["device_reset_cooldown"] = o.device_reset_cooldown;
    d["channel_reopen_wait"] = o.channel_reopen_wait;
    d["channel_stick_reset_cooldown"] = o.channel_stick_reset_cooldown;
    d["channel_open_retry_interval"] = o.channel_open_retry_interval;
    d["hr_frozen_timeout"] = o.hr_frozen_timeout;
    d["value_frozen_timeout"] = o.value_frozen_timeout;
    return d;
}

// The core's errors as the ant package's exception classes
void set_ant_error(const char* name, const char* what) {
    try {
        py::object cls = py::module_::import("ant.exceptions").attr(name);
        PyErr_SetString(cls.ptr(), what);
    } catch (py::error_already_set&) {
        PyErr_SetString(PyExc_RuntimeError, what);
    }
}

}  // namespace

void bind_sensors(py::module_& m) {
    py::register_exception_translator([](std::exception_ptr error) {
        try {
            if (error) std::rethrow_exception(error);
        } catch (const ConfigurationError& e) {
            set_ant_error("ANTConfigurationError", e.what());
        } catch (const NotOpenError& e) {
            set_ant_error("ANTNotOpenError", e.what());
        }
    });

    // ---- Pages -------------------------------------------------------------

    m.def("decode_page", [](int device_type, py::handle payload) -> py::object {
        const std::vector<std::uint8_t> bytes = bytes_of(payload);
        PageData page = decode_page(device_type, bytes.data(), bytes.size());
        if (page.page < 0) return py::none();
        return page_dict(page);
    }, py::arg("device_type"), py::arg("payload"));
    m.def("decode_battery", [](int device_type, py::handle payload) -> py::object {
        if (payload.is_none()) return py::none();
        const std::vector<std::uint8_t> bytes = bytes_of(payload);
        return battery_dict(decode_battery(device_type, &bytes));
    }, py::arg("device_type"), py::arg("payload"));

    m.def("page_basic_resistance", [](double percent) { return to_bytes(pages::basic_resistance(percent)); },
          py::arg("percent"));
    m.def("page_target_power", [](double watts) { return to_bytes(pages::target_power(watts)); }, py::arg("watts"));
    m.def("page_wind_resistance", [](std::optional<double> coefficient, std::optional<double> wind_speed,
                                     std::optional<double> drafting) {
        return to_bytes(pages::wind_resistance(coefficient, wind_speed, drafting));
    }, py::arg("coefficient_kg_per_m") = py::none(), py::arg("wind_speed_kmh") = py::none(),
       py::arg("drafting_factor") = py::none());
    m.def("page_track_resistance", [](std::optional<double> grade, std::optional<double> rolling) {
        return to_bytes(pages::track_resistance(grade, rolling));
    }, py::arg("grade_percent") = py::none(), py::arg("rolling_resistance") = py::none());
    m.def("page_user_configuration", [](std::optional<double> user, std::optional<double> bicycle,
                                        std::optional<double> wheel, std::optional<double> gear,
                                        std::optional<int> offset) {
        return to_bytes(pages::user_configuration(user, bicycle, wheel, gear, offset));
    }, py::arg("user_weight_kg") = py::none(), py::arg("bicycle_weight_kg") = py::none(),
       py::arg("wheel_diameter_m") = py::none(), py::arg("gear_ratio") = py::none(),
       py::arg("wheel_diameter_offset_mm") = py::none());
    m.def("page_request", [](int page, int times, bool acknowledged, std::optional<int> descriptor_1,
                             std::optional<int> descriptor_2) {
        return to_bytes(pages::request_page(page, times, acknowledged, descriptor_1, descriptor_2));
    }, py::arg("page"), py::arg("times") = 1, py::arg("acknowledged") = false, py::arg("descriptor_1") = py::none(),
       py::arg("descriptor_2") = py::none());
    m.def("page_mode_settings", [](int sport_mode) { return to_bytes(pages::mode_settings(sport_mode)); },
          py::arg("sport_mode"));
    m.def("page_heart_rate_feature_command", [](std::optional<bool> gym_mode) {
        return to_bytes(pages::heart_rate_feature_command(gym_mode));
    }, py::arg("gym_mode"));
    m.def("page_power_manual_zero", [] { return to_bytes(pages::power_manual_zero()); });
    m.def("page_power_auto_zero", [](bool enabled) { return to_bytes(pages::power_auto_zero(enabled)); },
          py::arg("enabled"));
    m.def("page_power_crank_length", [](std::optional<double> millimetres) {
        return to_bytes(pages::power_crank_length(millimetres));
    }, py::arg("millimetres"));
    m.def("page_power_cycling_dynamics", [](bool eight_hz, bool power_phase, bool pedal_center_offset,
                                            bool rider_position, bool torque_barycenter) {
        return to_bytes(pages::power_cycling_dynamics(eight_hz, power_phase, pedal_center_offset, rider_position,
                                                      torque_barycenter));
    }, py::arg("eight_hz"), py::arg("power_phase"), py::arg("pedal_center_offset"), py::arg("rider_position"),
       py::arg("torque_barycenter"));

    // ---- Profiles ----------------------------------------------------------

    py::class_<Profile, std::shared_ptr<Profile>>(m, "Profile")
        .def("on_message", [](Profile& p, py::handle message) {
            const DecodedMessage decoded = message_from_dict(message);
            std::lock_guard<std::mutex> lock(p.mutex);
            p.on_message(decoded);
        })
        .def_property_readonly("device_type", &Profile::device_type)
        .def_property_readonly("pages_received", locked<Profile>([](const Profile& p) { return p.pages_received; }))
        .def_property_readonly("readings_received",
                               locked<Profile>([](const Profile& p) { return p.readings_received; }))
        .def_property_readonly("last_page", locked<Profile>([](const Profile& p) { return p.last_page; }))
        .def_property_readonly("page_counts", locked<Profile>([](const Profile& p) { return p.page_counts; }))
        .def_property_readonly("version", locked<Profile>([](const Profile& p) { return p.version; }))
        .def_property_readonly("pages", locked<Profile>([](const Profile& p) { return pages_dict(p.pages); }))
        .def_property_readonly("last_raw", locked<Profile>([](const Profile& p) { return raw_page(p.last_raw); }));

    py::class_<HeartRateProfile, Profile, std::shared_ptr<HeartRateProfile>>(m, "HeartRateProfile")
        .def(py::init<>())
        .def_property_readonly("latest",
                               locked<HeartRateProfile>([](const HeartRateProfile& p) { return heart_rate_reading(p.latest); }))
        .def_property_readonly("event_time_unwrapped", locked<HeartRateProfile>([](const HeartRateProfile& p) {
            return p.event_time_unwrapped;
        }));

    py::class_<PowerMeterProfile, Profile, std::shared_ptr<PowerMeterProfile>>(m, "PowerMeterProfile")
        .def(py::init<>())
        .def_property_readonly("latest",
                               locked<PowerMeterProfile>([](const PowerMeterProfile& p) { return power_reading(p.latest); }));

    py::class_<SpeedCadenceProfile, Profile, std::shared_ptr<SpeedCadenceProfile>>(m, "SpeedCadenceProfile")
        .def(py::init<int, double>(), py::arg("device_type"), py::arg("wheel_circumference_m") = 2.199)
        .def_property_readonly("latest", locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) {
            return speed_cadence_reading(p);
        }))
        .def_property(
            "wheel_circumference_m",
            locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) { return p.wheel_circumference_m; }),
            [](SpeedCadenceProfile& p, double value) {
                std::lock_guard<std::mutex> lock(p.mutex);
                p.wheel_circumference_m = value;
            })
        .def_property_readonly("last_combined_payload", locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) {
            return p.last_combined_payload ? py::object(to_bytes(*p.last_combined_payload)) : py::none();
        }))
        .def_property_readonly("event_time_unwrapped", locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) {
            return p.event_time_unwrapped;
        }))
        .def_property_readonly("cadence_event_time_unwrapped",
                               locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) {
                                   return p.cadence_event_time_unwrapped;
                               }))
        .def_property_readonly("speed_event_time_unwrapped",
                               locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) {
                                   return p.speed_event_time_unwrapped;
                               }))
        .def_property_readonly("held_cadence_rpm", locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) {
            return p.held_cadence_rpm();
        }))
        .def_property_readonly("held_speed_mps", locked<SpeedCadenceProfile>([](const SpeedCadenceProfile& p) {
            return p.held_speed_mps();
        }));

    py::class_<FitnessEquipmentProfile, Profile, std::shared_ptr<FitnessEquipmentProfile>>(m, "FitnessEquipmentProfile")
        .def(py::init<>())
        .def_property_readonly("latest", locked<FitnessEquipmentProfile>([](const FitnessEquipmentProfile& p) {
            return fe_reading(p.latest);
        }))
        .def_property_readonly("capabilities", locked<FitnessEquipmentProfile>([](const FitnessEquipmentProfile& p) {
            return fe_capabilities(p.capabilities);
        }))
        .def_property_readonly("capabilities_version",
                               locked<FitnessEquipmentProfile>([](const FitnessEquipmentProfile& p) {
                                   return p.capabilities_version;
                               }));

    m.def("make_profile", &make_profile, py::arg("device_type"));
    m.def("has_profile", &has_profile, py::arg("device_type"));

    // ---- Sensor ------------------------------------------------------------

    py::class_<Sensor, std::shared_ptr<Sensor>> sensor(m, "Sensor");
    sensor
        .def(py::init([](int device_number, int device_type, int transmission_type, int channel,
                         std::shared_ptr<Profile> profile, py::object clock) {
                 ClockFn now = py_clock(std::move(clock));
                 const double at = now();
                 return std::make_shared<Sensor>(device_number, device_type, transmission_type, channel,
                                                 std::move(profile), now, at);
             }),
             py::arg("device_number"), py::arg("device_type"), py::arg("transmission_type"), py::arg("channel"),
             py::arg("profile"), py::arg("clock"))
        .def_property_readonly("device_number", &Sensor::device_number)
        .def_property_readonly("device_type", &Sensor::device_type)
        .def_property_readonly("transmission_type", &Sensor::transmission_type)
        .def_property_readonly("profile", &Sensor::profile)
        .def("update", [](Sensor& s, py::handle message) { return s.update(message_from_dict(message)); })
        .def("mark_error", &Sensor::mark_error)
        .def("mark_state", &Sensor::mark_state)
        .def("mark_recovered", &Sensor::mark_recovered)
        .def("mark_stalled", &Sensor::mark_stalled)
        .def("clear_stalled", &Sensor::clear_stalled)
        .def("battery", [](const Sensor& s) { return battery_dict(s.battery()); })
        .def("batteries", [](const Sensor& s) {
            py::dict result;
            for (const auto& [id, battery] : s.batteries()) {
                result[py::int_(id)] = battery_dict(battery);
            }
            return result;
        });
    sensor_field(sensor, "channel", &Sensor::channel);
    sensor_field(sensor, "last_seen", &Sensor::last_seen);
    sensor_field(sensor, "packet_count", &Sensor::packet_count);
    sensor_field(sensor, "last_new_data_seen", &Sensor::last_new_data_seen);
    sensor_field(sensor, "state", &Sensor::state);
    sensor_field(sensor, "error_count", &Sensor::error_count);
    sensor_field(sensor, "recovery_attempts", &Sensor::recovery_attempts);
    sensor_field(sensor, "recovery_count", &Sensor::recovery_count);
    sensor_field(sensor, "last_state_change", &Sensor::last_state_change);
    sensor_field(sensor, "lost_time", &Sensor::lost_time);
    sensor_field(sensor, "stale_time", &Sensor::stale_time);
    sensor_field(sensor, "data_stalled", &Sensor::data_stalled);
    sensor_field(sensor, "stalled_since", &Sensor::stalled_since);
    sensor_field(sensor, "data_resumed", &Sensor::data_resumed);

    // ---- Manager -----------------------------------------------------------

    py::class_<Manager, std::shared_ptr<Manager>>(m, "Manager")
        .def(py::init([](py::object native, py::object sink, py::object clock, py::object sleep,
                         py::object sensor_clock, py::object on_log) {
                 return std::make_shared<Manager>(std::make_shared<PyTransport>(std::move(native), std::move(sink)),
                                                  py_clock(std::move(clock)), py_sleep(std::move(sleep)),
                                                  py_clock(std::move(sensor_clock)), py_log(std::move(on_log)));
             }),
             py::arg("native"), py::arg("sink"), py::arg("clock"), py::arg("sleep"), py::arg("sensor_clock"),
             py::arg("on_log") = py::none())
        .def("open", &Manager::open, py::arg("baudrate") = 57600, py::arg("device_number") = py::none(), release_gil())
        .def("close", &Manager::close, release_gil())
        .def("start", &Manager::start, release_gil())
        .def("stop", &Manager::stop, release_gil())
        .def("configure_scan", [](Manager& manager, py::handle key, int network_number, int channel, int rf) {
            const std::vector<std::uint8_t> network_key = bytes_of(key);
            py::gil_scoped_release release;
            manager.configure_scan(network_key, network_number, channel, rf);
        }, py::arg("network_key"), py::arg("network_number") = 0, py::arg("channel") = 0, py::arg("rf_frequency") = 57)
        .def("open_device_channels", [](Manager& manager, py::iterable identities) {
            const std::vector<ChannelIdentity> wanted = identities_of(identities);
            py::gil_scoped_release release;
            manager.open_device_channels(wanted);
        })
        .def("return_to_scan", &Manager::return_to_scan, release_gil())
        .def("wants_device_channels", &Manager::wants_device_channels)
        .def("retry_device_channels", &Manager::retry_device_channels, release_gil())
        .def("reopen_channel", &Manager::reopen_channel, release_gil())
        .def("reopen_scan_channel", &Manager::reopen_scan_channel, release_gil())
        .def("reapply_configuration", &Manager::reapply_configuration, release_gil())
        .def("channel_for", &Manager::channel_for)
        .def("channel_devices", &Manager::channel_devices)
        .def("events_suppressed", &Manager::events_suppressed, py::arg("channel"), py::arg("now") = py::none())
        .def("on_message", [](Manager& manager, py::handle message) {
            const Manager::MessageResult result = manager.on_message(message_from_dict(message));
            return py::make_tuple(result.sensor ? py::cast(result.sensor) : py::none(), result.changed,
                                  result.dropped);
        })
        .def("get_devices", &Manager::get_devices)
        .def_property_readonly("mode", [](Manager& manager) -> py::object {
            const char* name = mode_name(manager.mode());
            return name ? py::object(py::str(name)) : py::none();
        })
        .def_property_readonly("needs_reset", &Manager::needs_reset)
        .def_property_readonly("scan_config", [](Manager& manager) { return scan_config_dict(manager.scan_config()); })
        .def_property_readonly("device_number", &Manager::device_number)
        .def("set_drop_devices", &Manager::set_drop_devices)
        .def("set_log", [](Manager& manager, py::object on_log) { manager.set_log(py_log(std::move(on_log))); });

    // ---- Recovery ----------------------------------------------------------

    py::class_<Recovery, std::shared_ptr<Recovery>>(m, "Recovery")
        .def(py::init([](std::shared_ptr<Manager> manager, py::dict options, py::object clock, py::object on_event) {
                 Recovery* recovery = new Recovery(*manager, options_from(options, RecoveryOptions{}),
                                                   py_clock(std::move(clock)), py_log(std::move(on_event)));
                 // Keeps the manager for as long as the watchdog, and stops the
                 // watchdog thread with the GIL released (it may be waiting for it)
                 return std::shared_ptr<Recovery>(recovery, [manager](Recovery* r) {
                     if (PyGILState_Check()) {
                         py::gil_scoped_release release;
                         delete r;
                     } else {
                         delete r;
                     }
                 });
             }),
             py::arg("manager"), py::arg("options"), py::arg("clock"), py::arg("on_event") = py::none())
        .def("start", &Recovery::start)
        .def("stop", &Recovery::stop, release_gil())
        .def_property_readonly("running", &Recovery::running)
        .def("check", &Recovery::check, release_gil())
        .def("notify_channel_event", &Recovery::notify_channel_event, py::arg("event_name"),
             py::arg("channel") = py::none())
        .def("set_watched_devices", [](Recovery& recovery, py::object identities) {
            if (identities.is_none()) {
                recovery.set_watched_devices(std::nullopt);
                return;
            }
            std::set<std::pair<int, int>> watched;
            for (py::handle identity : identities) {
                py::tuple t = py::tuple(py::reinterpret_borrow<py::object>(identity));
                watched.insert({t[0].cast<int>(), t[1].cast<int>()});
            }
            recovery.set_watched_devices(std::move(watched));
        })
        .def_property_readonly("watched_devices", [](Recovery& recovery) -> py::object {
            auto watched = recovery.watched_devices();
            if (!watched) return py::none();
            py::set result;
            for (const auto& [type, number] : *watched) {
                result.add(py::make_tuple(type, number));
            }
            return std::move(result);
        })
        .def("enable_escalation", &Recovery::enable_escalation)
        .def("disable_escalation", &Recovery::disable_escalation)
        .def_property_readonly("escalation_enabled", &Recovery::escalation_enabled)
        .def("report", &Recovery::report)
        .def("channel_reopens", [](Recovery& recovery) {
            py::dict result;
            for (const auto& [channel, episode] : recovery.channel_reopens()) {
                py::dict entry;
                entry["reopened"] = episode.reopened;
                entry["stick_reset"] = episode.stick_reset;
                result[py::int_(channel)] = entry;
            }
            return result;
        })
        .def("options", [](Recovery& recovery) { return options_dict(recovery.options()); })
        .def("set_options", [](Recovery& recovery, py::dict values) {
            recovery.set_options(options_from(values, recovery.options()));
        })
        .def("set_log", [](Recovery& recovery, py::object on_event) { recovery.set_log(py_log(std::move(on_event))); })
        .def_static("actionable_event", &Recovery::actionable_event);
}

}  // namespace antnative_py

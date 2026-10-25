#pragma once

// Conversions shared by the ant_native module's bindings (ant_native_module.cpp
// for the stick, sensor_bindings.cpp for sensors, profiles and recovery).

#include <pybind11/pybind11.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "../core/ant_core.hpp"
#include "../core/ant_manager.hpp"
#include "../core/ant_pages.hpp"

namespace antnative_py {

namespace py = pybind11;

std::vector<std::uint8_t> to_vector(const py::bytes& value);
py::bytes to_bytes(const std::vector<std::uint8_t>& value);
// bytes, bytearray or a list of ints
std::vector<std::uint8_t> bytes_of(py::handle value);

// A decoded page as a dict: its fields, plus "page" and "page_name"
py::dict page_dict(const antnative::PageData& page);
py::object field_value(const antnative::FieldValue& value);

// The message dict the pump's callback gets, and back: a dict built that way
// (or by hand, as the tests' fakes do) as the DecodedMessage the core works
// from - a key that's absent, or None, is an empty field
py::dict to_dict(const antnative::DecodedMessage& decoded);
antnative::DecodedMessage message_from_dict(py::handle message);

// A Python callable the core can keep and call from any thread: the GIL is
// taken for each call and for releasing it
std::shared_ptr<py::object> hold(py::object object);
antnative::ClockFn py_clock(py::object fn);
antnative::SleepFn py_sleep(py::object fn);
antnative::LogFn py_log(py::object fn);

void bind_sensors(py::module_& module);

}  // namespace antnative_py

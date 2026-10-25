# AntNative v23

ANT+ sensors for Python, C and C++: the USB stick, every data page of six sensor types, per-sensor profiles,
dedicated channels for sending to a sensor, and a watchdog that recovers dropped sensors. It's all one native core;
each language gets a thin layer over it.

## Layout

```
src/core/      The core, nothing language-specific:
                 ant_core      the ANT SDK transport (stick, channels, message pump) and the per-message decode
                 ant_pages     every ANT+ data page of the six sensor types, decoded; the control pages, built
                 ant_profiles  battery status and the per-sensor profiles (rates, rollover, each page received)
                 ant_manager   Sensor and Manager: scan/dedicated channels, routing messages to sensors
                 ant_recovery  the watchdog: stale/lost sensors, frozen readings, channel reopens, stick resets
src/python/    The ant_native Python module (pybind11): the stick (ant_native_module), sensors/profiles/watchdog
               (sensor_bindings), conversions (py_common)
src/c_api/     antnative_c.cpp: the C API, built as antnative.dll
include/       antnative.h (C API) and antnative.hpp (header-only C++ wrapper over the C API)
python/ant/    The Python package: ANTManager, RecoveryManager, ANTDevice, the profiles and ant.pages - thin
               layers over the core with the original Python API
examples/      list_devices.c, scan.cpp (the stick) and sensors.cpp (the sensor manager)
tests/         Tests that need no stick (pytest); python/*_test.py are the hardware scripts
```

## Building

From a VS 2022 Developer PowerShell, with the ANT SDK at `..\..\antlib\ANT_LIB` (or pass `-ANTLibRoot`):

```powershell
.\build_windows.ps1
```

This builds ANT_LIB, then `build\Release\antnative.dll` (+ `antnative.lib`, its import library), then
`build\Release\ant_native.cpXYZ-win_amd64.pyd` for every Python 3.10+ installed, and copies the `.pyd` files and the
`ant` package into AntWorkout. `CMakeLists.txt` builds the same libraries for CMake-based projects.

Then run the tests (no stick needed): `cd tests; py -m pytest`

## Python

The `ant` package keeps its API: `ANTManager`, `RecoveryManager`, `ANTDevice` and the profiles work as before, with
the work done in the core. They drive whatever `native` object they're given (the real stick, or a test fake) and
take their time from each module's `monotonic()`/`sleep()`, so tests can still substitute both.

Every received message has a `page_data` dict (the payload's page, field by field); every profile keeps the last
page of each number in `profile.pages` (also `device.pages`); `ant.pages` decodes a payload and builds the control
pages. See `python/ant/pages.py` for what each sensor type's pages contain.

## C and C++ API

`antnative.dll` exposes the core through a C ABI, so it works from any compiler or language that can call a C DLL
(C, C++, C# P/Invoke, LuaJIT FFI, Rust, a Godot GDExtension...). C++ programs can include `antnative.hpp`, which
wraps it in RAII classes and exceptions; it's built on the C API rather than exporting C++ classes, so the program
can use any compiler or runtime library.

Two levels, see `include/antnative.h`:

- **Sensors** (`ant_manager`, `antnative::Manager`): what most programs want. Open a stick, scan, read each
  sensor's latest values (`ant_manager_sensors`: heart rate, power, cadence, speed, trainer status, battery, ...;
  cadence and speed are held through repeated events and drop to 0 when stopped), read any page a sensor has sent
  (`ant_manager_sensor_page`), move sensors to their own channels and send to them (`ant_manager_send`, e.g. a
  trainer's target power or grade), and run the watchdog. `examples/sensors.cpp` is a working example.
- **The stick** (`ant_device`, `antnative::Device`): raw channel configuration and messages, polled from a queue
  (`ant_poll`), each decoded into `ant_message` (its `fields` bitmask says which groups are set, `page_data` holds the
  page). `examples/scan.cpp`.

Nothing ever calls back into the program: messages queue up and are read whenever suits, e.g. once per frame.

## Sensors and pages

| Sensor | Device type | Pages decoded |
|---|---|---|
| Heart rate | 120 | default, operating time, manufacturer/product id, previous beat (RR interval), swim interval summary, capabilities, battery |
| Speed / cadence | 123 / 122 | default, operating time, manufacturer/product id, battery, motion (stopped) |
| Speed & cadence | 121 | its single page-less layout |
| Bicycle power | 11 | calibration response, power only (pedal balance, accumulated power), wheel torque, crank torque, torque effectiveness/pedal smoothness, crank torque frequency |
| Fitness equipment (FE-C) | 17 | calibration, general FE data (speed, distance, elapsed time, heart rate, state), settings, metabolic, treadmill, elliptical, rower, climber, Nordic skier, trainer data and torque, control pages, capabilities, user configuration |
| all but 121 | | common pages: request, command status, manufacturer and product information, battery |

Layouts come from the Fitness Equipment (Rev 5.0) and Bike Speed and Cadence (Rev 2.1) device profiles and the
Recording Bike Power Data application note; `src/core/ant_pages.cpp` notes the few taken from elsewhere (heart rate
pages 5 and 6, common pages 80-82). Bicycle power's cycling dynamics (0xE0-0xE2) and get/set parameters (0x02)
aren't decoded: they aren't defined in the documents on hand.

## Trainer control (FE-C)

Control pages, built in the core (`ant.profiles.fitness_equipment`, `ant.pages`, `ant_page_*` in C):

- Basic Resistance (page 48) - percent of maximum resistance; leaves ERG mode
- Target Power (page 49, ERG mode) - 0-4000 W
- Wind Resistance (page 50) and Track Resistance (page 51, grade -200 to 200%) - simulation mode
- User Configuration (page 55) - rider and bike weight, wheel size, gear ratio
- Request Data Page (common page 70) - e.g. ask for FE Capabilities (0x36) or Command Status (0x47)

They're sent acknowledged on the trainer's dedicated channel: `ant_manager_send` in C, or in Python
`ANTNative.send_acknowledged(channel, payload, timeout_ms)` with `ANTManager.channel_for(17, device_number)` after
`ANTManager.open_device_channels(...)`. AntWorkout's `ClassAntDevices._service_trainer_target()` is the working
Python example: it resends the target every couple of seconds and retries on failure.

## USB device enumeration and dual-stick test

```python
import ant_native

for d in ant_native.list_devices():
    print(d)

a = ant_native.ANTNative()
b = ant_native.ANTNative()
print("open 0:", a.open(device_number=0))
print("open 1:", b.open(device_number=1))
print("states:", a.is_open(), b.is_open())
b.close()
a.close()
```

`list_devices()` uses the ANT SDK's `USBDeviceHandle::GetAllDevices()`; its indexes are the `device_number`s
`ANTNative.open()` takes. The sticks must not be owned by another application such as SimulANT+ at the same time.

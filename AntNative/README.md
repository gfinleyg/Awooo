# AntNative v22

ANT USB stick access for Python, C and C++, sharing one native core.

## Layout

```
src/core/      ant_core.hpp/.cpp: the ANT SDK transport (stick, channels, message pump) and the
               per-message decode. Nothing language-specific; both libraries below compile it in.
src/python/    ant_native_module.cpp: the ant_native Python module (pybind11), a thin layer over the core
src/c_api/     antnative_c.cpp: the C API, built as antnative.dll
include/       antnative.h (C API) and antnative.hpp (header-only C++ wrapper over the C API)
python/ant/    The Python package above ant_native: manager, recovery, ANT+ profiles
examples/      list_devices.c and scan.cpp: the C and C++ APIs in use
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

## C and C++ API

`antnative.dll` exposes the core through a C ABI, so it works from any compiler or language that can call a C DLL
(C, C++, C# P/Invoke, LuaJIT FFI, Rust, a Godot GDExtension...). C++ programs can include `antnative.hpp`, which
wraps it in RAII classes and exceptions; it's built on the C API rather than exporting C++ classes, so the program
can use any compiler or runtime library.

Messages are polled, never called back: the stick's messages queue up (newest 4096 kept) and `ant_poll` /
`Device::poll()` hands them over, decoded into `ant_message` (its `fields` bitmask says which groups are set), once
per frame or whenever suits. See `include/antnative.h` for the full API and `examples/scan.cpp` for a working scan.

The Python package (`python/ant`: ANTManager, RecoveryManager, the ANT+ profiles) is still Python only.

## Profiles

- Heart Rate (device type 120)
- Bicycle Speed/Cadence (device types 121/122/123)
- Bicycle Power (device type 11): Standard Power-Only page (0x10) decoded into instantaneous power and cadence; every other page is kept raw
- Fitness Equipment / FE-C trainer (device type 17): Specific Trainer Data (0x19) decoded into instantaneous power, cadence and trainer status; FE Capabilities (0x36) decoded into supported control modes and maximum resistance; every other page is kept raw

The core decodes each page's fields (heart rate pages 0-4, the Standard Power-Only page, speed/cadence page 0 and
the combined sensor's layout) the same for every language. Anything stateful across messages (rollover-safe rates,
wheel size, dedup) and the FE-C pages stay in the Python profiles.

## Trainer control (FE-C)

`ant.profiles.fitness_equipment` builds the 8-byte control pages:

- `target_power_payload(watts)` - Target Power (page 49, ERG mode), clamped to 0-4000 W
- `basic_resistance_payload(percent)` - Basic Resistance (page 48), clamped to 0-100%; sending it takes the trainer out of ERG mode

The profile only builds the pages; the owner of the channel sends them with `ANTNative.send_acknowledged(channel, payload, timeout_ms)` on the trainer's dedicated channel (`ANTManager.channel_for(17, device_number)` after `ANTManager.open_device_channels(...)`). AntWorkout's `ClassAntDevices._service_trainer_target()` is the working example: it resends the target every couple of seconds and retries on failure.

Not implemented yet: Wind Resistance (page 50), Track Resistance (page 51, grade simulation) and User Configuration (page 55).

## Sources / verification

ANT's BSC receiver documentation identifies combined BSC device type 121 and describes page-0 cadence/speed event and revolution data. See the official ANT+ receiver example.

## USB device enumeration and dual-stick test

The native module now exposes:

```python
ant_native.list_devices()
```

It uses the ANT SDK's `USBDeviceHandle::GetAllDevices()` and returns copied metadata only. Device indexes are the same indexes accepted by `ANTNative.open(..., device_number=...)`.

Example:

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

For two physical ANT USB sticks, this verifies that each stick can be selected and opened by a separate native instance. The sticks must not simultaneously be owned by another application such as SimulANT+.

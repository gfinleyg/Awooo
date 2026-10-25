# AntNative v21

v21 continues the Python ANT+ profile layer above the existing native `ant_native.pyd` transport.

## Profiles

- Heart Rate (device type 120)
- Bicycle Speed/Cadence (device types 121/122/123)
- Bicycle Power (device type 11): Standard Power-Only page (0x10) decoded into instantaneous power and cadence; every other page is kept raw
- Fitness Equipment / FE-C trainer (device type 17): Specific Trainer Data (0x19) decoded into instantaneous power, cadence and trainer status; FE Capabilities (0x36) decoded into supported control modes and maximum resistance; every other page is kept raw

The native layer remains generic. Profile-specific interpretation stays in Python.

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

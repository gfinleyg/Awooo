# AntNative v21

v21 continues the Python ANT+ profile layer above the existing native `ant_native.pyd` transport.

## Added profiles

- Heart Rate (device type 120)
- Bicycle Speed/Cadence (device types 121/122/123)
- Bicycle Power (device type 11; raw page collector pending exact page-set selection)
- Fitness Equipment (device type 17; raw page collector)
- Controllable Trainer (receive-side page collector; TX remains exposed through the native channel transport)

The native layer remains generic. Profile-specific interpretation stays in Python.

## Sources / verification

ANT's BSC receiver documentation identifies combined BSC device type 121 and describes page-0 cadence/speed event and revolution data. See the official ANT+ receiver example.

Do not treat the raw Power/FE page collectors as complete profile implementations yet; they intentionally preserve the payload until the exact profile page set is selected and tested against real/simulated devices.

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

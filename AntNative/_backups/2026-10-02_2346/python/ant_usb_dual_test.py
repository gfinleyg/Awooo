import sys

sys.path.insert(0, r".\build\Release")

import ant_native


def main():
    devices = ant_native.list_devices()
    print("devices:", len(devices))
    for device in devices:
        print(device)

    if len(devices) < 2:
        print("Need at least two ANT USB devices for the dual-open test.")
        return 2

    first = ant_native.ANTNative()
    second = ant_native.ANTNative()

    try:
        print("open 0:", first.open(device_number=devices[0]["index"]))
        print("open 1:", second.open(device_number=devices[1]["index"]))
        print("is_open 0:", first.is_open())
        print("is_open 1:", second.is_open())
        return 0 if first.is_open() and second.is_open() else 1
    finally:
        second.close()
        first.close()


if __name__ == "__main__":
    raise SystemExit(main())

import sys
import time

sys.path.insert(0, r"C:\Users\gfinl\Documents\AntNative\build\Release")

import ant_native


print("Listing devices")
devices = ant_native.list_devices()

for d in devices:
    print(d)


for index in range(len(devices)):
    print("\nTrying device", index)

    ant = ant_native.ANTNative()

    ok = ant.open(57600, index)

    print("open:", ok)

    if ok:
        print("is_open:", ant.is_open())

        time.sleep(2)

        ant.close()

        print("after close:", ant.is_open())

        time.sleep(1)

        ok2 = ant.open(57600, index)

        print("reopen:", ok2)

        if ok2:
            print("reopen state:", ant.is_open())

        ant.close()
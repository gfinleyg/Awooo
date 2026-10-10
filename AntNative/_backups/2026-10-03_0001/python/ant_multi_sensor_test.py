import time
import os
from threading import Timer

from ant.manager import ANTManager
from ant.recovery import RecoveryManager


NETWORK_KEY = bytes([
    0xB9, 0xA5, 0x21, 0xFB,
    0xBD, 0x72, 0xC3, 0x45
])


seen = {
    120: False,   # HR
    121: False,   # Speed/Cadence
    11: False     # Power
}


TYPE_NAMES = {
    120: "HEART RATE",
    121: "SPEED/CAD",
    11:  "POWER"
}


ant = ANTManager()

recovery = RecoveryManager(
    ant,
    stale_timeout=3,
    lost_timeout=10,
    interval=1
)


start = time.time()


def clear_screen():

    os.system(
        "cls" if os.name == "nt" else "clear"
    )


def show_status():

    elapsed = time.time() - start

    clear_screen()

    print("ANT Recovery Monitor")
    print("====================")
    print()

    print(
        f"Runtime: {elapsed:6.1f}s"
    )

    print()

    print(
        "DEVICE   TYPE          FOUND"
    )

    print(
        "-----------------------------"
    )

    for dtype, found in seen.items():

        print(
            f"{dtype:<8}"
            f"{TYPE_NAMES[dtype]:<14}"
            f"{found}"
        )

    print()

    devices = ant.get_devices()

    print(
        "Active ANT devices:"
    )

    print(
        "-------------------"
    )

    for device in devices:

        age = (
            time.monotonic()
            -
            device.last_seen
        )

        print(
            f"id={device.device_number:<3}"
            f"type={device.device_type:<4}"
            f"age={age:5.2f}s"
        )

    print()

    print(
        "Recovery stats:"
    )

    print(
        recovery.report()
    )

    print()

    print(
        "Waiting for events..."
    )


def callback(message, device):

    if device is None:
        return


    dtype = device.device_type


    if dtype not in seen:
        return


    #
    # Recovery heartbeat
    #
    recovery.packet_received(
        device
    )


    #
    # First discovery
    #
    if not seen[dtype]:

        seen[dtype] = True

        recovery.device_found(
            device.device_number,
            dtype
        )

        print(
            f"FOUND "
            f"id={device.device_number} "
            f"type={dtype} "
            f"{TYPE_NAMES[dtype]}"
        )



def force_recovery_test():

    print()
    print("=" * 60)
    print("RECOVERY TEST START")
    print("=" * 60)
    print()
    print(
        "Stop one simulator sensor now."
    )
    print(
        "Waiting for LOST -> RECOVERED sequence..."
    )



print("ANT multi sensor validation test")
print("-------------------------------")

print("Opening ANT USB...")

ant.open()


print("Configuring scan...")

ant.configure_scan(
    NETWORK_KEY,
    channel=0
)


print("Starting scan...")

recovery.start()

ant.start(
    callback
)


#
# Start recovery test after 15 seconds
#
Timer(
    15,
    force_recovery_test
).start()



try:

    while True:

        recovery.check()

        show_status()

        if all(seen.values()):

            pass

        time.sleep(1)



except KeyboardInterrupt:

    pass



finally:

    recovery.stop()

    ant.stop()

    ant.close()



print()
print("Final result")
print("----------------")

for k, v in seen.items():

    print(
        k,
        "FOUND" if v else "MISSING"
    )
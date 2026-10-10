#!/usr/bin/env python3

"""
ANT Recovery Automated Test

Combines:
  - Simulated per-device drop/recovery testing
    (was: ant_recovery_test.py)
  - Sensor-type discovery tracking + FOUND/MISSING summary
    and manual (physical) drop recovery test
    (was: ant_multi_sensor_test.py)

Controls:

1  Toggle HR drop            (simulated, by device number)
2  Toggle Power drop         (simulated, by device number)
3  Toggle Speed/Cad drop     (simulated, by device number)

0  Clear drops

t  Run automatic simulated recovery test (all drop combinations)
   Passive watchdog only - escalation (channel retry / device
   reset) is paused for the duration, since this drop is only
   ever simulated in software and shouldn't touch the radio.

m  Run manual recovery test (physically stop a real sensor)

e  Run escalation test (holds a real drop long enough to force
   an actual channel retry AND device-level reset, then checks
   recovery.stats to confirm both tiers actually fired)

q  Quit


State path:

ACTIVE
  |
  v
STALE
  |
  v
LOST
  |
  v
RECOVERED
  |
  v
ACTIVE
"""


import time
import os
import msvcrt
from threading import Timer


from ant.manager import ANTManager
from ant.recovery import RecoveryManager


NETWORK_KEY = bytes([
    0xB9, 0xA5, 0x21, 0xFB,
    0xBD, 0x72, 0xC3, 0x45
])


STALE_TIME = 1.5
LOST_TIME = 3


EXPECTED = {}

TEST_RUNNING = False
MANUAL_TEST_RUNNING = False
ESCALATION_TEST_RUNNING = False


EVENT_LOG = []



ant = ANTManager()

DROP_DEVICES = ant.drop_devices


recovery = RecoveryManager(
    ant,
    stale_timeout=STALE_TIME,
    lost_timeout=LOST_TIME,
    on_event=lambda msg: log(msg)
)



#
# Simulated drop test targets, keyed by device_number.
# These are the synthetic device IDs the drop-test rig uses.
#
DEVICE_NAMES = {

    1: "HEART RATE",
    2: "POWER",
    3: "SPD/CAD"

}



#
# Real sensor discovery tracking, keyed by ANT device_type.
# These are the actual ANT+ profile type numbers, distinct
# from the synthetic device numbers above.
#
SENSOR_TYPE_NAMES = {

    120: "HEART RATE",
    121: "SPEED/CAD",
    11:  "POWER"

}

SEEN_TYPES = {
    device_type: False
    for device_type in SENSOR_TYPE_NAMES
}



def clear_screen():

    os.system(
        "cls"
    )



def log(message):

    timestamp = time.monotonic()

    EVENT_LOG.append(
        (
            timestamp,
            message
        )
    )

    if len(EVENT_LOG) > 20:
        EVENT_LOG.pop(0)



def elapsed():

    return time.monotonic() - START_TIME


def event(msg):

    if hasattr(recovery, "log"):
        recovery.log(msg)
    else:
        log(msg)


def notify_recovery(device):

    #
    # RecoveryManager has no packet_received()/device_found()
    # hooks today (device.update() already refreshes last_seen
    # and packet_count, which is all recovery.check() reads).
    # Guarded here so the script keeps working if those hooks
    # get added later, without crashing in the meantime.
    #
    if hasattr(recovery, "packet_received"):
        recovery.packet_received(device)



def callback(message, device):

    if device is None:
        return


    device_id = device.device_number
    device_type = device.device_type


    EXPECTED.setdefault(
        device_id,
        "NORMAL"
    )


    #
    # simulate packet loss
    #
    if device_id in DROP_DEVICES:

        EXPECTED[device_id] = "DROPPED"

        return


    EXPECTED[device_id] = "NORMAL"


    notify_recovery(device)


    #
    # First-time discovery of a real sensor type
    #
    if device_type in SEEN_TYPES and not SEEN_TYPES[device_type]:

        SEEN_TYPES[device_type] = True

        if hasattr(recovery, "device_found"):
            recovery.device_found(device_id, device_type)

        log(
            f"FOUND id={device_id} type={device_type} "
            f"{SENSOR_TYPE_NAMES[device_type]}"
        )



def format_latest(dev):

    #
    # Shows what ant_native.cpp's profile_decode actually produced for
    # this device, so the C++ decode path can be verified against real
    # hardware - none of the existing panels above ever displayed this.
    #
    profile = getattr(dev, "profile", None)

    if profile is None:
        return "(no profile)"

    latest = getattr(profile, "latest", None)

    if latest is None:
        return "no data yet"

    if dev.device_type == 120:
        rr = getattr(latest, "rr_interval", None)
        rr_str = f" rr={rr:.3f}s" if rr is not None else ""
        return f"{latest.computed_heart_rate} bpm{rr_str}"

    if dev.device_type == 11:
        return f"{latest.instantaneous_power_watts} W"

    if dev.device_type == 121:
        cadence = getattr(latest, "cadence_rpm", None)
        speed = getattr(latest, "speed_kph", None)
        cadence_str = f"{cadence:.0f} rpm" if cadence is not None else "cadence: --"
        speed_str = f"{speed:.1f} kph" if speed is not None else "speed: --"
        return f"{cadence_str}  {speed_str}"

    if dev.device_type in (122, 123):
        cadence = getattr(latest, "cadence_rpm", None)
        speed = getattr(latest, "speed_kph", None)
        if cadence is not None:
            return f"{cadence:.0f} rpm"
        if speed is not None:
            return f"{speed:.1f} kph"
        return "no rate yet"

    return repr(latest)


def draw():

    clear_screen()

    print(
        "ANT RECOVERY TEST"
    )

    print(
        "================="
    )


    print()

    print(
        "Dropped:",
        sorted(DROP_DEVICES)
    )


    print()

    print(
        "Devices"
    )

    print(
        "-" * 65
    )


    print(
        f"{'ID':<4}"
        f"{'TYPE':<6}"
        f"{'STATE':<10}"
        f"{'AGE':<8}"
        f"{'PACKETS':<9}"
        f"EXPECTED"
    )


    print(
        "-" * 65
    )


    for dev in ant.get_devices():

        age = (
            time.monotonic()
            -
            dev.last_seen
        )


        expected = (
            "DROPPED"
            if dev.device_number in DROP_DEVICES
            else "NORMAL"
        )


        print(
            f"{dev.device_number:<4}"
            f"{dev.device_type:<6}"
            f"{dev.state:<10}"
            f"{age:5.2f}s "
            f"{dev.packet_count:<9}"
            f"{expected}"
        )


    print()

    print(
        "Decoded Values"
    )

    print(
        "-" * 65
    )

    print(
        f"{'ID':<4}"
        f"{'TYPE':<6}"
        f"VALUE"
    )

    print(
        "-" * 65
    )

    for dev in ant.get_devices():

        print(
            f"{dev.device_number:<4}"
            f"{dev.device_type:<6}"
            f"{format_latest(dev)}"
        )


    print()

    print(
        "Sensor Discovery"
    )

    print(
        "-" * 65
    )

    print(
        f"{'TYPE':<8}"
        f"{'NAME':<14}"
        f"FOUND"
    )

    print(
        "-" * 30
    )

    for device_type, found in SEEN_TYPES.items():

        print(
            f"{device_type:<8}"
            f"{SENSOR_TYPE_NAMES[device_type]:<14}"
            f"{found}"
        )


    print()

    print(
        "Stats"
    )

    print(
        "-----"
    )

    print(
        recovery.report()
    )


    print()

    print(
        "Event Log"
    )

    print(
        "---------"
    )


    for t, msg in EVENT_LOG[-12:]:

        print(
            f"{t-START_TIME:8.2f}s {msg}"
        )


    print()

    print(
        "Keys: 1 2 3 toggle drops | 0 clear | t auto test | "
        "m manual test | e escalation test | q quit"
    )



def toggle(device):

    if device in DROP_DEVICES:

        DROP_DEVICES.remove(device)

        event(
            f"DROP DISABLED id={device}"
        )

    else:

        DROP_DEVICES.add(device)

        event(
            f"DROP ENABLED id={device}"
        )

    log(f"ACTIVE DROP SET: {DROP_DEVICES}")



def keyboard():

    if not msvcrt.kbhit():

        return


    key = msvcrt.getch().decode(
        errors="ignore"
    )


    if key == "1":

        toggle(1)


    elif key == "2":

        toggle(2)


    elif key == "3":

        toggle(3)


    elif key == "0":

        DROP_DEVICES.clear()

        log(
            "ALL DROPS CLEARED"
        )


    elif key.lower() == "t":

        automatic_test()


    elif key.lower() == "m":

        manual_recovery_test()


    elif key.lower() == "e":

        escalation_test()


    elif key.lower() == "q":

        raise KeyboardInterrupt



def wait_for_state(
    ids,
    target,
    timeout=30
):

    start = time.monotonic()


    while (
        time.monotonic()
        -
        start
    ) < timeout:


        recovery.check()


        states = {

            d.device_number:
            d.state

            for d in ant.get_devices()

        }


        if all(
            states.get(i) == target
            for i in ids
        ):

            return True


        time.sleep(.25)



    return False



def run_test(ids):

    log(
        f"TEST DROP {ids}"
    )


    DROP_DEVICES.update(
        ids
    )


    wait_for_state(
        ids,
        "LOST"
    )


    log(
        f"TEST RESTORE {ids}"
    )


    for i in ids:

        DROP_DEVICES.discard(
            i
        )


    recovered = wait_for_state(
        ids,
        "ACTIVE"
    )


    log(
        f"TEST COMPLETE {ids} recovered={recovered}"
    )



def automatic_test():

    global TEST_RUNNING


    if TEST_RUNNING or MANUAL_TEST_RUNNING or ESCALATION_TEST_RUNNING:

        return


    TEST_RUNNING = True


    #
    # This test only exercises the passive watchdog (LOST via
    # simulated packet loss, ACTIVE again once packets resume).
    # Escalation can't tell a simulated drop from a real one, so
    # it's paused here to keep this a software-only test instead
    # of it quietly reopening the real scan channel underneath it.
    #
    recovery.disable_escalation()

    scenarios = [

        [1],
        [2],
        [3],

        [1,2],
        [1,3],
        [2,3],

        [1,2,3]

    ]


    for scenario in scenarios:

        run_test(
            scenario
        )

        time.sleep(2)


    recovery.enable_escalation()

    TEST_RUNNING = False


    log(
        "AUTOMATIC TEST FINISHED"
    )



def escalation_test():

    #
    # Deliberately exercises the real fallback path: holds a drop
    # on one real device well past channel_retry_limit attempts
    # and past device_reset_timeout, then confirms via
    # recovery.stats that a channel retry AND a device reset
    # actually fired (not just that the device eventually came
    # back, which the passive watchdog would also show).
    #
    global ESCALATION_TEST_RUNNING


    if ESCALATION_TEST_RUNNING or TEST_RUNNING or MANUAL_TEST_RUNNING:

        return


    ESCALATION_TEST_RUNNING = True


    devices = ant.get_devices()

    if not devices:

        log(
            "ESCALATION TEST ABORTED - no active devices to test"
        )

        ESCALATION_TEST_RUNNING = False

        return


    target = devices[0].device_number

    before = dict(recovery.report())

    #
    # Long enough to guarantee both tiers get a chance to fire:
    # channel_retry_limit attempts, channel_retry_cooldown apart,
    # then still past device_reset_timeout on top of that.
    #
    hold_time = (
        recovery.channel_retry_limit
        * recovery.channel_retry_cooldown
        + recovery.device_reset_timeout
        + 5
    )

    log(
        f"ESCALATION TEST START id={target} "
        f"holding drop for {hold_time:.0f}s"
    )

    DROP_DEVICES.add(target)

    deadline = time.monotonic() + hold_time

    while time.monotonic() < deadline:

        recovery.check()

        time.sleep(.25)


    DROP_DEVICES.discard(target)

    log(
        "ESCALATION TEST - drop cleared, waiting for recovery"
    )

    recovered = wait_for_state(
        [target],
        "ACTIVE",
        timeout=30
    )

    after = dict(recovery.report())

    channel_retries = (
        after["channel_retries"]
        - before["channel_retries"]
    )

    device_resets = (
        after["device_resets"]
        - before["device_resets"]
    )

    passed = (
        channel_retries > 0
        and device_resets > 0
        and recovered
    )

    log(
        f"ESCALATION TEST {'PASSED' if passed else 'FAILED'} "
        f"channel_retries={channel_retries} "
        f"device_resets={device_resets} "
        f"recovered={recovered}"
    )

    ESCALATION_TEST_RUNNING = False



def manual_recovery_test():

    #
    # Unlike automatic_test(), this exercises the real physical
    # link instead of the synthetic DROP_DEVICES filter: the
    # tester actually powers off / walks away from a sensor and
    # this watches for the real LOST -> RECOVERED transition on
    # whichever currently-active real device they choose.
    #
    global MANUAL_TEST_RUNNING


    if MANUAL_TEST_RUNNING or TEST_RUNNING or ESCALATION_TEST_RUNNING:

        return


    MANUAL_TEST_RUNNING = True


    ids = [
        dev.device_number
        for dev in ant.get_devices()
        if dev.device_number not in DROP_DEVICES
    ]

    log(
        "MANUAL TEST START - stop a real sensor now "
        f"(watching ids={ids})"
    )

    recovered = wait_for_state(
        ids,
        "LOST",
        timeout=60
    )

    if not recovered:

        log(
            "MANUAL TEST TIMEOUT waiting for LOST"
        )

        MANUAL_TEST_RUNNING = False

        return

    log(
        "MANUAL TEST - device(s) LOST, restart the sensor now"
    )

    recovered = wait_for_state(
        ids,
        "ACTIVE",
        timeout=60
    )

    log(
        f"MANUAL TEST COMPLETE recovered={recovered}"
    )

    MANUAL_TEST_RUNNING = False



def print_discovery_summary():

    print()
    print("Sensor discovery summary")
    print("-------------------------")

    for device_type, found in SEEN_TYPES.items():

        print(
            device_type,
            SENSOR_TYPE_NAMES[device_type],
            "FOUND" if found else "MISSING"
        )



START_TIME = time.monotonic()



print(
    "Opening ANT..."
)


ant.open()


ant.configure_scan(
    NETWORK_KEY,
    channel=0
)


recovery.start()


ant.start(
    callback
)


#
# Nudge to start the manual recovery test if the tester
# hasn't triggered it (key 'm') after 15 seconds.
#
Timer(
    15,
    lambda: log(
        "Tip: press 'm' to run the manual recovery test"
    ) if not MANUAL_TEST_RUNNING and not TEST_RUNNING else None
).start()



try:

    while True:

        recovery.check()

        keyboard()

        draw()

        time.sleep(.25)



except KeyboardInterrupt:

    pass



finally:

    recovery.stop()

    ant.stop()

    ant.close()

    print_discovery_summary()
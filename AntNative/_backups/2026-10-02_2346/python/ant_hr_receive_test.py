import sys
import time
import ant_native

KEY = bytes([
    0xB9, 0xA5, 0x21, 0xFB,
    0xBD, 0x72, 0xC3, 0x45
])


def check(name, result):
    print(f"{name}: {result}")
    if result is False:
        raise RuntimeError(f"{name} failed")


def main():
    device_index = int(sys.argv[1]) if len(sys.argv) > 1 else 0

    print("ANT HR receive test")
    print("-------------------")
    print(f"device index: {device_index}")

    ant = ant_native.ANTNative()

    check("open", ant.open(device_index))
    check("is_open", ant.is_open())

    check("reset", ant.reset())

    # ANT+ public network
    check("network", ant.set_network_key(0, KEY))

    # Channel 0, receive mode, public network
    check(
        "assign",
        ant.assign_channel(
            0,
            0x00,   # receive
            0       # public network
        )
    )

    # HR sensor search
    check(
        "channel id",
        ant.set_channel_id(
            0,
            1,      # any device number
            120,    # heart rate device type
            1       # any transmission type
        )
    )

    # ANT+ HR period
    check(
        "period",
        ant.set_channel_period(
            0,
            8070
        )
    )

    # ANT+ RF frequency
    check(
        "rf",
        ant.set_channel_rf_frequency(
            0,
            57
        )
    )

    check(
        "extended",
        ant.set_rx_extended_messages_enabled(True)
    )

    count = 0

    def callback(msg):
        nonlocal count
        count += 1
        print(msg)

    ant.start_message_pump(callback)

    check(
        "open channel",
        ant.open_channel(0)
    )

    print()
    print("Listening for 30 seconds...")
    print()

    time.sleep(30)

    print()
    print(f"messages received: {count}")

    ant.stop_message_pump()
    ant.close_channel(0)
    ant.close()


if __name__ == "__main__":
    main()
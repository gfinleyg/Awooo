import sys
import time
from collections import Counter

import ant_native


DEVICE_INDEX = int(sys.argv[1]) if len(sys.argv) > 1 else 0

NETWORK_KEY = bytes([
    0xB9, 0xA5, 0x21, 0xFB,
    0xBD, 0x72, 0xC3, 0x45
])


messages = []


def check(name, value):
    print(f"{name}: {value}")
    if value is False:
        raise RuntimeError(f"{name} failed")


def on_message(msg):
    messages.append(msg)

    try:
        print(msg)
    except Exception:
        print("message:", msg)


def main():

    print("ANT selected device test")
    print("------------------------")
    print("device index:", DEVICE_INDEX)

    ant = ant_native.ANTNative()

    try:
        check("open", ant.open(DEVICE_INDEX))
        print("is_open:", ant.is_open())

        check("reset", ant.reset())

        check(
            "network",
            ant.set_network_key(0, NETWORK_KEY)
        )

        check(
            "assign",
            ant.assign_channel(
                0,
                0x00,
                0
            )
        )

        check(
            "channel id",
            ant.set_channel_id(
                0,
                6582,
                120,
                81
            )
        )

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

        check(
            "open channel",
            ant.open_channel(0)
        )

        print()
        print("Starting message pump...")

        ant.start_message_pump(on_message)

        print("Listening for 15 seconds...")
        time.sleep(15)

        ant.stop_message_pump()

        print()
        print("messages:", len(messages))

        if messages:
            print(
                "message IDs:",
                Counter(
                    m["message_id"]
                    for m in messages
                )
            )

            print(
                "message names:",
                Counter(
                    m.get("message_name", "unknown")
                    for m in messages
                )
            )

        else:
            print("No messages received")

    finally:
        try:
            ant.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
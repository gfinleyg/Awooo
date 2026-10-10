import time
from collections import Counter

import ant_native


NETWORK_KEY = bytes([
    0xB9, 0xA5, 0x21, 0xFB,
    0xBD, 0x72, 0xC3, 0x45
])


def run_device(index, seconds=5):

    print()
    print("==============================")
    print("Testing device index:", index)
    print("==============================")

    messages = []

    def callback(msg):
        messages.append(msg)

    ant = ant_native.ANTNative()

    try:
        print("open:", ant.open(index))
        print("is_open:", ant.is_open())

        print("reset:", ant.reset())

        print(
            "network:",
            ant.set_network_key(0, NETWORK_KEY)
        )

        print(
            "assign:",
            ant.assign_channel(
                0,
                0,
                0
            )
        )

        print(
            "channel id:",
            ant.set_channel_id(
                0,
                6582,
                120,
                81
            )
        )

        print(
            "rf:",
            ant.set_channel_rf_frequency(
                0,
                57
            )
        )

        print(
            "extended:",
            ant.set_rx_extended_messages_enabled(True)
        )

        print(
            "open channel:",
            ant.open_channel(0)
        )

        ant.start_message_pump(callback)

        print(f"listening {seconds}s...")
        time.sleep(seconds)

        ant.stop_message_pump()

        counts = Counter(
            m["message_id"]
            for m in messages
        )

        print("messages:", len(messages))
        print("IDs:", counts)

        broadcast = [
            m for m in messages
            if m["message_id"] == 0x4E
        ]

        print("broadcasts:", len(broadcast))

    finally:
        ant.close()

        print(
            "closed, is_open:",
            ant.is_open()
        )


def main():

    print("ANT device switching test")
    print("-------------------------")

    for cycle in range(3):

        print()
        print("CYCLE", cycle + 1)

        run_device(0)
        run_device(1)


if __name__ == "__main__":
    main()
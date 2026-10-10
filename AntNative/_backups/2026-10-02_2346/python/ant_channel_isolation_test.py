import sys
import time
from collections import Counter
import ant_native


def run_device(index):
    print("\n" + "=" * 40)
    print(f"DEVICE {index}")
    print("=" * 40)

    ant = ant_native.ANTNative()

    if not ant.open(index):
        print("open failed")
        return

    print("open:", ant.is_open())

    ant.reset()
    ant.set_network_key(0, bytes.fromhex("B9 A5 21 FB BD 72 C3 45"))
    ant.assign_channel(0,0,0)
    ant.set_channel_id(0, 6582, 120, 81)
    ant.set_channel_period(0, 8070)
    ant.set_channel_rf_frequency(0, 57)
    ant.set_rx_extended_messages_enabled(True)

    print("channel open:", ant.open_channel(0))

    messages = []

    def callback(msg):
        messages.append(msg)

        if msg["message_id"] == 78:
            print(
                "RX:",
                "dev=", msg.get("device_number"),
                "type=", msg.get("device_type"),
                "tx=", msg.get("transmission_type")
            )

    ant.start_message_pump(callback)

    print("listening 10 seconds...")
    time.sleep(10)

    ant.stop_message_pump()
    ant.close_channel(0)

    print("messages:", len(messages))
    print("IDs:", Counter(m["message_id"] for m in messages))

    ant.close()

    print("closed:", not ant.is_open())


def main():
    if len(sys.argv) < 2:
        print("usage: python ant_channel_isolation_test.py <device index>")
        return

    index = int(sys.argv[1])
    run_device(index)


if __name__ == "__main__":
    main()
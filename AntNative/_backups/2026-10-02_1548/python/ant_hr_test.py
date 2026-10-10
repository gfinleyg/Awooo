import time
from ant.manager import ANTManager


def callback(message, device):
    if device is None:
        return

    print(
        "DEVICE:",
        device.device_number,
        device.device_type,
        device.transmission_type
    )

    if device.device_type == 120:
        hr = device.latest

        if hr:
            print(
                f"HR={hr.computed_heart_rate} bpm "
                f"beat={hr.heart_beat_count} "
                f"event={hr.heart_beat_event_time:.3f}s "
                f"page={hr.page}"
            )


ant = ANTManager()

print("ANT+ HR decoder test")
print("--------------------")

ant.open()

# ANT+ public network key
ant.configure_scan(
    bytes([0xB9,0xA5,0x21,0xFB,0xBD,0x72,0xC3,0x45])
)

ant.start(callback)

print("Listening...")

try:
    while True:
        time.sleep(1)

except KeyboardInterrupt:
    print("Stopping...")
    ant.close()
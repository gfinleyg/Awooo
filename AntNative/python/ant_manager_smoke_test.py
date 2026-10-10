import sys
import time

sys.path.insert(0, r".\build\Release")
sys.path.insert(0, r".\python")

from ant import ANTManager

KEY = bytes.fromhex("B9 A5 21 FB BD 72 C3 45")


def on_message(message, device):
    if device and device.profile:
        profile = device.profile
        page = profile.last_page
        latest = profile.latest
        if latest is not None:
            print(
                device.device_number,
                device.device_type,
                device.transmission_type,
                f"page={page}",
                f"hr={latest.computed_heart_rate}",
                f"beat={latest.heart_beat_count}",
                f"time={latest.heart_beat_event_time:.3f}",
                f"rr={latest.rr_interval}",
            )
        else:
            print(
                device.device_number,
                device.device_type,
                device.transmission_type,
                f"page={page}",
            )


m = ANTManager()
try:
    print("open:", m.open())
    m.configure_scan(KEY)
    m.start(on_message)
    print("Listening for 15 seconds...")
    time.sleep(15)
finally:
    m.close()

import time
from ant.manager_v21 import ANTManager

def callback(msg, device):
    if msg.get("has_device_id"):
        print(
            "DEVICE",
            "id=", msg.get("device_number"),
            "type=", msg.get("device_type"),
            "tx=", msg.get("transmission_type"),
            "page=", msg.get("payload", b"\x00")[0]
        )

ant = ANTManager()

print("Opening ANT USB...")
ant.open()

print("Configuring scan...")
ant.configure_scan(
    bytes([0xB9,0xA5,0x21,0xFB,0xBD,0x72,0xC3,0x45])
)

print("Starting scan...")
ant.start(callback)

try:
    while True:
        time.sleep(1)

except KeyboardInterrupt:
    ant.close()
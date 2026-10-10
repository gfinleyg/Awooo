from ant.manager import ANTManager
import time


def callback(message, device):
    print(message)

    if device:
        print(
            "DEVICE:",
            device.device_number,
            device.device_type,
            device.transmission_type
        )


print("ANT scan test")
print("----------------")

ant = ANTManager()

ant.open()

# ANT+ public network
ant.configure_scan(
    network_key=bytes([
        0xB9, 0xA5, 0x21, 0xFB,
        0xBD, 0x72, 0xC3, 0x45
    ])
)

print("Listening for SimulANT broadcasts...")

ant.start(callback)

try:
    while True:
        time.sleep(1)

except KeyboardInterrupt:
    ant.close()
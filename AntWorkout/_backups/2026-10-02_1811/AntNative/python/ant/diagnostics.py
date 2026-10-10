"""
ANT Device Diagnostics

Provides a common diagnostic view of any ANT device.

Used by:
    - recovery tests
    - console monitor
    - future UI/dashboard

"""

from time import monotonic


class DeviceDiagnostics:


    def __init__(self, device):

        self.device = device

        self.created = monotonic()

        self.packet_count = 0

        self.first_seen = monotonic()

        self.last_seen = monotonic()

        self.last_page = None

        self.last_payload = None



    def packet_received(self):

        self.packet_count += 1

        self.last_seen = monotonic()



    def update(self, device):

        self.device = device

        self.packet_received()


        if hasattr(device, "last_page"):

            self.last_page = (
                device.last_page
            )


        if hasattr(device, "latest"):

            latest = device.latest

            if latest:

                self.last_payload = (
                    repr(latest)
                )



    def age(self):

        return (
            monotonic()
            -
            self.last_seen
        )



    def packet_rate(self):

        elapsed = (
            monotonic()
            -
            self.first_seen
        )

        if elapsed <= 0:

            return 0


        return (
            self.packet_count
            /
            elapsed
        )



    def report(self):

        return {

            "device_number":
                self.device.device_number,

            "device_type":
                self.device.device_type,

            "transmission_type":
                self.device.transmission_type,


            "packets":
                self.packet_count,

            "age":
                round(
                    self.age(),
                    3
                ),

            "packet_rate":
                round(
                    self.packet_rate(),
                    2
                ),


            "last_page":
                self.last_page,


            "last_payload":
                self.last_payload

        }
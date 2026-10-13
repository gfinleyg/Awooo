from dataclasses import dataclass, field
from time import monotonic

from .battery import decode_battery


@dataclass
class ANTDevice:

    device_number: int
    device_type: int
    transmission_type: int
    channel: int

    profile: object | None = None

    #
    # Communication tracking
    #
    last_seen: float = field(default_factory=monotonic)
    last_message: dict | None = None
    packet_count: int = 0

    #
    # Set whenever a message actually changes the profile's decoded
    # payload (see update() below) - distinct from last_seen, which
    # updates on every packet even if it's byte-for-byte identical to
    # the last one. The gap between the two is what lets a "still
    # transmitting but the reading is frozen" fault be told apart from
    # a device that's gone silent (see RecoveryManager._check_data_freshness).
    #
    last_new_data_seen: float = field(default_factory=monotonic)


    #
    # Recovery state
    #
    state: str = "ACTIVE"

    error_count: int = 0

    recovery_attempts: int = 0
    recovery_count: int = 0

    last_state_change: float = field(
        default_factory=monotonic
    )

    lost_time: float = 0.0
    stale_time: float = 0.0

    #
    # Data-freshness state. Deliberately kept separate from `state`
    # above: `state` is recomputed from raw packet silence on every
    # RecoveryManager.check() tick, which would otherwise stomp this
    # back to false/true out of step with the actual data-plausibility
    # check. A device can be ACTIVE (packets arriving fine) and
    # data_stalled (the decoded value stopped changing) at the same time.
    #
    data_stalled: bool = False
    stalled_since: float = 0.0

    #
    # Set when new data clears data_stalled (see update()), until
    # RecoveryManager has logged that the reading is moving again - the
    # flag itself is cleared right away, so whatever reads it (the gauges'
    # fallbacks) sees the sensor as fine from the very next reading.
    #
    data_resumed: bool = False

    #
    # Latest battery status per battery id (a dual-sided power meter
    # reports each side separately). See battery.py; the `battery`
    # property below gives the weakest one.
    #
    batteries: dict = field(default_factory=dict)



    @property
    def identity(self):

        return (
            self.device_number,
            self.device_type,
            self.transmission_type,
        )



    def update(self, message):

        now = monotonic()

        #
        # Every ANT packet refreshes the watchdog
        #
        self.last_seen = now
        self.last_message = message
        self.packet_count += 1

        battery = decode_battery(
            self.device_type,
            message.get("payload")
        )
        if battery is not None:
            self.batteries[battery.battery_id] = battery


        #
        # Parser update
        #
        if self.profile:

            before = getattr(
                self.profile,
                "pages_received",
                None
            )

            self.profile.on_message(
                message
            )

            after = getattr(
                self.profile,
                "pages_received",
                None
            )

            changed = (
                before is None
                or after != before
            )

            if changed:
                # Genuinely new decoded data - this is also the signal
                # that clears a prior "stalled" flag (see
                # RecoveryManager._check_data_freshness): the moment the
                # reading actually moves again, it's no longer frozen.
                self.last_new_data_seen = now
                if self.data_stalled:
                    self.data_resumed = True
                self.clear_stalled()

            return changed


        #
        # No profile decode available - nothing to compare payloads
        # against, so every packet counts as new data.
        #
        self.last_new_data_seen = now

        return True



    def mark_error(self):

        self.error_count += 1



    def mark_state(self, state):

        if self.state != state:

            self.state = state
            self.last_state_change = monotonic()



    def mark_recovered(self):

        self.state = "ACTIVE"

        self.recovery_count += 1

        self.last_state_change = monotonic()

        self.lost_time = 0.0
        self.stale_time = 0.0



    def mark_stalled(self):

        if not self.data_stalled:

            self.data_stalled = True

            self.stalled_since = monotonic()



    def clear_stalled(self):

        if self.data_stalled:

            self.data_stalled = False

            self.stalled_since = 0.0



    @property
    def age(self):

        return (
            monotonic()
            - self.last_seen
        )



    @property
    def data_age(self):

        return (
            monotonic()
            - self.last_new_data_seen
        )



    @property
    def latest(self):

        if self.profile:

            return getattr(
                self.profile,
                "latest",
                None
            )

        return None



    @property
    def battery(self):

        if not self.batteries:
            return None

        return max(
            self.batteries.values(),
            key=lambda b: b.severity
        )



    @property
    def last_page(self):

        if self.profile:

            return getattr(
                self.profile,
                "last_page",
                None
            )

        return None



    @property
    def status(self):

        return {

            "id": self.device_number,
            "type": self.device_type,

            "state": self.state,

            "age": round(
                self.age,
                3
            ),

            "packets": self.packet_count,

            "recoveries": self.recovery_count,

            "errors": self.error_count,

            "data_stalled": self.data_stalled,

            "data_age": round(
                self.data_age,
                3
            )

        }
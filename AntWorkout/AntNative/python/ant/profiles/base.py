"""The profiles' shared plumbing. Each profile is a thin layer over its native counterpart (ant_native's
HeartRateProfile, ...; src/core/ant_profiles.cpp), which does all the decoding: this keeps the Python API, handing
out the same reading object until a new one arrives."""

from abc import ABC

from ..transport.native import ant_native as _core


class ANTProfile(ABC):
    device_type = None

    @classmethod
    def can_handle(cls, device_type):
        return cls.device_type == device_type

    @classmethod
    def _wrap(cls, core):
        """A profile over one the native layer created (for a sensor ANTManager heard)"""
        profile = cls.__new__(cls)
        profile._attach(core)
        return profile

    def _attach(self, core):
        self._core = core
        self._latest = None
        self._latest_version = None

    def on_message(self, message):
        self._core.on_message(message)

    def _reading(self, fields):
        """The profile's reading dataclass from the native reading's fields"""
        raise NotImplementedError

    @property
    def latest(self):
        version = self._core.version
        if version != self._latest_version:
            fields = self._core.latest
            self._latest = None if fields is None else self._reading(fields)
            self._latest_version = version
        return self._latest

    @property
    def pages_received(self):
        return self._core.pages_received

    @property
    def readings_received(self):
        """New readings only - pages_received also counts pages with nothing new in them"""
        return self._core.readings_received

    @property
    def last_page(self):
        return self._core.last_page

    @property
    def page_counts(self):
        return self._core.page_counts

    @property
    def pages(self):
        """{page number: the last page of that number received, decoded}: its fields plus "page" and "page_name"
        (see ant.pages)"""
        return self._core.pages


def core_module():
    return _core

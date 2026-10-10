"""
Performance.py

The performance settings picked from the title bar's ⚙ menu (Performance...), saved in savedSettingsANT.json:

  "gaugeFps": 60  - how many times a second the gauge needles move while easing toward a new reading;
                    0 turns the easing off (the needle jumps to each reading, about 4 a second)

Lower is lighter on the CPU: each needle frame redraws the moving parts of the gauge (see
AnalogGaugeWidget.paintEvent). Gauges watch() for changes, so a new setting applies to the ones on screen too.
"""

import weakref

import SavedSettings

#the slider's stops, lightest first
GAUGE_FPS_CHOICES = (0, 10, 15, 20, 30, 45, 60)
DEFAULT_GAUGE_FPS = 60

_gaugeFps = None #(read from the saved settings the first time it's asked for)
_watchers = []


def gauge_fps():
    global _gaugeFps
    if _gaugeFps is None:
        saved = SavedSettings.load().get("gaugeFps")
        _gaugeFps = saved if saved in GAUGE_FPS_CHOICES else DEFAULT_GAUGE_FPS
    return _gaugeFps


def set_gauge_fps(fps):
    global _gaugeFps
    if fps not in GAUGE_FPS_CHOICES or fps == gauge_fps():
        return
    _gaugeFps = fps
    SavedSettings.update(gaugeFps=fps)
    notify()


def gauge_fps_text(fps):
    if fps == 0:
        return "Off - needles jump to each reading"
    if fps == GAUGE_FPS_CHOICES[-1]:
        return f"{fps} fps - smoothest"
    return f"{fps} fps"


###Changes
def watch(obj, methodName):
    """obj.methodName() is called whenever a performance setting changes (held weakly: a deleted widget drops out)"""
    _watchers.append((weakref.ref(obj), methodName))


def notify():
    for ref, methodName in list(_watchers):
        obj = ref()
        if obj is None:
            _watchers.remove((ref, methodName))
            continue
        try:
            getattr(obj, methodName)()
        except RuntimeError:
            #its Qt side is gone
            _watchers.remove((ref, methodName))

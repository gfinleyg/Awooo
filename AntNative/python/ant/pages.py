"""ANT+ data pages for the six sensor types: every page decoded field by field, and the pages a display sends.

decode_page(device_type, payload) gives a dict of the page's fields plus "page" (its number) and "page_name", or
None for a combined speed & cadence sensor (it has no pages). The same dict is in every received message as
message["page_data"] and, per page number, in each profile's `pages`. A page that isn't decoded still has "page"
and "page_name" ("unknown").

  Heart rate (120)        default, cumulative operating time, manufacturer/product id, previous heart beat,
                          swim interval summary, capabilities, battery status
  Speed (123) and         default, cumulative operating time, manufacturer/product id, battery status, motion
  cadence (122)           (stopped)
  Bicycle power (11)      calibration response, power only (pedal balance, accumulated power), wheel torque,
                          crank torque, torque effectiveness and pedal smoothness, crank torque frequency
  Fitness equipment (17)  calibration, general FE data (speed, distance, elapsed time, heart rate, state),
                          settings, metabolic data, treadmill/elliptical/rower/climber/Nordic skier data, trainer
                          data and torque, the control pages (as echoed back), capabilities, user configuration
  All but 121             common pages: request, command status, manufacturer and product information, battery

The decoding is the native layer's (src/core/ant_pages.cpp, which documents each page's source and units).
"""

from .transport.native import ant_native as _core


def decode_page(device_type, payload):
    return _core.decode_page(device_type, bytes(payload))


def basic_resistance(percent):
    """FE-C page 48: percent of the trainer's maximum resistance"""
    return _core.page_basic_resistance(percent)


def target_power(watts):
    """FE-C page 49: ERG mode target, 0-4000 W"""
    return _core.page_target_power(watts)


def wind_resistance(coefficient_kg_per_m=None, wind_speed_kmh=None, drafting_factor=None):
    """FE-C page 50 (simulation mode); None leaves a field at the trainer's default"""
    return _core.page_wind_resistance(coefficient_kg_per_m, wind_speed_kmh, drafting_factor)


def track_resistance(grade_percent=None, rolling_resistance=None):
    """FE-C page 51 (simulation mode): grade -200 to 200%, coefficient of rolling resistance"""
    return _core.page_track_resistance(grade_percent, rolling_resistance)


def user_configuration(user_weight_kg=None, bicycle_weight_kg=None, wheel_diameter_m=None, gear_ratio=None,
                       wheel_diameter_offset_mm=None):
    """FE-C page 55"""
    return _core.page_user_configuration(user_weight_kg, bicycle_weight_kg, wheel_diameter_m, gear_ratio,
                                         wheel_diameter_offset_mm)


def request_page(page, times=1, acknowledged=False):
    """Common page 70: ask a sensor to send `page` (e.g. FE capabilities, 0x36; command status, 0x47) `times`
    times, or until acknowledged"""
    return _core.page_request(page, times, acknowledged)


def power_manual_zero():
    """Bicycle power page 1: manual zero offset calibration request"""
    return _core.page_power_manual_zero()

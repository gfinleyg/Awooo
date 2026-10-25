"""ANT+ data pages for the six sensor types: every page decoded field by field, and the pages a display sends.

decode_page(device_type, payload) gives a dict of the page's fields plus "page" (its number) and "page_name", or
None for a combined speed & cadence sensor (it has no pages). The same dict is in every received message as
message["page_data"] and, per page number, in each profile's `pages`. A page that isn't decoded still has "page"
and "page_name" ("unknown").

  Heart rate (120)        default, cumulative operating time, manufacturer/product id, previous heart beat,
                          swim interval summary, capabilities (incl. gym mode), battery status, device
                          information (measured or computed beat times)
  Speed (123) and         default, cumulative operating time, manufacturer/product id, battery status, motion
  cadence (122)           (stopped)
  Bicycle power (11)      calibration (manual and auto zero, crank torque frequency), parameters (crank length and
                          sensor status, power phase, rider position, advanced capabilities), measurement output,
                          power only (pedal balance, accumulated power), wheel and crank torque, torque
                          effectiveness and pedal smoothness, torque barycenter, crank torque frequency, cycling
                          dynamics (force angles, pedal position: seated/standing, platform center offset)
  Fitness equipment (17)  calibration, general FE data (speed, distance, elapsed time, heart rate, state),
                          settings, metabolic data, treadmill/elliptical/rower/climber/Nordic skier data, trainer
                          data and torque, the control pages (as echoed back), capabilities, user configuration
  Power (11) and FE (17)  common pages: request, command status, generic command, open channel, mode settings,
                          (multi-component) manufacturer and product information, battery, time and date,
                          subfield data (temperature, pressure, humidity, ...), memory level, paired devices,
                          error description

Heart rate monitors and speed/cadence sensors don't send common pages (and a legacy one's page byte means
nothing), so only their own pages are decoded. Layouts are from the ANT+ device profiles and Common Data Pages
document; src/core/ant_pages.cpp gives each page's table and units.
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


def request_page(page, times=1, acknowledged=False, descriptor_1=None, descriptor_2=None):
    """Common page 70: ask a sensor to send `page` (e.g. FE capabilities, 0x36; command status, 0x47) `times`
    times, or until acknowledged. The descriptors pick a subpage: e.g. page 2 (power parameters) with descriptor_1
    1 for the crank parameters, 0xFD/0xFE for the advanced capabilities."""
    return _core.page_request(page, times, acknowledged, descriptor_1, descriptor_2)


def mode_settings(sport_mode):
    """Common page 76: put a sensor in a sport mode (1 running, 2 cycling, 5 swimming)"""
    return _core.page_mode_settings(sport_mode)


def heart_rate_feature_command(gym_mode):
    """Heart rate page 32: gym mode on (True) or off (False); None leaves it as it is"""
    return _core.page_heart_rate_feature_command(gym_mode)


def power_manual_zero():
    """Bicycle power page 1: manual zero offset calibration request"""
    return _core.page_power_manual_zero()


def power_auto_zero(enabled):
    """Bicycle power page 1: auto zero on or off"""
    return _core.page_power_auto_zero(enabled)


def power_crank_length(millimetres):
    """Bicycle power page 2 subpage 1: crank length (110-236.5 mm); None asks the sensor to determine it"""
    return _core.page_power_crank_length(millimetres)


def power_cycling_dynamics(eight_hz=True, power_phase=True, pedal_center_offset=True, rider_position=True,
                           torque_barycenter=True):
    """Bicycle power page 2 subpage 0xFE: switch on the cycling dynamics pages - force angles, pedal center offset,
    rider position (seated/standing, page 0xE2) and torque barycenter. They need the sensor at 8 Hz (a 4 Hz
    channel still receives them)."""
    return _core.page_power_cycling_dynamics(eight_hz, power_phase, pedal_center_offset, rider_position,
                                             torque_barycenter)

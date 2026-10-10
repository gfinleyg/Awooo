"""
Units.py

The measurements picked from the title bar's ⚙ menu (Measurements...), saved in savedSettingsANT.json:

  "weightUnit":      "lbs" or "kg"    - the rider's weight on the setup screen
  "powerWeightUnit": "W/kg" or "W/lb" - power to weight on the synopsis (only with a weight set)
  "distanceUnit":    "mi" or "km"     - speed (MPH / KM/H) and distance on the workout screen and synopsis
  "riderWeightKg":   the rider's weight, always kept in kg (0: not set)

Everything is measured in one unit and only converted for showing: weight in kg, speed in MPH and distance in
miles (as the sensors are read), so switching units never loses anything and lbs with W/kg (or kg with W/lb) just
works. Screens that show a unit watch() for changes.
"""

import weakref

import SavedSettings

WEIGHT_UNITS = ("lbs", "kg")
POWER_WEIGHT_UNITS = ("W/kg", "W/lb")
DISTANCE_UNITS = ("mi", "km")
LBS_PER_KG = 2.20462262
KM_PER_MILE = 1.609344
#the weights the setup screen accepts (0 is always fine: not set), in kg
MIN_WEIGHT_KG, MAX_WEIGHT_KG = 20, 300


class _Units:
    weight = "lbs"
    powerWeight = "W/kg"
    distance = "mi"
    riderWeightKg = 0.0


current = _Units()
_watchers = []


def load():
    """The saved units and weight (anything missing or unknown keeps its default)"""
    settings = SavedSettings.load()
    if settings.get("weightUnit") in WEIGHT_UNITS:
        current.weight = settings["weightUnit"]
    if settings.get("powerWeightUnit") in POWER_WEIGHT_UNITS:
        current.powerWeight = settings["powerWeightUnit"]
    if settings.get("distanceUnit") in DISTANCE_UNITS:
        current.distance = settings["distanceUnit"]
    weight = settings.get("riderWeightKg")
    if isinstance(weight, (int, float)) and (weight == 0 or MIN_WEIGHT_KG <= weight <= MAX_WEIGHT_KG):
        current.riderWeightKg = float(weight)


def set_units(weight=None, powerWeight=None, distance=None):
    if weight in WEIGHT_UNITS:
        current.weight = weight
    if powerWeight in POWER_WEIGHT_UNITS:
        current.powerWeight = powerWeight
    if distance in DISTANCE_UNITS:
        current.distance = distance
    SavedSettings.update(weightUnit=current.weight, powerWeightUnit=current.powerWeight, distanceUnit=current.distance)
    notify()


def set_rider_weight_kg(kg):
    current.riderWeightKg = float(kg)
    SavedSettings.update(riderWeightKg=round(current.riderWeightKg, 3))
    notify()


###Weight
def weight_from_kg(kg):
    return kg*LBS_PER_KG if current.weight == "lbs" else kg


def weight_to_kg(value):
    return value/LBS_PER_KG if current.weight == "lbs" else value


def weight_text(kg=None):
    """The rider's weight (or kg) in the weight unit, e.g. "165 lbs"; "0 lbs" when it isn't set"""
    kg = current.riderWeightKg if kg is None else kg
    return f"{format_weight(kg)} {current.weight}"


def format_weight(kg):
    """kg in the weight unit, to a tenth (no decimal for a whole number)"""
    value = round(weight_from_kg(kg), 1)
    return f"{value:g}" if value != int(value) else str(int(value))


def weight_range():
    """The weights the setup screen accepts, in the weight unit"""
    return weight_from_kg(MIN_WEIGHT_KG), weight_from_kg(MAX_WEIGHT_KG)


###Power to weight
def has_weight():
    return current.riderWeightKg > 0


def power_to_weight(watts):
    """Watts per kg or lb of the rider; None without a weight"""
    if not has_weight() or watts is None:
        return None
    perUnit = current.riderWeightKg*LBS_PER_KG if current.powerWeight == "W/lb" else current.riderWeightKg
    return watts/perUnit


def power_to_weight_text(watts, prefix=" (", suffix=")"):
    """e.g. " (3.25 W/kg)" for watts, or "" without a weight"""
    value = power_to_weight(watts)
    if value is None:
        return ""
    return f"{prefix}{value:.2f} {current.powerWeight}{suffix}"


###Speed and distance
def speed(mph):
    return mph*KM_PER_MILE if current.distance == "km" else mph


def distance(miles):
    return miles*KM_PER_MILE if current.distance == "km" else miles


def speed_unit():
    return "KM/H" if current.distance == "km" else "MPH"


def distance_unit(long=True):
    if current.distance == "km":
        return "Km" if long else "km"
    return "Miles" if long else "mi"


###Changes
def watch(obj, methodName):
    """obj.methodName() is called whenever the units or weight change (held weakly: a deleted window drops out)"""
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

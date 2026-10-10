import time

from AppState import AppState


def MinimizedDuration(duration):
    if duration < 60:
        return time.strftime("%#Ss", time.gmtime(duration))
    elif duration < 3600:
        if duration % 60 == 0:
            return time.strftime("%#Mm", time.gmtime(duration))
        else:
            return time.strftime("%#Mm %#Ss", time.gmtime(duration))
    else:
        if duration % 3600 == 0:
            return time.strftime("%#Hh", time.gmtime(duration))
        elif duration % 60 == 0:
            return time.strftime("%#Hh %#Mm", time.gmtime(duration))
        else:
            return time.strftime("%#Hh %#Mm %#Ss", time.gmtime(duration))


def ProcessNonIntervalWorkoutLineText(line, difficulty):
    #(Musical Cadence Mode says RPM where these say Watts)
    #free   [position + " " + MinimizedDuration(duration)]
    #steady [position + " " + adjustedPower + " Watts " + MinimizedDuration(duration)]
    #ramp   [position + " " + str(adjustedStartPower) + "-" + str(adjustedEndPower) + " Watts " + MinimizedDuration(duration)]
    positionList = ["Seated", "Seated High Cadence", "Seated Low Cadence", "Standing", "Single Leg", "Switch Single Leg"]
    if line[0] == 0:
        return [positionList[line[2]] + " " + MinimizedDuration(line[1])]
    unit = " " + AppState.target_unit() + " "
    if line[0] == 1:
        return [positionList[line[3]] + " " + str(line[2][difficulty]) + unit + MinimizedDuration(line[1])]
    elif line[0] == 2:
        return [positionList[line[3]] + " " + str(line[2][difficulty][0]) + "-" + str(line[2][difficulty][1]) + unit + MinimizedDuration(line[1])]


def ProcessWorkoutLine(line, difficulty):
    #interval [multiplier, interval block 0, interval block 1, interval block 2] 
    if line[0] in [0,1,2]:
        return ProcessNonIntervalWorkoutLineText(line, difficulty)
    elif line[0] == 3:
        intvList = []
        intvList.append(str(line[1]) + "x")
        intvList.extend(ProcessNonIntervalWorkoutLineText(line[2], difficulty))
        intvList.extend(ProcessNonIntervalWorkoutLineText(line[3], difficulty))
        #optional extra block in the interval
        if len(line) == 5:
            intvList.extend(ProcessNonIntervalWorkoutLineText(line[4], difficulty))
        return intvList
    
#Musical Cadence Mode's song lines in the workout lists ("<length> : <Artist> - <Title>", see
#MusicCadence.song_header) and their look; no block's text has " : " in it
SONG_HEADER_CSS = "background-color:qlineargradient(x1:0 y1:0, x2:1 y2:1, stop:0 #28275e stop:1 #9fc5d1); color: white;"

def IsSongHeader(txt):
    return AppState.cadenceMode and " : " in txt

def TargetsFromText(txt):
    """A block label's target(s) as (low, high) - high is 0 for a single target - read from the number before its
    "Watts" (or "RPM"); None for text without one (free ride, headers)"""
    words = txt.split(" ")
    for unit in ("Watts", "RPM"):
        if unit in words:
            index = words.index(unit)
            if index == 0:
                return None
            values = words[index-1].split("-")
            try:
                return (int(values[0]), int(values[1]) if len(values) > 1 else 0)
            except ValueError:
                return None
    return None

def GetMaxOrZero(lst):
    if len(lst) == 0:
        return 0
    else:
        return max(lst)
    
def GetMaxOrZeroAndIndex(lst):
    if len(lst) == 0:
        return 0, 0
    else:
        return max(lst), max(range(len(lst)), key=lst.__getitem__)
    
#Sensor dropouts on the graphs: a gap between readings up to DROPOUT_ZERO_SECONDS long is drawn as a
#straight line from the last reading to the next; anything longer drops to zero DROPOUT_EDGE_SECONDS
#(about one ANT+ message period) after the last reading and stays there until the sensor is heard again
DROPOUT_ZERO_SECONDS = 5.0
DROPOUT_EDGE_SECONDS = 0.25

def WithDropoutsAsZero(times, values, endTime=None):
    """Plot-only copy of a sensor series (x list, y list) with long gaps filled with zeros. endTime is
    'now' for a live graph (or the end of the ride): a sensor silent that long at the end also shows zero"""
    if len(times) == 0:
        return [], []
    outTimes, outValues = [times[0]], [values[0]]
    for i in range(1, len(times)):
        if times[i] - times[i-1] > DROPOUT_ZERO_SECONDS:
            outTimes += [times[i-1] + DROPOUT_EDGE_SECONDS, times[i] - DROPOUT_EDGE_SECONDS]
            outValues += [0, 0]
        outTimes.append(times[i])
        outValues.append(values[i])
    if endTime is not None and endTime - times[-1] > DROPOUT_ZERO_SECONDS:
        outTimes += [times[-1] + DROPOUT_EDGE_SECONDS, endTime]
        outValues += [0, 0]
    return outTimes, outValues

def is_float(element: any):
    #If you expect None to be passed:
    if element is None: 
        return False
    try:
        float(element)
        return True
    except ValueError:
        return False


#The devices picked for the workout, one "<device type> <device number>" string each. Kept in memory for the
#life of the app (cleared by "Disconnect and Rescan"); every read and write goes through these so only well
#formed, known, unique entries are ever stored
SELECTABLE_DEVICE_TYPES = ("11", "17", "120", "121", "122", "123")
_selectedDeviceLines = []

def ReadSelectedDeviceLines():
    return list(_selectedDeviceLines)

def WriteSelectedDeviceLines(lines):
    cleanLines = []
    for rawLine in lines:
        parts = rawLine.split()
        if len(parts) != 2 or parts[0] not in SELECTABLE_DEVICE_TYPES or not parts[1].isdigit():
            continue
        if not 0 < int(parts[1]) <= 65535: #ANT+ device numbers are 16 bit, 0 is the wildcard
            continue
        line = parts[0] + " " + str(int(parts[1]))
        if line not in cleanLines:
            cleanLines.append(line)
    _selectedDeviceLines[:] = cleanLines

def ClearSelectedDeviceLines():
    _selectedDeviceLines.clear()

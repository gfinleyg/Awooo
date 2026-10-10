###Zwift workout files (.zwo) as Awooo workout (.awo) lines: the app reads a .zwo through this as if it were the
###.awo it converts to (see WorkoutSetupWindow.process_workout_file), and zwo2awo.py writes those lines to .awo files.
###
###  <name>                              T:<name>
###  <FreeRide>/<Freeride>, <MaxEffort>  0:<duration>               (MaxEffort with a "Max effort!" message)
###  <SteadyState>                       1:<duration>,<power>
###  <Warmup>/<Cooldown>/<Ramp>          2:<duration>,<PowerLow>,<PowerHigh>  (start to end, as Zwift rides them)
###  <IntervalsT>                        3:<Repeat>;1:<OnDuration>,<OnPower>;1:<OffDuration>,<OffPower>
###  <textevent> inside a block          |<message>|<seconds>|<start> on that block
###  Cadence="<rpm>" on a block          a "<rpm> RPM" message, and the seated high/low cadence position for
###                                      fast/slow ones
###  <ftpOverride>                       the powers as explicit watts (<power> x the override, e.g. 250W): the
###                                      workout was written for that FTP, whatever the rider's
###
###.awo limits are kept: blocks longer than MAX_BLOCK_SECONDS are split, a "ramp" with the same start and end is a
###steady block, powers are clamped to what a workout file allows, and messages lose any | or ; (the .awo field
###separators). An interval's messages in its first rep go on its blocks (an .awo interval shows them every rep); one
###with messages later on is written out rep by rep instead, so each shows once, as in Zwift.

import os
import re
import xml.etree.ElementTree as ET

MAX_BLOCK_SECONDS = 1800
MIN_FTP_MULTIPLIER, MAX_FTP_MULTIPLIER = 0.05, 3
MIN_EXPLICIT_WATTS, MAX_EXPLICIT_WATTS = 1, 2000
#how long a text event shows (Zwift's own is about this), unless it says otherwise
MESSAGE_SECONDS = 10
#a block's Cadence at or above / below these is seated high / low cadence (positions 1 / 2)
HIGH_CADENCE_RPM, LOW_CADENCE_RPM = 100, 70


class ZwoError(Exception):
    pass


def _number(element, *names):
    """The first of the attributes (names, any capitalization) the element has, as a float; None if none"""
    attributes = {key.lower(): value for key, value in element.attrib.items()}
    for name in names:
        value = attributes.get(name.lower())
        if value is not None:
            try:
                return float(value)
            except ValueError:
                raise ZwoError(f"<{element.tag}> {name}=\"{value}\" isn't a number")
    return None


def _clean_message(text):
    return " ".join(text.replace("|", "/").replace(";", ",").split())


class _Writer:
    def __init__(self, ftpOverride):
        self.ftpOverride = ftpOverride
        self.lines = []
        self.warnings = []
        self.clamped = False

    def power(self, multiplier):
        """A Zwift power (x FTP) as an .awo power: a multiplier, or watts with a ftpOverride"""
        if self.ftpOverride:
            watts = int(round(multiplier*self.ftpOverride))
            if not MIN_EXPLICIT_WATTS <= watts <= MAX_EXPLICIT_WATTS:
                self.clamped = True
                watts = min(max(watts, MIN_EXPLICIT_WATTS), MAX_EXPLICIT_WATTS)
            return f"{watts}W"
        if not MIN_FTP_MULTIPLIER <= multiplier <= MAX_FTP_MULTIPLIER:
            self.clamped = True
            multiplier = min(max(multiplier, MIN_FTP_MULTIPLIER), MAX_FTP_MULTIPLIER)
        return f"{round(multiplier, 3):g}"

    @staticmethod
    def messages(messages, start, duration):
        """|message|seconds|start fields for the messages (block second, text, seconds) inside [start, start+duration)"""
        fields = ""
        for second, text, seconds in messages:
            if start <= second < start + duration:
                offset = int(second - start)
                fields += f"|{text}|{max(1, min(seconds, duration - offset))}|{offset}"
        return fields

    @staticmethod
    def pieces(duration):
        """A duration split into whole-second pieces of at most MAX_BLOCK_SECONDS"""
        count = -(-duration//MAX_BLOCK_SECONDS)
        return [duration//count + (1 if i < duration % count else 0) for i in range(count)]

    def block(self, kind, duration, powers=(), position=0, messages=()):
        """A free ride (kind 0), steady (1) or ramp (2) block, split into pieces if it's too long; powers are x FTP"""
        if kind == 2 and self.power(powers[0]) == self.power(powers[1]):
            kind, powers = 1, powers[:1]
        start = 0
        for piece in self.pieces(duration):
            if kind == 2:
                low, high = powers
                values = [low + (high - low)*start/duration, low + (high - low)*(start + piece)/duration]
                text = f"2:{piece},{self.power(values[0])},{self.power(values[1])}"
                if self.power(values[0]) == self.power(values[1]):
                    text = f"1:{piece},{self.power(values[0])}"
            elif kind == 1:
                text = f"1:{piece},{self.power(powers[0])}"
            else:
                text = f"0:{piece}"
            if position:
                text += f",{position}"
            self.lines.append(text + self.messages(messages, start, piece))
            start += piece

    def sub_block(self, duration, power, position, messages):
        """One block of an interval line (no splitting: an interval's blocks are each at most MAX_BLOCK_SECONDS)"""
        text = f"1:{duration},{self.power(power)}" + (f",{position}" if position else "")
        return text + self.messages(messages, 0, duration)


def _duration(element, name="Duration"):
    value = _number(element, name)
    return 0 if value is None else int(round(value))


def _cadence(element, name="Cadence"):
    """A block's Cadence: (position, message) - the position for a fast or slow one, and an "<rpm> RPM" message"""
    rpm = _number(element, name)
    if rpm is None or rpm <= 0:
        return 0, None
    position = 1 if rpm >= HIGH_CADENCE_RPM else 2 if rpm < LOW_CADENCE_RPM else 0
    return position, f"{int(round(rpm))} RPM"


def _text_events(element, duration):
    """The block's <textevent>s as (block second, text, seconds shown)"""
    events = []
    for event in element:
        if event.tag.lower() != "textevent":
            continue
        text = _clean_message(event.attrib.get("message", ""))
        if not text:
            continue
        offset = int(round(_number(event, "timeoffset") or 0))
        seconds = int(round(_number(event, "duration") or MESSAGE_SECONDS))
        if 0 <= offset < duration:
            events.append((offset, text, max(1, seconds)))
    return events


TAG_RE = re.compile(r"<(/?)([A-Za-z_][\w.-]*)[^>]*?(/?)>")


def _drop_stray_closing_tags(text):
    """The file's text without closing tags that don't close anything (hand edited files have them, and Zwift
    doesn't mind), and the line number of each one dropped"""
    stack, drop = [], []
    for match in TAG_RE.finditer(text):
        closing, name, selfClosing = match.group(1), match.group(2), match.group(3)
        if selfClosing:
            continue
        if not closing:
            stack.append(name)
        elif stack and stack[-1] == name:
            stack.pop()
        elif name in stack:
            #closes an outer one: what it skips over are left open, close them here too
            while stack.pop() != name:
                pass
        else:
            drop.append(match)
    for match in reversed(drop):
        text = text[:match.start()] + text[match.end():]
    return text, [text.count("\n", 0, match.start()) + 1 for match in drop]


def convert(path):
    """The .zwo file's workout as .awo lines: (lines, title, warnings). Raises ZwoError if it can't be read"""
    repairs = []
    try:
        with open(path, "rb") as f:
            data = f.read()
        try:
            root = ET.fromstring(data)
        except ET.ParseError as e:
            text, lines = _drop_stray_closing_tags(data.decode("utf-8", errors="replace"))
            if not lines:
                raise ZwoError(f"Not a readable workout file ({e})")
            try:
                root = ET.fromstring(text)
            except ET.ParseError:
                raise ZwoError(f"Not a readable workout file ({e})")
            repairs = [f"Left out a closing tag that doesn't close anything (line {n})" for n in lines]
    except OSError as e:
        raise ZwoError(f"Can't open the file ({e.strerror})")
    if root.tag.lower() != "workout_file":
        raise ZwoError("Not a Zwift workout file (no <workout_file>)")
    children = {child.tag.lower(): child for child in root}
    sport = (children.get("sporttype").text or "").strip().lower() if children.get("sporttype") is not None else ""
    workout = children.get("workout")
    if workout is None:
        raise ZwoError("No <workout> in the file")
    ftpOverride = None
    if children.get("ftpoverride") is not None:
        try:
            ftpOverride = float(children["ftpoverride"].text)
        except (TypeError, ValueError):
            raise ZwoError("<ftpOverride> isn't a number")
        if ftpOverride <= 0:
            ftpOverride = None
    title = " ".join((children["name"].text or "").split()) if children.get("name") is not None else ""

    writer = _Writer(ftpOverride)
    writer.warnings += repairs
    if sport and sport != "bike":
        writer.warnings.append(f"Written for {sport}, not bike")
    if ftpOverride:
        writer.warnings.append(f"Powers are watts, for an FTP of {ftpOverride:g} (its <ftpOverride>)")
    if title:
        writer.lines.append("T:" + title)
    for element in workout:
        tag = element.tag.lower()
        if tag == "textevent":
            continue
        duration = _duration(element)
        position, cadenceMessage = _cadence(element)
        if tag in ("freeride", "maxeffort"):
            if duration <= 0:
                continue
            messages = _text_events(element, duration)
            if tag == "maxeffort":
                messages.insert(0, (0, "Max effort!", min(MESSAGE_SECONDS, duration)))
            if cadenceMessage:
                messages.insert(0, (0, cadenceMessage, duration))
            writer.block(0, duration, (), position, messages)
        elif tag == "steadystate":
            if duration <= 0:
                continue
            low, high = _number(element, "PowerLow"), _number(element, "PowerHigh")
            power = _number(element, "Power")
            messages = ([(0, cadenceMessage, duration)] if cadenceMessage else []) + _text_events(element, duration)
            if power is None and low is not None and high is not None:
                writer.block(2, duration, (low, high), position, messages)
            elif power is not None:
                writer.block(1, duration, (power,), position, messages)
            else:
                raise ZwoError(f"<SteadyState> without a Power")
        elif tag in ("warmup", "cooldown", "ramp"):
            if duration <= 0:
                continue
            low, high = _number(element, "PowerLow"), _number(element, "PowerHigh")
            if low is None or high is None:
                raise ZwoError(f"<{element.tag}> needs PowerLow and PowerHigh")
            messages = ([(0, cadenceMessage, duration)] if cadenceMessage else []) + _text_events(element, duration)
            writer.block(2, duration, (low, high), position, messages)
        elif tag == "intervalst":
            repeat = int(round(_number(element, "Repeat") or 1))
            onDuration, offDuration = _duration(element, "OnDuration"), _duration(element, "OffDuration")
            onPower = _number(element, "OnPower", "PowerOnHigh", "PowerOnLow")
            offPower = _number(element, "OffPower", "PowerOffLow", "PowerOffHigh")
            if onPower is None or offPower is None:
                raise ZwoError("<IntervalsT> needs OnPower and OffPower")
            if repeat <= 0 or onDuration <= 0:
                continue
            onPosition, onCadence = _cadence(element, "Cadence")
            offPosition, offCadence = _cadence(element, "CadenceResting")
            events = _text_events(element, repeat*(onDuration + offDuration))
            onMessages = [(0, onCadence, onDuration)] if onCadence else []
            offMessages = [(0, offCadence, offDuration)] if offCadence else []
            if offDuration <= 0:
                #no rest: just the efforts back to back
                writer.block(1, repeat*onDuration, (onPower,), onPosition, onMessages + events)
            elif events and all(second < onDuration + offDuration for second, _, _ in events):
                #messages in the first rep stay on its blocks (shown again each rep): the interval stays one block
                onMessages += [(s, t, n) for s, t, n in events if s < onDuration]
                offMessages += [(s - onDuration, t, n) for s, t, n in events if s >= onDuration]
                writer.lines.append(f"3:{repeat};" + writer.sub_block(onDuration, onPower, onPosition, onMessages) +
                                    ";" + writer.sub_block(offDuration, offPower, offPosition, offMessages))
            elif events:
                #messages later on show once, where they are in Zwift: the interval rep by rep
                for rep in range(repeat):
                    repStart = rep*(onDuration + offDuration)
                    writer.block(1, onDuration, (onPower,), onPosition,
                                 onMessages + [(s - repStart, t, n) for s, t, n in events])
                    writer.block(1, offDuration, (offPower,), offPosition,
                                 offMessages + [(s - repStart - onDuration, t, n) for s, t, n in events])
            elif onDuration > MAX_BLOCK_SECONDS or offDuration > MAX_BLOCK_SECONDS:
                for _ in range(repeat):
                    writer.block(1, onDuration, (onPower,), onPosition, onMessages)
                    writer.block(1, offDuration, (offPower,), offPosition, offMessages)
            else:
                writer.lines.append(f"3:{repeat};" + writer.sub_block(onDuration, onPower, onPosition, onMessages) +
                                    ";" + writer.sub_block(offDuration, offPower, offPosition, offMessages))
        else:
            if duration > 0:
                writer.warnings.append(f"<{element.tag}> isn't supported, ridden as a free ride")
                writer.block(0, duration, (), 0, [(0, element.tag, min(MESSAGE_SECONDS, duration))])
            else:
                writer.warnings.append(f"<{element.tag}> isn't supported, left out")
    if writer.clamped:
        writer.warnings.append("Some powers were outside what a workout file allows, they're limited to it")
    if not any(line[:1] in "0123" for line in writer.lines):
        raise ZwoError("The workout has no blocks")
    return writer.lines, title, writer.warnings


def awo_path(zwoPath):
    return os.path.splitext(zwoPath)[0] + ".awo"


def write_awo(zwoPath, awoPath=None):
    """Converts a .zwo file to an .awo file beside it (or at awoPath); returns (awo path, warnings)"""
    lines, _, warnings = convert(zwoPath)
    awoPath = awoPath or awo_path(zwoPath)
    with open(awoPath, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return awoPath, warnings

import importlib.util
import os

import pytest

from conftest import APP_DIR
import ZwoConvert
from workout_files import WorkoutParser, verify


def zwo(body, header="<name>Test Ride</name>", name="ride.zwo"):
    with open(name, "w", encoding="utf-8") as f:
        f.write(f"<workout_file>{header}<sportType>bike</sportType><workout>{body}</workout></workout_file>")
    return name


def convert(body, **kwargs):
    lines, title, warnings = ZwoConvert.convert(zwo(body, **kwargs))
    return lines, warnings


def assert_valid(lines):
    """Whatever the converter writes is a workout the app (and the verifier) reads"""
    parser = WorkoutParser(ftp=250)
    assert parser.parse(lines), parser.rejections
    assert verify(lines) == ""


def test_basic_blocks():
    lines, warnings = convert("""
        <Warmup Duration="300" PowerLow="0.25" PowerHigh="0.75"/>
        <SteadyState Duration="600" Power="0.88"/>
        <FreeRide Duration="120"/>
        <Cooldown Duration="300" PowerLow="0.7" PowerHigh="0.3"/>""")
    assert lines == ["T:Test Ride", "2:300,0.25,0.75", "1:600,0.88", "0:120", "2:300,0.7,0.3"]
    assert warnings == []
    assert_valid(lines)


def test_attribute_names_are_case_insensitive():
    lines, _ = convert('<steadystate duration="60" power="0.5"/><RAMP DURATION="60" POWERLOW="0.5" POWERHIGH="0.6"/>')
    assert lines[1:] == ["1:60,0.5", "2:60,0.5,0.6"]


def test_ramp_with_the_same_start_and_end_is_steady():
    lines, _ = convert('<Ramp Duration="60" PowerLow="0.5" PowerHigh="0.5"/>')
    assert lines[1:] == ["1:60,0.5"]
    #a SteadyState with a range is a ramp
    lines, _ = convert('<SteadyState Duration="60" PowerLow="0.5" PowerHigh="0.7"/>')
    assert lines[1:] == ["2:60,0.5,0.7"]


def test_long_blocks_are_split():
    lines, _ = convert('<SteadyState Duration="4000" Power="0.6"/><FreeRide Duration="1801"/>')
    assert lines[1:] == ["1:1334,0.6", "1:1333,0.6", "1:1333,0.6", "0:901", "0:900"]
    assert_valid(lines)


def test_long_ramp_is_split_into_a_continuous_ramp():
    lines, _ = convert('<Warmup Duration="3600" PowerLow="0.4" PowerHigh="0.8"/>')
    assert lines[1:] == ["2:1800,0.4,0.6", "2:1800,0.6,0.8"]


def test_split_pieces():
    for duration in (1, 1800, 1801, 3600, 5401, 10000):
        pieces = ZwoConvert._Writer.pieces(duration)
        assert sum(pieces) == duration and max(pieces) <= 1800 and max(pieces) - min(pieces) <= 1


def test_intervals():
    lines, _ = convert('<IntervalsT Repeat="4" OnDuration="30" OffDuration="60" OnPower="1.2" OffPower="0.5"/>')
    assert lines[1:] == ["3:4;1:30,1.2;1:60,0.5"]
    assert_valid(lines)


def test_intervals_alternative_power_names_and_no_rest():
    lines, _ = convert('<IntervalsT Repeat="3" OnDuration="20" OffDuration="0" PowerOnHigh="1.5" PowerOffLow="0.4"/>')
    assert lines[1:] == ["1:60,1.5"]


def test_intervals_with_blocks_too_long_are_written_out():
    lines, _ = convert('<IntervalsT Repeat="2" OnDuration="2000" OffDuration="60" OnPower="0.8" OffPower="0.5"/>')
    assert lines[1:] == ["1:1000,0.8", "1:1000,0.8", "1:60,0.5"]*2
    assert_valid(lines)


def test_interval_messages_in_the_first_rep_stay_on_the_interval():
    lines, _ = convert('<IntervalsT Repeat="3" OnDuration="30" OffDuration="30" OnPower="1.2" OffPower="0.5">'
                       '<textevent timeoffset="5" message="Go | hard; now"/>'
                       '<textevent timeoffset="40" message="Rest" duration="5"/></IntervalsT>')
    assert lines[1:] == ["3:3;1:30,1.2|Go / hard, now|10|5;1:30,0.5|Rest|5|10"]
    assert_valid(lines)


def test_interval_messages_later_on_are_written_rep_by_rep():
    lines, _ = convert('<IntervalsT Repeat="2" OnDuration="30" OffDuration="30" OnPower="1.2" OffPower="0.5">'
                       '<textevent timeoffset="70" message="Second rep"/></IntervalsT>')
    assert lines[1:] == ["1:30,1.2", "1:30,0.5", "1:30,1.2|Second rep|10|10", "1:30,0.5"]
    assert_valid(lines)


def test_text_events_on_blocks():
    lines, _ = convert('<SteadyState Duration="60" Power="0.6"><textevent timeoffset="55" message="End soon"/>'
                       '<textevent timeoffset="90" message="Past the end"/><textevent timeoffset="0" message="  "/>'
                       '</SteadyState>')
    #a message lasts to the end of its block at most; one past the end or with no text is left out
    assert lines[1:] == ["1:60,0.6|End soon|5|55"]
    assert_valid(lines)


def test_max_effort_and_cadence():
    lines, _ = convert('<MaxEffort Duration="20"/><SteadyState Duration="60" Power="0.6" Cadence="105"/>'
                       '<SteadyState Duration="60" Power="0.6" Cadence="60"/>'
                       '<SteadyState Duration="60" Power="0.6" Cadence="85"/>')
    assert lines[1:] == ["0:20|Max effort!|10|0", "1:60,0.6,1|105 RPM|60|0", "1:60,0.6,2|60 RPM|60|0",
                         "1:60,0.6|85 RPM|60|0"]
    assert_valid(lines)


def test_interval_cadences():
    lines, _ = convert('<IntervalsT Repeat="2" OnDuration="30" OffDuration="30" OnPower="1.2" OffPower="0.5" '
                       'Cadence="110" CadenceResting="65"/>')
    assert lines[1:] == ["3:2;1:30,1.2,1|110 RPM|30|0;1:30,0.5,2|65 RPM|30|0"]


def test_ftp_override_writes_watts():
    lines, warnings = convert('<SteadyState Duration="60" Power="0.8"/><Ramp Duration="60" PowerLow="0.5" '
                              'PowerHigh="1.0"/>', header="<name>W</name><ftpOverride>250</ftpOverride>")
    assert lines[1:] == ["1:60,200W", "2:60,125W,250W"]
    assert any("250" in w for w in warnings)
    assert_valid(lines)


def test_out_of_range_powers_are_clamped():
    lines, warnings = convert('<SteadyState Duration="60" Power="5"/><SteadyState Duration="60" Power="0.01"/>')
    assert lines[1:] == ["1:60,3", "1:60,0.05"]
    assert any("limited" in w for w in warnings)
    lines, warnings = convert('<SteadyState Duration="60" Power="10"/>',
                              header="<ftpOverride>300</ftpOverride>")
    assert lines == ["1:60,2000W"]


def test_unsupported_blocks():
    lines, warnings = convert('<SteadyState Duration="60" Power="0.6"/><SolidState Duration="30"/><Mystery/>')
    assert lines[1:] == ["1:60,0.6", "0:30|SolidState|10|0"]
    assert len(warnings) == 2


def test_other_sport_and_zero_length_blocks():
    with open("run.zwo", "w") as f:
        f.write('<workout_file><sportType>run</sportType><workout><SteadyState Duration="0" Power="1"/>'
                '<SteadyState Duration="60" Power="1"/></workout></workout_file>')
    lines, title, warnings = ZwoConvert.convert("run.zwo")
    assert title == "" and lines == ["1:60,1"]
    assert any("run" in w for w in warnings)


def test_stray_closing_tags_are_repaired():
    lines, warnings = convert('<SteadyState Duration="60" Power="0.6"></SteadyState></SteadyState>'
                              '<FreeRide Duration="30"/>')
    assert lines[1:] == ["1:60,0.6", "0:30"]
    assert any("closing tag" in w for w in warnings)


@pytest.mark.parametrize("text, error", [
    ("not xml at all", "Not a readable workout file"),
    ("<workout_file><workout>", "Not a readable workout file"),
    ("<html><body/></html>", "no <workout_file>"),
    ("<workout_file><name>x</name></workout_file>", "No <workout>"),
    ("<workout_file><ftpOverride>abc</ftpOverride><workout><FreeRide Duration='60'/></workout></workout_file>",
     "ftpOverride"),
    ("<workout_file><workout><SteadyState Duration='60'/></workout></workout_file>", "without a Power"),
    ("<workout_file><workout><SteadyState Duration='x' Power='1'/></workout></workout_file>", "isn't a number"),
    ("<workout_file><workout><Ramp Duration='60' PowerLow='0.5'/></workout></workout_file>", "PowerHigh"),
    ("<workout_file><workout><IntervalsT Repeat='2' OnDuration='30'/></workout></workout_file>", "OnPower"),
    ("<workout_file><workout></workout></workout_file>", "no blocks"),
])
def test_errors(text, error):
    with open("bad.zwo", "w") as f:
        f.write(text)
    with pytest.raises(ZwoConvert.ZwoError, match=error):
        ZwoConvert.convert("bad.zwo")


def test_missing_file():
    with pytest.raises(ZwoConvert.ZwoError, match="Can't open"):
        ZwoConvert.convert("nowhere.zwo")


def test_write_awo():
    zwo('<SteadyState Duration="60" Power="0.6"/>')
    path, warnings = ZwoConvert.write_awo("ride.zwo")
    assert path == "ride.awo" and warnings == []
    with open(path, encoding="utf-8") as f:
        assert f.read() == "T:Test Ride\n1:60,0.6\n"


def test_zwo2awo_finds_files(tmp_path):
    spec = importlib.util.spec_from_file_location("zwo2awo", os.path.join(APP_DIR, "zwo2awo.py"))
    zwo2awo = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(zwo2awo)
    (tmp_path / "b.ZWO").write_text("x")
    (tmp_path / "a.zwo").write_text("x")
    (tmp_path / "c.awo").write_text("x")
    assert zwo2awo.zwo_files([]) == [os.path.join(".", "a.zwo"), os.path.join(".", "b.ZWO")]
    assert zwo2awo.zwo_files(["one.zwo"]) == ["one.zwo"]

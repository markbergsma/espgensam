#!/usr/bin/env python3
"""Host-side test for tools/sam2yaml.py.

    python3 tests/test_sam2yaml.py

No framework, matching the C++ host tests alongside it. Needs PyYAML, because the tool does;
if your system interpreter lacks it, run this with ESPHome's own.

The fixture below is a synthetic setup file rather than a real one: captures/ is not tracked,
so a committed test cannot read anyone's GLM configuration. Its shape and its numbers are
taken from a real GLM 5.2 file and from a bus capture of GLM applying it, so the assertions
are still about observed behaviour -- the subwoofer's three per-group levels and AutoPhase
delays, the two device classes' differing slot layouts, and their differing design rates.

What is worth testing here is almost entirely the mapping, because that is where a mistake is
both easy and silent: a band placed in the wrong slot, or a rounded gain, produces a perfectly
valid configuration that mistunes a speaker with nothing to show for it.
"""

import io
import os
import sys
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import sam2yaml  # noqa: E402
import yaml  # noqa: E402  (a dependency of sam2yaml, so importable if it is)


# --- fixture ---------------------------------------------------------------------------

def _bands(key, entries, sr):
    out = []
    for i, (freq, gain, q) in enumerate(entries, start=1):
        out.append(f"{key}:{i}")
        out.append(f"Frequency:{freq}")
        out.append(f"Gain:{gain}")
        if q is not None:
            out.append(f"Q_Value:{q}")
        out.append(f"SR:{sr}")
    return out


def _subwoofer_node(serial, level, phase, source_input, sr=12000, notches=None):
    notches = notches or [(56.1739, -6.05847, 4.68839)] + [(40, 0, 1)] * 19
    return [
        "Class:SubwooferGen2",
        "----",
        f"Serial:{serial}",
        "Group_ON:1",
        f"Level_Sensitivity:{level}",
        f"Calibration_Level:{level}",
        "Optional_Gain:0",
        "Time-of-flight_Compensation:0",
        "Video_Delay:0",
        "Group_Sensitivity:0",
        "SubwooferGroupID:0",
        f"Input:{source_input}",
        "CrossoverFrequency(Hz):90",
        f"Phase(degrees):{phase}",
        "LFE_+10:0",
        "LFE_Channel:0",
        "LFE_Level:0",
        *_bands("Notch", notches, sr),
        "Data_End:",
    ]


def _twoway_node(serial, level, source_input, sr=48000, crossover=90):
    return [
        "Class:TwowayGen2",
        "----",
        f"Serial:{serial}",
        "Group_ON:1",
        f"Level_Sensitivity:{level}",
        "Optional_Gain:0",
        "Time-of-flight_Compensation:0",
        f"Input:{source_input}",
        *([f"CrossoverFrequency(Hz):{crossover}"] if crossover is not None else []),
        *_bands("LP_Shelve", [(743.8, -3.12747e-05, None), (118.711, -0.177536, None)], sr),
        *_bands("HP_Shelve", [(14999, -0.0199986, None), (14999, -0.0199986, None)], sr),
        *_bands("Notch", [(198.371, -4.55673, 3.49809)] + [(1000, 0, 1)] * 15, sr),
        "Data_End:",
    ]


def _ghost_node():
    """A device GLM retained but that was never really present; see the README note."""
    return [
        "Class:SubwooferGen2",
        "----",
        "Serial:2468013",
        "Group_ON:0",
        "Level_Sensitivity:0",
        "Input:4",
        "CrossoverFrequency(Hz):90",
        "Phase(degrees):0",
        *_bands("Notch", [(40, 0, 1)] * 20, 12000),
        "Data_End:",
    ]


def make_sam(**kw):
    lines = [
        "Magic: GLM5_v2",
        "GLM_Version: 5.2.0",
        "Devices: 3",
        "Groups: 2",
        "Header_End: ",
        # inventory
        "Model:7350A", "----", "Type:1", "Serial:1842915", "Barcode:7350APM88123456", "Data_End: ",
        "Model:8330A", "----", "Type:2", "Serial:1654321", "Barcode:8330AP99234567", "Data_End: ",
        "Model:7350A", "----", "Type:1", "Serial:2468013", "Barcode:8330AP99234567", "Data_End: ",
        # group 1: subwoofer on AES3 sum, main on AES3 A, plus the ghost
        "Group_Name:Listening Position A",
        "----",
        "Group_Crossover:90",
        # The 9320 controller block is a nested record with its own namespace whose keys
        # collide with group-level ones. Group_Crossover here is what a leak would corrupt.
        "9320DATA:",
        "Serial:999999",
        "Group_Crossover:999",
        "Video_Delay:7",
        "Group_Nodes:3",
        *_subwoofer_node("1842915", kw.get("g1_level", -1.9258), -165, 3),
        # Omits CrossoverFrequency(Hz), so it inherits the group's and would pick up a leak.
        *_twoway_node("1654321", 0, 1, crossover=None),
        *_ghost_node(),
        "Group_End:",
        # group 2: everything analog, different AutoPhase result
        "Group_Name:Listening Position B",
        "----",
        "Group_Crossover:90",
        "Group_Nodes:2",
        *_subwoofer_node("1842915", -8.37833, 45, 3),
        *_twoway_node("1654321", -1.0774, 4),
        "Group_End:",
    ]
    return "\n".join(lines) + "\n"


KNOWN = {"1842915", "1654321"}


# --- harness ---------------------------------------------------------------------------

failures = 0


def check(ok, what):
    global failures
    print(f"  {'[ok]  ' if ok else '[FAIL]'} {what}")
    if not ok:
        failures += 1


def convert(text=None, known=KNOWN):
    """Write the fixture to a temp file, convert it, and capture warnings."""
    import tempfile

    with tempfile.NamedTemporaryFile("w", suffix=".sam", delete=False, encoding="latin-1") as fh:
        fh.write(text if text is not None else make_sam())
        path = fh.name
    try:
        err = io.StringIO()
        with unittest.mock.patch("sys.stderr", err):
            groups = sam2yaml.convert(sam2yaml.parse_sam(path), known)
        return groups, err.getvalue()
    finally:
        os.unlink(path)


# --- structure ---------------------------------------------------------------------------

def test_structure():
    groups, _ = convert()
    check(len(groups) == 2, "two groups are converted")
    check([g["name"] for g in groups] == ["Listening Position A", "Listening Position B"],
          "group names and order are preserved")
    check([len(g["devices"]) for g in groups] == [2, 2],
          "the ghost device is dropped, leaving two real devices per group")
    for g in groups:
        for d in g["devices"]:
            check(len(d["filters"]) == sam2yaml.PEQ_BAND_COUNT,
                  f"{g['name']} / {d['unique_id']} has exactly 20 wire slots")


def test_ghost_device_is_reported_not_silently_dropped():
    _, warnings = convert()
    check("2468013" in warnings and "not in --monitors" in warnings,
          "skipping an unknown device is reported by unique_id")
    # Without a filter it must be kept: silently discarding a device the user does own
    # would be far worse than emitting one they do not.
    groups, _ = convert(known=None)
    check(len(groups[0]["devices"]) == 3, "without --monitors every device is emitted")


# --- slot layout, which differs by device class ---------------------------------------------

def test_subwoofer_slots_are_all_peaking():
    groups, _ = convert()
    sub = groups[0]["devices"][0]
    check(sub["unique_id"] == "1842915", "subwoofer is the first device of group 1")
    check(all(s["type"] == "notch" for s in sub["filters"]),
          "a subwoofer fills all 20 slots with peaking bands and has no shelves")
    first = sub["filters"][0]
    check(first["frequency"] == 56.1739 and first["gain"] == -6.05847 and first["q"] == 4.68839,
          "subwoofer Notch:1 lands in wire slot 0 with its parameters intact")


def test_twoway_slots_are_shelves_then_peaking():
    groups, _ = convert()
    two = groups[0]["devices"][1]
    kinds = [s["type"] for s in two["filters"]]
    check(kinds[0:2] == ["low_shelf"] * 2, "two-way slots 0-1 are the low shelves")
    check(kinds[2:4] == ["high_shelf"] * 2, "two-way slots 2-3 are the high shelves")
    check(kinds[4:] == ["notch"] * 16, "two-way slots 4-19 are the peaking bands")
    check(two["filters"][4]["frequency"] == 198.371,
          "two-way Notch:1 lands in wire slot 4, not slot 0")


# --- values that must not be rounded or reinterpreted ----------------------------------------

def test_level_is_carried_at_full_precision():
    groups, _ = convert()
    # -8.37833 and -8.3783 encode to level words eleven counts apart.
    check(groups[1]["devices"][0]["level_db"] == -8.37833,
          "subwoofer level keeps every digit of -8.37833")
    check(groups[0]["devices"][0]["level_db"] == -1.9258, "group 1 subwoofer level is -1.9258")


def test_phase_becomes_the_delay_glm_transmits():
    groups, _ = convert()
    # Both values were observed on the wire for these exact phases at a 90 Hz crossover.
    check(groups[0]["devices"][0]["delay_samples"] == 289, "phase -165 deg at 90 Hz -> 289 samples")
    check(groups[1]["devices"][0]["delay_samples"] == 67, "phase +45 deg at 90 Hz -> 67 samples")
    check(all(d["delay_samples"] == 0 for g in groups for d in g["devices"]
              if d["unique_id"] == "1654321"),
          "a device with no AutoPhase angle gets no delay")


def test_input_enum_maps_to_source():
    groups, _ = convert()
    check(groups[0]["devices"][0]["source"] == "aes3_sum", "Input:3 is AES3 A+B sum")
    check(groups[0]["devices"][1]["source"] == "aes3_a", "Input:1 is AES3 A")
    check(groups[1]["devices"][1]["source"] == "analog", "Input:4 is analog")


def test_9320_block_does_not_leak_into_the_group():
    # The controller block is a nested record whose keys collide with group-level ones. The
    # fixture sets Group_Crossover:999 inside it, and group 1's two-way omits its own
    # crossover, so a leak would be visible as that device inheriting 999 Hz.
    groups, warnings = convert()
    two = groups[0]["devices"][1]
    check(two["crossover"] == 90,
          "a device inheriting the group crossover gets 90 Hz, not the 9320 block's 999")
    check(two["unique_id"] == "1654321", "the 9320 block's Serial does not become a device")
    check("Video_Delay is 7" not in warnings, "the 9320 block's Video_Delay is not reported")


# --- refusals -------------------------------------------------------------------------------

def test_unexpected_design_rate_is_fatal():
    # An 83x1-class monitor is reported to run a 96 kHz DSP path. Emitting its bands as if
    # they were designed at 48 kHz would mistune every one of them, so this must not pass.
    text = make_sam().replace("SR:48000", "SR:96000")
    try:
        convert(text)
        check(False, "a design rate the firmware will not use is rejected")
    except sam2yaml.SamError as err:
        check("96000" in str(err) and "48000" in str(err),
              "a design rate the firmware will not use is rejected, naming both rates")


def test_not_calibrated_sentinel_does_not_become_silence():
    # GLM writes -999 for a device it has not calibrated. Passed through, that encodes as
    # digital silence and mutes the speaker.
    text = make_sam(g1_level=-999)
    groups, warnings = convert(text)
    check(groups[0]["devices"][0]["level_db"] == 0.0, "a -999 level is replaced with 0 dB")
    check("not calibrated" in warnings, "replacing the sentinel is reported")


def test_dropped_fields_are_reported():
    text = make_sam().replace("Optional_Gain:0", "Optional_Gain:6", 1)
    _, warnings = convert(text)
    check("Optional_Gain" in warnings and "not applied" in warnings,
          "a non-zero field with no known encoding is reported rather than dropped silently")


# --- emitted YAML -----------------------------------------------------------------------------

def test_emitted_yaml_round_trips():
    """Emit, re-parse, and check the result says what the converter meant.

    This is the property that matters and the one a hand-rolled emitter could not check: it
    holds for any group name GLM's free-text field might contain, rather than for the cases
    someone thought to quote. The names below are all ones a naive emitter mangles -- `yes`
    and `on` become booleans, `12345` an integer, `*star` an alias reference, `a: b` a nested
    mapping.
    """
    for hostile in ["yes", "on", "12345", "*star", "a: b", "- dash", "@at", ""]:
        text = make_sam().replace("Group_Name:Listening Position A", f"Group_Name:{hostile}")
        groups, _ = convert(text)
        loaded = yaml.safe_load(sam2yaml.emit_yaml(groups, "fixture.sam"))
        check(loaded[0]["name"] == hostile, f"group name {hostile!r} survives a round trip")

    # Surrounding whitespace is stripped when the setup file is parsed, not lost in the
    # emitter: every value in that format is trimmed, and GLM does not write names that
    # depend on padding.
    text = make_sam().replace("Group_Name:Listening Position A", "Group_Name:  padded  ")
    groups, _ = convert(text)
    check(groups[0]["name"] == "padded", "surrounding whitespace in a name is trimmed on parse")

    groups, _ = convert()
    loaded = yaml.safe_load(sam2yaml.emit_yaml(groups, "fixture.sam"))
    sub = loaded[0]["devices"][0]
    check(sub["level_db"] == -1.9258, "level survives a round trip unrounded")
    check(sub["delay_samples"] == 289, "delay survives a round trip")
    check(sub["source"] == "aes3_sum", "source survives a round trip")
    check(len(sub["filters"]) == sam2yaml.PEQ_BAND_COUNT, "all 20 slots survive a round trip")
    check(sub["filters"][0] == {"type": "notch", "frequency": 56.1739, "gain": -6.05847,
                                "q": 4.68839},
          "a band survives a round trip with every parameter intact")
    check(loaded[1]["devices"][0]["level_db"] == -8.37833,
          "-8.37833 survives a round trip, not rounded to -8.3783")


def test_emitted_yaml_keeps_precision():
    groups, _ = convert()
    text = sam2yaml.emit_yaml(groups, "fixture.sam")
    check("-8.37833" in text, "the emitted YAML carries the unrounded level")
    check("56.1739" in text, "the emitted YAML carries the unrounded frequency")
    check("q: 4.68839" in text, "peaking bands emit their Q")
    check("high_shelf, frequency: 14999.0, gain: -0.0199986}" in text,
          "shelving bands emit no Q, which the schema rejects for them")
    check("q: 4.68839}" in text, "peaking bands emit their Q inline with the rest of the band")


def main():
    global failures
    print("sam2yaml")
    for name, fn in sorted(globals().items()):
        if not (name.startswith("test_") and callable(fn)):
            continue
        try:
            fn()
        except Exception as err:  # noqa: BLE001 - a raising test is a failing test
            # Reported rather than propagated so one broken case does not hide the rest, and
            # so a regression that trips a refusal path still reads as a failure.
            print(f"  [FAIL] {name} raised {type(err).__name__}: {err}")
            failures += 1
    if failures:
        print(f"\n{failures} check(s) FAILED")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

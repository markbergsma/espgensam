#!/usr/bin/env python3
"""Host-side test for components/gensam/sam_import.py, the GLM 5 setup file mapping.

    python3 tests/test_sam_import.py

No framework and no dependencies, matching the C++ host tests alongside it: sam_import is
standard library only, so this runs on any interpreter. The fixture it converts lives in
tests/sam_fixture.py and is shared with the two tests either side of this one.

What is worth testing here is almost entirely the mapping, because that is where a mistake is
both easy and silent: a band placed in the wrong slot, or a rounded gain, produces a perfectly
valid configuration that mistunes a speaker with nothing to show for it.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from sam_fixture import (  # noqa: E402
    GHOST_ID,
    check,
    collect_warnings,
    convert,
    make_sam,
    run,
    sam_import,
    write_sam,
)


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
            check(len(d["filters"]) == sam_import.PEQ_BAND_COUNT,
                  f"{g['name']} / {d['unique_id']} has exactly 20 wire slots")


def test_ghost_device_is_reported_not_silently_dropped():
    _, warnings = convert()
    check(str(GHOST_ID) in warnings and "skipped" in warnings,
          "skipping an unknown device is reported by unique_id")
    # Without a filter it must be kept: silently discarding a device the user does own
    # would be far worse than emitting one they do not.
    groups, _ = convert(known=None)
    check(len(groups[0]["devices"]) == 3, "with no monitor list every device is emitted")


def test_unique_id_is_an_int():
    # The schema takes a positive_int and the hub compares it against the monitors: block,
    # so the serial is parsed here rather than by each caller.
    groups, _ = convert()
    check(all(isinstance(d["unique_id"], int) for g in groups for d in g["devices"]),
          "every unique_id is an int, not the file's text")
    check(groups[0]["devices"][0]["unique_id"] == 1842915, "the subwoofer's id is 1842915")


def test_non_numeric_serial_is_skipped_with_a_warning():
    # A serial that is not a number cannot be named by a monitor's unique_id either, so it
    # could never be matched to hardware; dropping it silently would hide that.
    text = make_sam().replace("Serial:1654321", "Serial:ABC1234")
    groups, warnings = convert(text, known=None)
    check(all(d["unique_id"] != "ABC1234" for g in groups for d in g["devices"]),
          "a non-numeric serial does not reach the group")
    check("not a number" in warnings, "a non-numeric serial is reported")


# --- slot layout, which differs by device class ---------------------------------------------

def test_subwoofer_slots_are_all_peaking():
    groups, _ = convert()
    sub = groups[0]["devices"][0]
    check(sub["unique_id"] == 1842915, "subwoofer is the first device of group 1")
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


def test_group_sensitivity_is_summed_into_the_level():
    # GLM applies the group-level trim on top of each device's own calibration, and sends the
    # sum as one level word. Confirmed against captures/glm_lfe_capture.log: a group carrying
    # Group_Sensitivity:-0.3 with Level_Sensitivity:0 put 10 01 00 7B A7 8D on the wire to all
    # three speakers, and round(10^(-0.3/20) * 8388607) == 0x7BA78D exactly. Dropping it plays
    # the whole group 0.3 dB loud.
    text = make_sam().replace("Group_Sensitivity:0", "Group_Sensitivity:-0.3", 1)
    groups, warnings = convert(text)
    check(groups[0]["devices"][0]["level_db"] == -1.9258 + -0.3,
          "Group_Sensitivity is added to the device's own Level_Sensitivity")
    check("Group_Sensitivity" not in warnings,
          "an applied Group_Sensitivity is no longer reported as dropped")

    # The sentinel path has to keep working: -999 means "not calibrated", so the device
    # contributes 0 dB and the group trim still applies on top of that.
    text = make_sam(g1_level=-999).replace("Group_Sensitivity:0", "Group_Sensitivity:-0.3", 1)
    groups, _ = convert(text)
    check(groups[0]["devices"][0]["level_db"] == -0.3,
          "a -999 device level leaves the group trim intact rather than discarding it")


def test_summed_level_is_not_clamped_on_its_way_to_the_schema():
    # Summing can leave the -60..0 dB range the group schema accepts where neither field could
    # alone, so it is worth pinning that convert() passes the sum through untouched. The schema
    # is the backstop for a setup file by design -- see test_sam_config.py -- and clamping here
    # would hide an out-of-range level rather than refuse to build.
    text = make_sam().replace("Group_Sensitivity:0", "Group_Sensitivity:-70", 1)
    groups, _ = convert(text)
    check(groups[0]["devices"][0]["level_db"] == -1.9258 + -70,
          "a sum past the schema floor reaches the schema rather than being trimmed")


def test_phase_becomes_the_delay_glm_transmits():
    groups, _ = convert()
    # Both values were observed on the wire for these exact phases at a 90 Hz crossover.
    check(groups[0]["devices"][0]["delay_samples"] == 289, "phase -165 deg at 90 Hz -> 289 samples")
    check(groups[1]["devices"][0]["delay_samples"] == 67, "phase +45 deg at 90 Hz -> 67 samples")
    check(all(d["delay_samples"] == 0 for g in groups for d in g["devices"]
              if d["unique_id"] == 1654321),
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
    check(two["unique_id"] == 1654321, "the 9320 block's Serial does not become a device")
    check("Video_Delay is 7" not in warnings, "the 9320 block's Video_Delay is not reported")


# --- refusals -------------------------------------------------------------------------------

def test_unexpected_design_rate_is_fatal():
    # An 83x1-class monitor is reported to run a 96 kHz DSP path. Emitting its bands as if
    # they were designed at 48 kHz would mistune every one of them, so this must not pass.
    text = make_sam().replace("SR:48000", "SR:96000")
    try:
        convert(text)
        check(False, "a design rate the firmware will not use is rejected")
    except sam_import.SamError as err:
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


# --- the `groups:` shape, which both front ends depend on ------------------------------------

def test_every_slot_is_filled_including_the_unused_ones():
    # A band the setup file leaves unset must still be transmitted, as an explicit bypass:
    # the speaker keeps whatever was in that slot before otherwise.
    text = make_sam().replace("Notch:20\nFrequency:40\nGain:0\nQ_Value:1\nSR:12000\n", "")
    doc = sam_import.to_group_config(convert(text)[0])
    sub = doc[0]["devices"][0]
    check(len(sub["filters"]) == sam_import.PEQ_BAND_COUNT, "a device always has exactly 20 bands")
    check(sub["filters"][19] == {"type": "bypass", "frequency": 1000, "gain": 0},
          "a slot the file does not fill becomes an explicit bypass band")


def test_q_is_emitted_only_for_peaking_bands():
    # The schema rejects a q on a shelving band: GLM exposes no slope control there and the
    # firmware uses a fixed value per type.
    doc = sam_import.to_group_config(convert()[0])
    two = doc[0]["devices"][1]
    check(all("q" not in b for b in two["filters"][0:4]), "shelving bands carry no q")
    check(all("q" in b for b in two["filters"][4:]), "peaking bands carry a q")


def test_crossover_is_absent_when_the_file_gives_none():
    # Left out rather than guessed, so the group's own default applies.
    text = make_sam().replace("CrossoverFrequency(Hz):90", "").replace("Group_Crossover:90", "")
    doc = sam_import.to_group_config(convert(text)[0])
    check(all("crossover" not in d for g in doc for d in g["devices"]),
          "no crossover anywhere in the file means no crossover key")
    doc = sam_import.to_group_config(convert()[0])
    check(doc[0]["devices"][0]["crossover"] == 90, "a crossover in the file is carried across")


def test_load_groups_is_the_composition_it_claims_to_be():
    import tempfile

    with tempfile.TemporaryDirectory() as tmp, collect_warnings():
        path = write_sam(tmp)
        expected = sam_import.to_group_config(
            sam_import.convert(sam_import.parse_sam(path), {1842915, 1654321})
        )
        check(sam_import.load_groups(path, {1842915, 1654321}) == expected,
              "load_groups equals parse_sam + convert + to_group_config")


if __name__ == "__main__":
    sys.exit(run("sam_import", globals()))

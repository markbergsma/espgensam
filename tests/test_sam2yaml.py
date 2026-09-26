#!/usr/bin/env python3
"""Host-side test for tools/sam2yaml.py, the YAML emitter.

    python3 tests/test_sam2yaml.py

Needs PyYAML, because the tool does; if your system interpreter lacks it, run this with
ESPHome's own. The mapping itself is tested without PyYAML in tests/test_sam_import.py; what
is left here is emission, where the risks are different: a group name GLM's free-text field
allows but YAML reinterprets, and a level rounded on the way out.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

from sam_fixture import check, convert, full_band_sam, make_sam, run, sam_import  # noqa: E402

import sam2yaml  # noqa: E402
import yaml  # noqa: E402  (a dependency of sam2yaml, so importable if it is)


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
    check(len(sub["filters"]) == sam_import.PEQ_BAND_COUNT, "all 20 slots survive a round trip")
    check(sub["filters"][0] == {"type": "notch", "frequency": 56.1739, "gain": -6.05847,
                                "q": 4.68839},
          "a band survives a round trip with every parameter intact")
    check(loaded[1]["devices"][0]["level_db"] == -8.37833,
          "-8.37833 survives a round trip, not rounded to -8.3783")


def test_emitted_yaml_is_what_the_component_would_have_validated():
    """The text and the in-config route must describe the same groups.

    Both go through sam_import.to_group_config, and this is what keeps that true: a round
    trip through the emitter must reload to exactly the structure the component's `sam_file:`
    expansion hands to the schema. Anything the emitter added or dropped shows up here.
    """
    groups, _ = convert()
    loaded = yaml.safe_load(sam2yaml.emit_yaml(groups, "fixture.sam"))
    check(loaded == sam_import.to_group_config(groups),
          "the emitted YAML reloads to exactly what the component validates")

    groups, _ = convert(full_band_sam())
    loaded = yaml.safe_load(sam2yaml.emit_yaml(groups, "fixture.sam"))
    check(loaded == sam_import.to_group_config(groups),
          "a full-band file reloads to exactly what the component validates")
    check(loaded[0]["devices"][0]["crossover"] == "full_band", "full band is emitted as full_band")


def test_emitted_yaml_keeps_precision():
    groups, _ = convert()
    text = sam2yaml.emit_yaml(groups, "fixture.sam")
    check("-8.37833" in text, "the emitted YAML carries the unrounded level")
    check("56.1739" in text, "the emitted YAML carries the unrounded frequency")
    check("q: 4.68839" in text, "peaking bands emit their Q")
    check("high_shelf, frequency: 14999.0, gain: -0.0199986}" in text,
          "shelving bands emit no Q, which the schema rejects for them")
    check("q: 4.68839}" in text, "peaking bands emit their Q inline with the rest of the band")


def test_bands_are_emitted_one_per_line():
    # A device carries twenty bands, so block style would turn a three-group setup into
    # hundreds of four-line stanzas and make the bank unreadable.
    groups, _ = convert()
    text = sam2yaml.emit_yaml(groups, "fixture.sam")
    check(text.count("- {type:") == 4 * sam_import.PEQ_BAND_COUNT,
          "every band is one inline mapping, and none are missing")


if __name__ == "__main__":
    sys.exit(run("sam2yaml", globals()))

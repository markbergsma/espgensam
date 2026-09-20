#!/usr/bin/env python3
"""Convert a GLM 5 setup file (.sam) into an espgensam ``groups:`` YAML file.

    tools/sam2yaml.py "Home Cinema.sam" --monitors 1842915,1654321,1987654 \
        -o gensam_groups.yaml

Then include the result from the hub configuration::

    gensam:
      groups: !include gensam_groups.yaml

A GLM setup file holds, for every group and every speaker in it, the AutoCal result for one
listening position: a 20-band parametric EQ, a level trim, a crossover, the input routing and
(for a subwoofer) an AutoPhase angle. Transcribing that by hand is hundreds of numbers per
group, so this does it.

Needs PyYAML, which ESPHome already depends on. If your system interpreter lacks it,
either ``pip install pyyaml`` or run this with ESPHome's own interpreter.

What it does not carry over
---------------------------
Fields whose wire encoding is unknown are dropped rather than guessed, and every dropped
non-default value is reported on stderr so nothing disappears silently: ``Optional_Gain``,
``Time-of-flight_Compensation``, ``Group_Sensitivity``, ``Video_Delay``, the ``LFE_*`` family
and ``SubwooferGroupID``. In a normally calibrated setup these are all zero.
"""

import argparse
import sys

try:
    import yaml
except ImportError:  # pragma: no cover - environment problem, not a code path
    sys.exit(
        "error: this tool needs PyYAML.\n"
        "       pip install pyyaml\n"
        "       (ESPHome already depends on it, so running this with ESPHome's own\n"
        "        interpreter also works if you would rather not install it globally.)"
    )

# .sam Input: enum -> espgensam source. GLM stores routing and sub-channel as one value.
SAM_INPUT_TO_SOURCE = {1: "aes3_a", 2: "aes3_b", 3: "aes3_sum", 4: "analog"}

# .sam band key -> (espgensam filter type, first wire slot for that key).
#
# Every device has exactly 20 slots. A two-way monitor fills them with two low shelves, two
# high shelves and sixteen peaking bands; a subwoofer has no shelves and uses all twenty as
# peaking bands. Both layouts were read directly off the 10 0E indices in a bus capture.
TWOWAY_SLOTS = {"LP_Shelve": ("low_shelf", 0), "HP_Shelve": ("high_shelf", 2), "Notch": ("notch", 4)}
SUBWOOFER_SLOTS = {"Notch": ("notch", 0)}

PEQ_BAND_COUNT = 20

# Design rate expected for each device class, and the rate the firmware will derive from the
# discovered model. A mismatch means the file was produced by a model this tool has not seen
# and would silently be designed at the wrong rate, so it is fatal rather than a warning.
CLASS_DESIGN_RATE = {"SubwooferGen2": 12000, "TwowayGen2": 48000}

# Time-of-flight sample counts are at 48 kHz on every device class, including subwoofers,
# whose PEQ is designed at 12 kHz. Confirmed against captured AutoPhase delays.
DELAY_RATE_HZ = 48000

# Levels at or below this are not real settings. GLM writes -999 for "not calibrated", and
# anything near it would encode as digital silence.
LEVEL_SENTINEL_DB = -130.0

# Fields this tool understands but does not emit, with the value that means "unset".
DROPPED_FIELDS = {
    "Optional_Gain": 0.0,
    "Time-of-flight_Compensation": 0.0,
    "Group_Sensitivity": 0.0,
    "Video_Delay": 0.0,
    "LFE_+10": 0.0,
    "LFE_Channel": 0.0,
    "LFE_Level": 0.0,
    "SubwooferGroupID": 0.0,
}

BAND_KEYS = ("Notch", "LP_Shelve", "HP_Shelve")


class SamError(Exception):
    """A setup file this tool cannot convert correctly."""


def warn(msg):
    print(f"warning: {msg}", file=sys.stderr)


def _num(text, default=None):
    try:
        return float(text)
    except (TypeError, ValueError):
        return default


def parse_sam(path):
    """Parse a .sam into {'devices': {serial: {...}}, 'groups': [{...}]}.

    The format is line-oriented ``Key:Value`` with ``Data_End:`` / ``Group_End:`` terminators
    and dashed rules between sections, so a small state machine is enough. Keys repeat within
    a block (every band re-uses Frequency/Gain), which is why bands are accumulated against
    the most recent band marker rather than collected into a dict.
    """
    with open(path, encoding="latin-1") as fh:
        raw = [line.rstrip("\r\n") for line in fh]

    if not any(line.startswith("Magic:") and "GLM5" in line for line in raw[:20]):
        warn(f"{path}: no 'Magic: GLM5_v2' header; this may not be a GLM 5 setup file")

    devices, groups = {}, []
    section = "preamble"   # preamble -> inventory -> groups
    node = group = band = None
    inventory = None
    skipping_9320 = False

    for line in raw:
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key, value = key.strip(), value.strip()

        if key == "Header_End":
            section = "inventory"
            continue

        # --- device inventory, between the header and the first group ------------------
        if section == "inventory":
            if key == "Model":
                inventory = {"model": value}
            elif key == "Data_End":
                if inventory is not None and "serial" in inventory:
                    devices[inventory["serial"]] = inventory
                inventory = None
            elif key == "Group_Name":
                section = "groups"          # inventory finished; fall through below
            elif inventory is not None:
                if key == "Serial":
                    inventory["serial"] = value
                elif key in ("Barcode", "Type", "Name"):
                    inventory[key.lower()] = value
            if section == "inventory":
                continue

        # --- groups ---------------------------------------------------------------------
        if key == "Group_Name":
            group = {"name": value, "nodes": [], "crossover": None}
            groups.append(group)
            node = band = None
            skipping_9320 = False
            continue
        if group is None:
            continue

        # The 9320 controller block re-uses generic key names; ignore it wholesale.
        if key == "9320DATA":
            skipping_9320 = True
            continue
        if key == "Group_Nodes":
            skipping_9320 = False
            continue
        if skipping_9320:
            continue

        if key == "Class":
            node = {"class": value, "bands": [], "fields": {}, "serial": None}
            group["nodes"].append(node)
            band = None
            continue
        if key == "Data_End":
            node = band = None
            continue
        if key == "Group_End":
            group = node = band = None
            continue

        if node is None:
            if key == "Group_Crossover":
                group["crossover"] = _num(value)
            continue

        # --- inside a device node --------------------------------------------------------
        if key == "Serial" and node["serial"] is None:
            node["serial"] = value
        elif key in BAND_KEYS:
            band = {"key": key, "index": int(_num(value, 0)), "sr": None}
            node["bands"].append(band)
        elif band is not None and key in ("Frequency", "Gain", "Q_Value", "SR"):
            band[key.lower()] = _num(value)
        else:
            node["fields"][key] = value

    return {"devices": devices, "groups": groups}


def band_slots(node, source_name):
    """Place a node's bands into their 20 wire slots, or raise if the layout is unexpected."""
    layout = SUBWOOFER_SLOTS if node["class"].startswith("Subwoofer") else TWOWAY_SLOTS
    expect_rate = CLASS_DESIGN_RATE.get(node["class"])
    if expect_rate is None:
        raise SamError(
            f"{source_name}: unknown device class {node['class']!r}. This tool knows "
            f"{', '.join(sorted(CLASS_DESIGN_RATE))}; a new class may use a different PEQ "
            f"slot layout or design rate, and guessing would mistune the speaker."
        )

    slots = [None] * PEQ_BAND_COUNT
    for band in node["bands"]:
        if band["key"] not in layout:
            raise SamError(
                f"{source_name}: a {node['class']} carries a {band['key']} band, which this "
                f"tool does not expect for that class"
            )
        kind, base = layout[band["key"]]
        slot = base + band["index"] - 1
        if not 0 <= slot < PEQ_BAND_COUNT:
            raise SamError(f"{source_name}: {band['key']}:{band['index']} maps outside the 20 slots")
        if slots[slot] is not None:
            raise SamError(f"{source_name}: two bands claim wire slot {slot}")

        rate = band.get("sr")
        if rate is not None and int(rate) != expect_rate:
            raise SamError(
                f"{source_name}: {band['key']}:{band['index']} is designed at {int(rate)} Hz, "
                f"but the firmware will design a {node['class']} at {expect_rate} Hz. "
                f"Emitting it would shift every filter by {int(rate) / expect_rate:g}x."
            )
        slots[slot] = {"type": kind, "frequency": band.get("frequency", 0.0),
                       "gain": band.get("gain", 0.0), "q": band.get("q_value")}
    return slots


def phase_to_delay(phase_deg, crossover_hz):
    """Convert an AutoPhase angle to the delay GLM transmits for it.

    GLM does not send the phase; it sends a time-of-flight delay in 48 kHz samples that
    realises that phase shift at the crossover frequency. This reproduced all three captured
    subwoofer delays exactly.
    """
    if crossover_hz is None or crossover_hz <= 0:
        return 0
    return int(round((phase_deg % 360.0) / 360.0 / crossover_hz * DELAY_RATE_HZ))


def convert(model, known_ids=None):
    """Turn a parsed setup file into the group structures the YAML emitter wants."""
    out = []
    for group in model["groups"]:
        gname = group["name"]
        devices = []
        for node in group["nodes"]:
            serial = node["serial"]
            where = f"group {gname!r} device {serial}"
            if serial is None:
                warn(f"group {gname!r}: a {node['class']} node has no Serial; skipped")
                continue

            if known_ids is not None and serial not in known_ids:
                inv = model["devices"].get(serial, {})
                warn(
                    f"{where} ({inv.get('model', 'unknown model')}) is not in --monitors; "
                    f"skipped. GLM setup files can retain devices that were never really "
                    f"present, so check this is not one of yours."
                )
                continue

            fields = node["fields"]
            crossover = _num(fields.get("CrossoverFrequency(Hz)"), group["crossover"])

            level = _num(fields.get("Level_Sensitivity"), 0.0)
            if level <= LEVEL_SENTINEL_DB:
                warn(f"{where}: Level_Sensitivity is {level:g}, GLM's 'not calibrated' marker; using 0 dB")
                level = 0.0

            delay = 0
            phase = _num(fields.get("Phase(degrees)"))
            if phase is not None:
                delay = phase_to_delay(phase, crossover)

            for field, unset in DROPPED_FIELDS.items():
                value = _num(fields.get(field))
                if value is not None and value != unset:
                    warn(
                        f"{where}: {field} is {value:g} and is not applied -- its wire "
                        f"encoding is unknown, so the group will differ from GLM here"
                    )

            source_key = int(_num(fields.get("Input"), 4))
            if source_key not in SAM_INPUT_TO_SOURCE:
                raise SamError(f"{where}: unrecognised Input value {source_key}")

            devices.append(
                {
                    "unique_id": serial,
                    "enabled": fields.get("Group_ON", "1") == "1",
                    "source": SAM_INPUT_TO_SOURCE[source_key],
                    "crossover": int(crossover) if crossover else None,
                    "level_db": level,
                    "delay_samples": delay,
                    "filters": band_slots(node, where),
                }
            )

        if not devices:
            warn(f"group {gname!r} has no usable devices; skipped")
            continue
        out.append({"name": gname, "devices": devices})
    return out


class _Flow(dict):
    """A mapping to emit inline, as ``{a: 1, b: 2}``.

    Only the filter bands use it. A device carries twenty of them, so block style would turn
    a three-group setup into roughly eight hundred lines of four-line stanzas; inline they
    stay one line each and the bank can be read at a glance.
    """


def _represent_flow(dumper, data):
    return dumper.represent_mapping("tag:yaml.org,2002:map", data, flow_style=True)


yaml.add_representer(_Flow, _represent_flow, Dumper=yaml.SafeDumper)


def _document(groups):
    """Build the plain data structure that gets dumped.

    Numbers are passed through as Python floats and ints rather than pre-formatted strings:
    PyYAML's float representer round-trips exactly, which matters because -8.37833 and
    -8.3783 dB encode to level words eleven counts apart.
    """
    doc = []
    for group in groups:
        devices = []
        for dev in group["devices"]:
            entry = {
                "unique_id": int(dev["unique_id"]),
                "enabled": dev["enabled"],
                "source": dev["source"],
            }
            if dev["crossover"] is not None:
                entry["crossover"] = int(dev["crossover"])
            entry["level_db"] = float(dev["level_db"])
            entry["delay_samples"] = int(dev["delay_samples"])

            bands = []
            for slot in dev["filters"]:
                if slot is None:
                    bands.append(_Flow(type="bypass", frequency=1000, gain=0))
                    continue
                band = _Flow(
                    type=slot["type"],
                    frequency=float(slot["frequency"]),
                    gain=float(slot["gain"]),
                )
                # The schema rejects a Q on a shelving band: GLM exposes no slope control for
                # those and the firmware uses a fixed value per type.
                if slot["type"] == "notch":
                    band["q"] = float(slot["q"] if slot["q"] is not None else 1.0)
                bands.append(band)
            entry["filters"] = bands
            devices.append(entry)
        doc.append({"name": group["name"], "devices": devices})
    return doc


def emit_yaml(groups, source_path):
    header = (
        f"# Generated by tools/sam2yaml.py from {source_path}\n"
        f"#\n"
        f"# Include from the hub configuration with:  groups: !include <this file>\n"
        f"# Regenerate rather than editing if the GLM setup changes.\n\n"
    )
    body = yaml.safe_dump(
        _document(groups),
        sort_keys=False,      # field order is meaningful to a reader; alphabetical is not
        default_flow_style=False,
        width=1000,           # never wrap a band across lines
        allow_unicode=True,   # group names come from a free text field
    )
    return header + body


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sam", help="GLM 5 setup file to convert")
    ap.add_argument(
        "--monitors",
        help="comma-separated unique_ids to keep, matching the hub's monitors: block. "
        "Without it every device in the file is emitted, including any GLM has retained "
        "but you do not own.",
    )
    ap.add_argument("-o", "--output", help="write here instead of stdout")
    args = ap.parse_args(argv)

    known = None
    if args.monitors:
        known = {m.strip() for m in args.monitors.split(",") if m.strip()}

    try:
        model = parse_sam(args.sam)
        groups = convert(model, known)
    except SamError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1

    if not groups:
        print("error: no convertible groups found", file=sys.stderr)
        return 1

    text = emit_yaml(groups, args.sam)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(text)
        counts = ", ".join(f"{g['name']!r} ({len(g['devices'])})" for g in groups)
        print(f"wrote {args.output}: {counts}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

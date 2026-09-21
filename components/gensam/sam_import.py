"""Read a GLM 5 setup file (.sam) and turn it into espgensam group presets.

===========================================================================================
ARCHITECTURE & DESIGN RATIONALE
===========================================================================================
A GLM setup file holds, for every group and every speaker in it, the AutoCal result for one
listening position: a 20-band parametric EQ, a level trim, a crossover, the input routing and
(for a subwoofer) an AutoPhase angle. Transcribing that by hand is hundreds of numbers per
group, so this reads it instead.

1. One Mapping, Two Front Ends:
   This module is the single source of truth for the .sam mapping. The ESPHome component
   imports it during config validation to expand `sam_file:` into groups, and
   `tools/sam2yaml.py` imports it to write the same groups out as YAML. A mistake in the
   mapping -- a band placed in the wrong slot, a rounded gain -- produces a perfectly valid
   configuration that mistunes a speaker with nothing to show for it, so the two paths must
   not be able to drift apart.

2. Standard Library Only, And Inside The Component:
   It lives under `components/gensam/` because an external component fetched with
   `source: type: git` gets only that directory, and the component needs this at validation
   time. It imports nothing but the standard library so the CLI can load it as a plain
   top-level module without dragging in ESPHome, and so validation never depends on PyYAML;
   YAML is the CLI's concern alone.

3. Device Identity:
   A device is named by the decimal number the setup file calls `Serial:`, which is the same
   number the component's `monitors:` block calls `unique_id` and the GLM protocol calls the
   hardware id. It is carried as an int throughout, because that is what the schema takes.

What it does not carry over
---------------------------
Fields whose wire encoding is unknown are dropped rather than guessed, and every dropped
non-default value is reported so nothing disappears silently: ``Optional_Gain``,
``Time-of-flight_Compensation``, ``Video_Delay`` and ``SubwooferGroupID``.

Two fields that look droppable are not:

* ``Group_Sensitivity`` is summed with the device's own ``Level_Sensitivity``; GLM sends the
  total as one level word.
* The ``LFE_*`` family is carried, except ``LFE_CrossoverFrequency(Hz)``, which GLM fixes at
  120 Hz and never transmits. ``LFE_Channel`` becomes the subwoofer's input-1 routing and
  ``LFE_Level`` plus ``LFE_+10`` become one effective level. See ``convert()``.
"""

import logging

# .sam Input: enum -> espgensam source. GLM stores routing and sub-channel as one value.
SAM_INPUT_TO_SOURCE = {1: "aes3_a", 2: "aes3_b", 3: "aes3_sum", 4: "analog"}

# .sam LFE_Channel: -> espgensam lfe_channel. Subwoofers only, and 0 means the group has no
# LFE feed at all, which is the usual case outside a surround setup. The numbering is the AES3
# sub-channel's own, not the Input: enum's: GLM puts this value straight into the sub-channel
# byte of the input-1 routing frame, so 2 travels as 0x02.
SAM_LFE_CHANNEL = {0: "none", 1: "aes3_a", 2: "aes3_b", 3: "aes3_sum"}

# Decibels the .sam LFE_+10 flag adds to LFE_Level. The ".1" channel is mastered 10 dB below
# reference for headroom, and this is the playback boost that puts it back.
LFE_PLUS_10_DB = 10.0

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

# Fields this module understands but does not emit, with the value that means "unset".
DROPPED_FIELDS = {
    "Optional_Gain": 0.0,
    "Time-of-flight_Compensation": 0.0,
    "Video_Delay": 0.0,
    "SubwooferGroupID": 0.0,
}

BAND_KEYS = ("Notch", "LP_Shelve", "HP_Shelve")

_LOGGER = logging.getLogger(__name__)


class SamError(Exception):
    """A setup file this module cannot convert correctly."""


def warn(msg):
    """Report something the conversion could not carry across faithfully.

    Logged rather than printed so the component's warnings land in the ESPHome validation log
    with everything else, while tools/sam2yaml.py installs a handler that reproduces its
    original ``warning: ...`` line on stderr. Tests replace this name with a collector.
    """
    _LOGGER.warning("%s", msg)


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
    """Turn a parsed setup file into the group structures the emitters want.

    ``known_ids`` is a set of int unique_ids to keep -- the hub's ``monitors:`` block, or the
    CLI's ``--monitors``. A GLM setup file can retain a speaker that is no longer connected,
    or one that was never really there, so anything outside the set is skipped rather than
    configured. ``None`` keeps everything.
    """
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

            try:
                unique_id = int(serial)
            except ValueError:
                warn(
                    f"{where}: Serial is not a number and so cannot be named by a monitor's "
                    f"unique_id either; skipped"
                )
                continue

            if known_ids is not None and unique_id not in known_ids:
                inv = model["devices"].get(serial, {})
                warn(
                    f"{where} ({inv.get('model', 'unknown model')}) is not in the monitor "
                    f"list; skipped. GLM setup files can retain devices that were never "
                    f"really present, so check this is not one of yours."
                )
                continue

            fields = node["fields"]
            crossover = _num(fields.get("CrossoverFrequency(Hz)"), group["crossover"])

            level = _num(fields.get("Level_Sensitivity"), 0.0)
            if level <= LEVEL_SENTINEL_DB:
                warn(f"{where}: Level_Sensitivity is {level:g}, GLM's 'not calibrated' marker; using 0 dB")
                level = 0.0

            # GLM sends one level word per device carrying both trims summed, so the group
            # offset has to be added here rather than dropped. The sentinel above is applied
            # first, deliberately: an uncalibrated device contributes 0 dB of its own but still
            # takes the group's offset, which is what GLM does.
            # Summing can leave the range the group schema accepts where neither field could
            # alone. That is left to fail validation rather than clamped here: the schema is
            # deliberately the backstop for a setup file (see test_sam_config.py), and silently
            # trimming a level is worse than refusing to build.
            level += _num(fields.get("Group_Sensitivity"), 0.0)

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

            # The LFE feed, which only a subwoofer has: the discrete ".1" channel, reaching it
            # on input 1 alongside the bass-managed program on input 0. Absent or 0 means the
            # group has no LFE at all, which is every stereo and 2.1 group.
            lfe_key = int(_num(fields.get("LFE_Channel"), 0))
            if lfe_key not in SAM_LFE_CHANNEL:
                raise SamError(f"{where}: unrecognised LFE_Channel value {lfe_key}")
            lfe_channel = SAM_LFE_CHANNEL[lfe_key]

            # A subwoofer fed both a summed program and an LFE channel would get the LFE
            # content twice: once on its own input and once folded into the sum. GLM narrows
            # the program input to avoid that, and the mapping has to do the same or the two
            # disagree on the wire -- the capture shows Input:3 going out as A alone once LFE
            # was put on B.
            #
            # Which channel it narrows to rests on a single observation, LFE on B giving A.
            # That is equally consistent with "always A", but "the channel the LFE is not on"
            # is the reading that still makes sense when the two are swapped, so it is the one
            # used here.
            if lfe_channel != "none" and SAM_INPUT_TO_SOURCE[source_key] == "aes3_sum":
                program = {"aes3_a": "aes3_b", "aes3_b": "aes3_a"}.get(lfe_channel)
                if program is None:
                    raise SamError(
                        f"{where}: Input is the AES3 A+B sum and the LFE feed is on "
                        f"{lfe_channel}, which leaves no channel for the program material"
                    )
                source_key = {v: k for k, v in SAM_INPUT_TO_SOURCE.items()}[program]

            # GLM sends one level for the LFE path with the +10 dB boost already folded in,
            # so the flag is applied here rather than carried separately.
            lfe_level = _num(fields.get("LFE_Level"), 0.0)
            if _num(fields.get("LFE_+10"), 0.0):
                lfe_level += LFE_PLUS_10_DB

            # The wire field is a signed byte of whole decibels, so a fractional trim cannot
            # be transmitted. GLM has only ever been seen writing integers here; say so rather
            # than round in silence.
            if lfe_channel != "none" and lfe_level != round(lfe_level):
                warn(
                    f"{where}: LFE level {lfe_level:g} dB is not a whole number of decibels, "
                    f"which is all the wire field carries; it will be sent as "
                    f"{round(lfe_level):g} dB"
                )

            devices.append(
                {
                    "unique_id": unique_id,
                    "enabled": fields.get("Group_ON", "1") == "1",
                    "source": SAM_INPUT_TO_SOURCE[source_key],
                    "crossover": int(crossover) if crossover else None,
                    "level_db": level,
                    "lfe_channel": lfe_channel,
                    "lfe_level_db": lfe_level,
                    "delay_samples": delay,
                    "filters": band_slots(node, where),
                }
            )

        if not devices:
            warn(f"group {gname!r} has no usable devices; skipped")
            continue
        out.append({"name": gname, "devices": devices})
    return out


def to_group_config(groups):
    """Normalise convert()'s output into the exact shape the `groups:` schema takes.

    Both front ends need this identically: an unused slot becomes an explicit bypass band, and
    a ``q`` is emitted only for a peaking band because the schema rejects one on a shelving
    band -- GLM exposes no slope control there and the firmware uses a fixed value per type.
    Normalising in two places could drift, and a drift here mistunes a speaker while still
    producing a valid configuration.

    Numbers are passed through as Python floats and ints rather than pre-formatted strings:
    -8.37833 and -8.3783 dB encode to level words eleven counts apart, so nothing here may
    round.
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
            # Left out entirely when the file gave none, so the group default still applies.
            if dev["crossover"] is not None:
                entry["crossover"] = int(dev["crossover"])
            entry["level_db"] = float(dev["level_db"])
            entry["delay_samples"] = int(dev["delay_samples"])

            # Emitted only when there is an LFE feed, so the overwhelming majority of groups
            # -- every stereo and 2.1 one -- keep the shape they had before LFE was supported
            # and the YAML stays readable.
            if dev["lfe_channel"] != "none":
                entry["lfe_channel"] = dev["lfe_channel"]
                entry["lfe_level_db"] = float(dev["lfe_level_db"])

            bands = []
            for slot in dev["filters"]:
                if slot is None:
                    bands.append({"type": "bypass", "frequency": 1000, "gain": 0})
                    continue
                band = {
                    "type": slot["type"],
                    "frequency": float(slot["frequency"]),
                    "gain": float(slot["gain"]),
                }
                if slot["type"] == "notch":
                    band["q"] = float(slot["q"] if slot["q"] is not None else 1.0)
                bands.append(band)
            entry["filters"] = bands
            devices.append(entry)
        doc.append({"name": group["name"], "devices": devices})
    return doc


def load_groups(path, known_ids=None):
    """parse_sam + convert + to_group_config: the whole conversion in one call."""
    return to_group_config(convert(parse_sam(path), known_ids))

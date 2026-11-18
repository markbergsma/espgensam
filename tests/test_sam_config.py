#!/usr/bin/env python3
"""Host-side test for the gensam component's `sam_file:` expansion.

    /opt/homebrew/Cellar/esphome/2026.8.2/libexec/bin/python tests/test_sam_config.py

Needs ESPHome, so it runs with ESPHome's own interpreter rather than the system one. It
imports the component as a plain package and calls its validators directly, which is as far
as it can go without a build: the hub schema's pin validators need CORE.target_platform, so
CONFIG_SCHEMA as a whole can only be exercised by `esphome config` against a real YAML.

The mapping is already covered in tests/test_sam_import.py. What is left here is everything
the component adds around it -- where imported groups land, which devices are kept, that the
setup file cannot smuggle a value past the schema, and that a path outside the configuration
directory resolves.
"""

import logging
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components"))

from sam_fixture import GHOST_ID, check, full_band_sam, make_sam, run, write_sam  # noqa: E402

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402
from esphome import final_validate as fv  # noqa: E402

import gensam  # noqa: E402


MONITORS = [{"unique_id": 1842915}, {"unique_id": 1654321}]

def test_runtime_group_select_requires_uploader():
    try:
        gensam._validate_hub({gensam.CONF_GROUP_SELECT: {}})
        check(False, "the selector rejects a configuration without the uploader")
    except cv.Invalid:
        check(True, "the selector rejects a configuration without the uploader")
    config = gensam._validate_hub({
        gensam.CONF_GROUP_SELECT: {},
        gensam.CONF_SAM_UPLOAD: {},
        gensam.CONF_MONITORS: [
            {gensam.CONF_NAME: "Left", gensam.CONF_UNIQUE_ID: 1842915},
            {gensam.CONF_NAME: "Right", gensam.CONF_UNIQUE_ID: 1654321},
        ],
    })
    check(gensam.CONF_GROUP_SELECT in gensam._validate_groups(config),
          "the runtime selector works before any groups have been uploaded")
    config = validate_hub({gensam.CONF_SAM_UPLOAD: {}})
    check(gensam.CONF_SAM_UPLOAD in config, "the uploader needs no configured monitor IDs")


class _Collector(logging.Handler):
    """Gather every warning the expansion emits, from the component and from sam_import."""

    def __init__(self):
        super().__init__(level=logging.WARNING)
        self.messages = []

    def emit(self, record):
        self.messages.append(record.getMessage())


def expand(monitors=MONITORS, groups=None, text=None, name="fixture.sam", path=None):
    """Run _import_sam_groups over a hand-built config and return (config, warnings).

    CORE.config_path is what cv.file_ resolves a relative `sam_file:` against, so it is
    pointed at the temporary directory the fixture is written to.
    """
    with tempfile.TemporaryDirectory() as tmp:
        written = write_sam(tmp, text, name)
        collector = _Collector()
        root = logging.getLogger()
        root.addHandler(collector)
        previous_path, previous_level = CORE.config_path, root.level
        root.setLevel(logging.WARNING)
        CORE.config_path = Path(tmp) / "test.yaml"
        try:
            config = {gensam.CONF_SAM_FILE: cv.file_(path if path is not None else written)}
            if monitors is not None:
                config[gensam.CONF_MONITORS] = monitors
            if groups is not None:
                config[gensam.CONF_GROUPS] = groups
            return gensam._import_sam_groups(config), "\n".join(collector.messages)
        finally:
            CORE.config_path = previous_path
            root.setLevel(previous_level)
            root.removeHandler(collector)


def hand_written(name="Mute Everything"):
    return gensam.GROUP_SCHEMA(
        {"name": name, "devices": [{"unique_id": 1842915, "enabled": False}]}
    )


# --- where the imported groups land ------------------------------------------------------

def test_imported_groups_come_first_without_a_yaml_default():
    config, _ = expand(groups=[hand_written()])
    names = [g[gensam.CONF_NAME] for g in config[gensam.CONF_GROUPS]]
    check(names == ["Listening Position A", "Listening Position B", "Mute Everything"],
          "the file's groups keep their order and precede the hand-written one")
    config = gensam._validate_groups(config)
    check("default_group" not in config, "YAML groups do not create a startup default")


def test_without_sam_file_nothing_changes():
    groups = [hand_written()]
    config = gensam._import_sam_groups({gensam.CONF_GROUPS: groups})
    check(config[gensam.CONF_GROUPS] == groups, "a config with no sam_file is passed through")


# --- which devices are kept ----------------------------------------------------------------

def test_devices_outside_the_monitor_list_are_skipped():
    config, warnings = expand()
    ids = [d[gensam.CONF_UNIQUE_ID]
           for g in config[gensam.CONF_GROUPS] for d in g[gensam.CONF_DEVICES]]
    check(GHOST_ID not in ids, "a device that is not a configured monitor does not reach a group")
    check(str(GHOST_ID) in warnings, "skipping it is reported by unique_id")


def test_no_monitors_imports_everything_and_says_so():
    for monitors, what in ((None, "no monitors: block"), ([{"unique_id": 0}], "unique_id 0 only")):
        config, warnings = expand(monitors=monitors)
        ids = [d[gensam.CONF_UNIQUE_ID] for d in config[gensam.CONF_GROUPS][0][gensam.CONF_DEVICES]]
        check(GHOST_ID in ids, f"with {what}, every device in the file is imported")
        check("no monitors with a unique_id" in warnings,
              f"with {what}, importing unfiltered is reported")


def test_imported_groups_pass_the_cross_checks_unchanged():
    config = gensam._validate_groups(expand()[0])
    devices = config[gensam.CONF_GROUPS][0][gensam.CONF_DEVICES]
    check(all(gensam.CONF_CROSSOVER in d for d in devices),
          "every imported device ends up with an explicit crossover")
    # The fixture's two-way omits its own crossover and inherits the group's 90 Hz.
    check(devices[1][gensam.CONF_CROSSOVER] == 90, "the inherited crossover is the group's")


def test_a_full_band_setup_file_builds():
    # The failure this replaced: "value must be at least 50 (devices[0].crossover)".
    config = gensam._validate_groups(expand(text=full_band_sam())[0])
    devices = config[gensam.CONF_GROUPS][0][gensam.CONF_DEVICES]
    check(all(d[gensam.CONF_CROSSOVER] == gensam.CROSSOVER_FULL_BAND for d in devices),
          "a full-band group validates to the full-band wire value")


def test_crossover_spellings():
    def device(crossover):
        return gensam.GROUP_DEVICE_SCHEMA({gensam.CONF_UNIQUE_ID: 1, gensam.CONF_CROSSOVER: crossover})

    check(device("full_band")[gensam.CONF_CROSSOVER] == gensam.CROSSOVER_FULL_BAND,
          "full_band is accepted")
    check(device("Full_Band")[gensam.CONF_CROSSOVER] == gensam.CROSSOVER_FULL_BAND,
          "full_band is case-insensitive")
    check(device(90)[gensam.CONF_CROSSOVER] == 90, "a frequency is still accepted")
    for bad, why in ((1, "the bare wire value 1"), (40, "below the range"), (125, "above the range"),
                     (87, "off the 5 Hz grid"), ("none", "an unknown keyword")):
        try:
            device(bad)
            check(False, f"{why} is rejected")
        except cv.Invalid:
            check(True, f"{why} is rejected")

    group = gensam._validate_groups({
        gensam.CONF_GROUPS: [gensam.GROUP_SCHEMA({
            "name": "No Sub", "crossover": "full_band", "devices": [{"unique_id": 1}],
        })],
    })
    check(group[gensam.CONF_GROUPS][0][gensam.CONF_DEVICES][0][gensam.CONF_CROSSOVER]
          == gensam.CROSSOVER_FULL_BAND, "a device inherits a group-level full_band")


def test_the_crossover_options_match_crossover_h():
    # The same list tests/test_crossover.cpp checks crossover_to_str() against.
    check(gensam.CROSSOVER_OPTIONS == ["Full band"] + [f"{hz} Hz" for hz in range(50, 121, 5)],
          "the select offers full band, then 50 to 120 Hz in 5 Hz steps")


def test_a_name_collision_with_a_hand_written_group_is_rejected():
    try:
        gensam._validate_groups(expand(groups=[hand_written("Listening Position A")])[0])
        check(False, "a hand-written group may not reuse an imported group's name")
    except cv.Invalid as err:
        check("Duplicate group name" in str(err),
              "a hand-written group reusing an imported name is rejected by name")


# --- the setup file cannot get past the schema ------------------------------------------------

def test_a_value_the_schema_rejects_fails_the_build():
    # An uncalibrated trim well below MIN_LEVEL_DB. Passing it through would push a level to
    # a speaker that no hand-written group could have expressed.
    text = make_sam(g1_level=-70)
    try:
        expand(text=text)
        check(False, "a level outside the schema's range is rejected")
    except cv.Invalid as err:
        message = str(err)
        check("Listening Position A" in message and "fixture.sam" in message,
              "the rejection names the setup file and the group it came from")


def test_an_lfe_feed_on_a_summed_program_input_is_rejected():
    # The importer narrows the program input off the LFE channel, as GLM does, so this cannot
    # arise from a setup file. A hand-written group can express it, and it would play the LFE
    # content twice -- once on the subwoofer's own LFE input and once folded into the sum.
    device = {
        gensam.CONF_UNIQUE_ID: 1842915,
        gensam.CONF_SOURCE: "aes3_sum",
        gensam.CONF_LFE_CHANNEL: "aes3_b",
    }
    try:
        gensam.GROUP_DEVICE_SCHEMA(device)
        check(False, "an LFE feed alongside a summed program input is rejected")
    except cv.Invalid as err:
        check("reproduced twice" in str(err),
              "an LFE feed alongside a summed program input is rejected, explaining why")

    # The same device on a single channel is fine, and so is the sum with no LFE feed.
    ok_single = gensam.GROUP_DEVICE_SCHEMA({**device, gensam.CONF_SOURCE: "aes3_a"})
    check(ok_single[gensam.CONF_LFE_CHANNEL] == "aes3_b",
          "an LFE feed on a single-channel program input is accepted")
    ok_sum = gensam.GROUP_DEVICE_SCHEMA({k: v for k, v in device.items()
                                         if k != gensam.CONF_LFE_CHANNEL})
    check(ok_sum[gensam.CONF_LFE_CHANNEL] == "none",
          "a summed program input with no LFE feed is accepted and defaults to no LFE")


def test_a_file_that_is_not_a_setup_file_is_rejected():
    try:
        expand(text="nothing here is a GLM setup\n")
        check(False, "a file with no convertible groups is rejected")
    except cv.Invalid as err:
        check("no usable groups" in str(err), "a file with no convertible groups is rejected")


def test_a_missing_file_is_rejected_before_anything_else():
    previous = CORE.config_path
    CORE.config_path = Path(tempfile.gettempdir()) / "test.yaml"
    try:
        cv.file_("no-such-setup.sam")
        check(False, "a sam_file that does not exist is rejected")
    except cv.Invalid as err:
        check("Could not find file" in str(err), "a sam_file that does not exist is rejected")
    finally:
        CORE.config_path = previous


# --- calibration bypass entities ----------------------------------------------------------

def validate_hub(config):
    """_validate_hub on a fresh entity registry.

    Every auto-created entity registers its name with CORE, which rejects a second one of the
    same name, so running the validator once per test needs the registry cleared in between.
    """
    CORE.unique_ids = {}
    return gensam._validate_hub(config)


def bypass_config(groups=True, monitor=None):
    """A hub config as _validate_hub sees it, with one monitor and optionally one group."""
    config = {gensam.CONF_MONITORS: [monitor or {gensam.CONF_NAME: "Left", gensam.CONF_UNIQUE_ID: 1842915}]}
    if groups:
        config[gensam.CONF_GROUPS] = [hand_written()]
    return validate_hub(config)


def test_calibration_bypass_switches_come_with_groups():
    config = bypass_config()
    hub_switch = config.get(gensam.CONF_CALIBRATION_BYPASS)
    check(hub_switch is not None and hub_switch[gensam.CONF_NAME] == "Bypass Calibration",
          "a hub-wide Bypass Calibration switch is created when there are groups")
    check(hub_switch is not None and not hub_switch[gensam.CONF_DISABLED_BY_DEFAULT],
          "the hub-wide switch is enabled by default")
    mon_switch = config[gensam.CONF_MONITORS][0].get(gensam.CONF_CALIBRATION_BYPASS)
    check(mon_switch is not None and mon_switch[gensam.CONF_NAME] == "Left Bypass Calibration",
          "each monitor gets its own Bypass Calibration switch")
    check(mon_switch is not None and mon_switch[gensam.CONF_DISABLED_BY_DEFAULT],
          "the per-monitor switch is disabled by default, like the other per-speaker settings")


def test_calibration_bypass_switches_come_with_a_setup_file():
    config, _ = expand(monitors=[{gensam.CONF_NAME: "Left", gensam.CONF_UNIQUE_ID: 1842915}])
    config = validate_hub(config)
    check(gensam.CONF_CALIBRATION_BYPASS in config
          and gensam.CONF_CALIBRATION_BYPASS in config[gensam.CONF_MONITORS][0],
          "groups imported from sam_file count, since the import runs first")


def test_no_groups_means_nothing_to_bypass():
    config = bypass_config(groups=False)
    check(gensam.CONF_CALIBRATION_BYPASS not in config,
          "no hub-wide bypass switch without groups")
    check(gensam.CONF_CALIBRATION_BYPASS not in config[gensam.CONF_MONITORS][0],
          "no per-monitor bypass switch without groups")

def test_runtime_upload_exposes_calibration_bypass():
    config = validate_hub({gensam.CONF_SAM_UPLOAD: {}})
    check(gensam.CONF_CALIBRATION_BYPASS in config,
          "the runtime uploader exposes the system calibration bypass")


def test_monitor_diagnostics_are_enabled_by_default():
    CORE.unique_ids = {}
    static_monitor = gensam.MONITOR_SCHEMA({gensam.CONF_NAME: "Static"})
    static_config = validate_hub({
        gensam.CONF_MONITORS: [static_monitor],
        gensam.CONF_SAM_UPLOAD: {},
    })
    discovery_config = validate_hub({
        gensam.CONF_DISCOVERY_SLOTS: 2,
        gensam.CONF_SAM_UPLOAD: {},
    })
    monitors = static_config[gensam.CONF_MONITORS] + discovery_config[gensam.CONF_MONITORS]
    for monitor in monitors:
        for key in gensam.MONITOR_DIAGNOSTICS:
            entity = monitor.get(key)
            check(entity is not None
                  and str(entity.get("entity_category")) == "diagnostic"
                  and not entity.get(gensam.CONF_DISABLED_BY_DEFAULT, True),
                  f"{monitor[gensam.CONF_NAME]} {key} is an enabled-by-default diagnostic")


def test_explicit_diagnostic_disable_is_preserved():
    CORE.unique_ids = {}
    monitor = gensam.MONITOR_SCHEMA({
        gensam.CONF_NAME: "Explicit",
        gensam.CONF_PROCESSOR_LOAD: {
            gensam.CONF_NAME: "Custom Processor Load",
            gensam.CONF_DISABLED_BY_DEFAULT: True,
        },
    })
    check(monitor[gensam.CONF_PROCESSOR_LOAD][gensam.CONF_DISABLED_BY_DEFAULT],
          "an explicitly disabled diagnostic keeps its configured setting")


def test_discovery_slots_create_fixed_per_monitor_entities():
    for count in (0, 1, 2, 16, 32):
        config = validate_hub({
            gensam.CONF_DISCOVERY_SLOTS: count,
            gensam.CONF_SAM_UPLOAD: {},
        })
        slots = config.get(gensam.CONF_MONITORS, [])
        check(len(slots) == count,
              f"{count} discovery slots create exactly {count} monitor entity sets")
        check([slot[gensam.CONF_NAME] for slot in slots]
              == [f"Monitor {index}" for index in range(1, count + 1)],
              "discovery slots have stable, numbered entity names")
        check(all(slot[gensam.CONF_DISCOVERY_SLOT] for slot in slots),
              "generated monitor bindings are marked for runtime discovery assignment")
        check(all(slot[gensam.CONF_UNIQUE_ID] == 0 for slot in slots),
              "discovery slots do not match every monitor through a zero hardware ID")
        check(all(gensam.CONF_CALIBRATION_BYPASS in slot for slot in slots),
              "discovery slots retain the per-monitor group calibration controls")
        check(all(slot[gensam.CONF_SAM_NAME_SENSOR]["name"].endswith("SAM Name")
                  for slot in slots),
              "discovery slots expose a diagnostic SAM Name entity")
        check(all(str(slot[gensam.CONF_MUTE]["entity_category"]) == "config" for slot in slots),
              "per-monitor mute is a configuration entity like the other monitor controls")

    try:
        validate_hub({
            gensam.CONF_DISCOVERY_SLOTS: 1,
            gensam.CONF_MONITORS: [{gensam.CONF_NAME: "Static"}],
        })
        check(False, "static monitor bindings cannot conflict with discovery slots")
    except cv.Invalid as err:
        check("cannot be combined" in str(err),
              "static monitor bindings cannot conflict with discovery slots")

    config = validate_hub({gensam.CONF_DISCOVERY_SLOTS: 1})
    check(gensam.CONF_SAM_NAME_SENSOR not in config[gensam.CONF_MONITORS][0],
          "configs without runtime uploads do not add an unused SAM Name entity")


def test_discovery_slots_generate_home_assistant_devices():
    config = validate_hub({gensam.CONF_DISCOVERY_SLOTS: 2})
    full_config = {"esphome": {"devices": []}}
    token = fv.full_config.set(full_config)
    try:
        gensam._final_validate_discovery_devices(config)
    finally:
        fv.full_config.reset(token)

    devices = full_config["esphome"]["devices"]
    check([str(device[gensam.CONF_ID]) for device in devices]
          == ["gensam_monitor_1_device", "gensam_monitor_2_device"],
          "each discovery slot gets a declared ESPHome device")
    check([device[gensam.CONF_NAME] for device in devices] == ["Monitor 1", "Monitor 2"],
          "generated ESPHome devices use the matching slot name")
    check(all(slot[gensam.CONF_DEVICE_ID] == device[gensam.CONF_ID]
              for slot, device in zip(config[gensam.CONF_MONITORS], devices)),
          "each monitor slot's entities reference its generated device")


def test_discovery_slot_organization():
    settings = [
        {
            "slot": 1,
            "device_id": cv.sub_device_id("speaker_one"),
            "web_server": {
                "sorting_group_id": cv.use_id(gensam.cg.int_)("speaker_one_diagnostics"),
                "web_server_id": cv.use_id(gensam.web_server.WebServer)(None),
                "sorting_weight": 20,
            },
        }
    ]
    config = validate_hub({
        gensam.CONF_DISCOVERY_SLOTS: 2,
        gensam.CONF_SAM_UPLOAD: {},
        "discovery_slot_settings": settings,
    })
    first, second = config[gensam.CONF_MONITORS]
    full_config = {"esphome": {"devices": []}}
    token = fv.full_config.set(full_config)
    try:
        gensam._final_validate_discovery_devices(config)
    finally:
        fv.full_config.reset(token)
    check([device[gensam.CONF_NAME] for device in full_config["esphome"]["devices"]]
          == ["Monitor 2"],
          "an explicit slot device overrides generation only for that slot")

    names = {
        "temperature": "Temperature",
        "input_level": "Input Level",
        "output_level": "Output Level",
        "online": "Online",
        "identify": "Identify",
        "model": "Model",
        "serial_number_sensor": "Serial Number",
        "firmware_version": "Firmware Version",
        "hardware_id": "Hardware ID",
        "dip_switch_status": "DIP Switch Status",
        "mute": "Mute",
        "input": "Input",
        "level_db": "Level",
        "delay_ms": "Delay",
        "calibration_bypass": "Bypass Calibration",
        "bass_management_crossover_frequency": "Bass Management Crossover",
    }
    for key, name in names.items():
        check(first[key]["name"] == name, f"sub-device {key} has a non-redundant name")
        check(second[key]["name"] == name, f"automatically grouped {key} has a concise name")
        check(str(second[key]["device_id"]) == "gensam_monitor_2_device",
              f"{key} belongs to its automatically generated Home Assistant device")
    for key in ("temperature", "input_level", "output_level", "online", "identify",
                "model", "serial_number_sensor", "firmware_version", "hardware_id",
                "dip_switch_status"):
        check(str(first[key]["device_id"]) == "speaker_one",
              f"{key} belongs to the slot's Home Assistant device")
        check(str(first[key]["web_server"]["sorting_group_id"]) == "speaker_one_diagnostics",
              f"{key} belongs to the slot's web diagnostics group")
        check("web_server" not in second[key], f"{key} keeps the default grouping on other slots")
    ordered_diagnostics = ("online", "input_level", "output_level", "temperature", "model",
                           "serial_number_sensor", "firmware_version", "hardware_id",
                           "identify", "dip_switch_status", gensam.CONF_SAM_NAME_SENSOR)
    weights = [first[key]["web_server"]["sorting_weight"] for key in ordered_diagnostics]
    check(weights == list(range(20, 130, 10)),
          "diagnostics are ordered by importance from the configured starting weight")
    default_config = validate_hub({
        gensam.CONF_DISCOVERY_SLOTS: 1,
        "discovery_slot_settings": [{
            "slot": 1,
            "web_server": {"sorting_group_id": cv.use_id(gensam.cg.int_)("diagnostics")},
        }],
    })
    default_slot = default_config[gensam.CONF_MONITORS][0]
    default_diagnostics = ordered_diagnostics[:-1]
    check([default_slot[key]["web_server"]["sorting_weight"] for key in default_diagnostics]
          == list(range(50, 150, 10)), "diagnostics use the default starting weight of 50")
    check("calibration_bypass" not in default_slot,
          "the monitor card does not create bypass without groups or upload")
    server_ids = [first[key]["web_server"]["web_server_id"] for key in gensam.MONITOR_DIAGNOSTICS]
    check(len({id(server_id) for server_id in server_ids}) == len(server_ids),
          "each diagnostic has its own auto-resolved web server reference")
    for key in ("mute", "input", "level_db", "delay_ms", "calibration_bypass",
                "bass_management_crossover_frequency"):
        check(str(first[key]["device_id"]) == "speaker_one",
              f"{key} belongs to the slot's Home Assistant device")
        check(str(first[key].get("web_server", {}).get("sorting_group_id")) == "speaker_one_diagnostics",
              f"{key} belongs to the slot's web card")
        check("web_server" not in second[key], f"{key} keeps default grouping on other slots")
    check(all(first[key]["disabled_by_default"] for key in
              ("calibration_bypass", "bass_management_crossover_frequency", "level_db", "delay_ms")),
          "grouping preserves the disabled-by-default monitor settings")
    controls = ("mute", "input", "calibration_bypass", "bass_management_crossover_frequency",
                "level_db", "delay_ms")
    check([first[key].get("web_server", {}).get("sorting_weight") for key in controls]
          == list(range(-40, 20, 10)), "monitor controls appear in order before diagnostics")
    check(len({id(first[key]["web_server"]["web_server_id"])
               for key in controls + ordered_diagnostics if "web_server" in first[key]})
          == sum("web_server" in first[key] for key in controls + ordered_diagnostics),
          "all monitor entities have independent web server references")

    for invalid in (
        [{"slot": 1}, {"slot": 1}],
        [{"slot": 3}],
    ):
        try:
            validate_hub({gensam.CONF_DISCOVERY_SLOTS: 2, "discovery_slot_settings": invalid})
            check(False, "duplicate and out-of-range slot settings are rejected")
        except cv.Invalid:
            check(True, "duplicate and out-of-range slot settings are rejected")


def test_each_monitor_gets_its_own_web_card():
    config = validate_hub({
        gensam.CONF_DISCOVERY_SLOTS: 3,
        gensam.CONF_SAM_UPLOAD: {},
        "discovery_slot_settings": [{
            "slot": 2,
            "web_server": {"sorting_group_id": cv.use_id(gensam.cg.int_)("custom")},
        }],
    })
    cards = gensam._monitor_web_cards(config)
    check([(name, weight) for _, name, weight, _ in cards] == [("Monitor 1", 1), ("Monitor 3", 3)],
          "every monitor without explicit web settings gets a card named after it, in slot order")
    first = config[gensam.CONF_MONITORS][0]
    expected = [first[key][gensam.CONF_ID] for key in
                gensam.MONITOR_CONTROLS + gensam.MONITOR_DIAGNOSTICS if key in first]
    _, _, _, entities = cards[0]
    check([entity_id for entity_id, _ in entities] == expected,
          "the card holds all configuration and diagnostics, controls first")
    check(gensam.CONF_SAM_NAME_SENSOR in first and gensam.CONF_IDENTIFY in first
          and len(entities) == len(expected),
          "diagnostics such as SAM name and identify are in the card")
    weights = [weight for _, weight in entities]
    check(weights == sorted(weights) and len(set(weights)) == len(weights),
          "card entries have distinct ascending weights")


def test_shared_package_exposes_discovery_slot_substitution():
    import yaml

    root = Path(__file__).resolve().parents[1]
    package = yaml.safe_load((root / "packages/gensam.yaml").read_text())
    check(package["gensam"]["discovery_slots"] == "${discovery_slots}",
          "the shared package uses the configurable discovery slot count")
    check(package["substitutions"]["discovery_slots"] == "16",
          "existing configurations retain the default of 16 discovery slots")


def test_an_explicit_calibration_bypass_switch_is_kept():
    explicit = gensam.MONITOR_CALIBRATION_BYPASS_SCHEMA({gensam.CONF_NAME: "Left EQ Off"})
    config = bypass_config(monitor={gensam.CONF_NAME: "Left", gensam.CONF_UNIQUE_ID: 1842915,
                                    gensam.CONF_CALIBRATION_BYPASS: explicit})
    check(config[gensam.CONF_MONITORS][0][gensam.CONF_CALIBRATION_BYPASS][gensam.CONF_NAME]
          == "Left EQ Off", "a declared per-monitor switch is not replaced")

def test_system_led_switch_is_configurable():
    """The monitor LED control is shared; it does not depend on monitor bindings."""
    config = validate_hub({
        gensam.CONF_SYSTEM_LEDS: {gensam.CONF_NAME: "Monitor LEDs"},
    })
    check(config[gensam.CONF_SYSTEM_LEDS][gensam.CONF_NAME] == "Monitor LEDs",
          "the shared system LED switch is accepted")


# --- paths outside the configuration directory --------------------------------------------

def test_the_path_may_point_outside_the_config_directory():
    """GLM keeps its setups in Documents/Genelec/GLM5/Setup Files, not beside the YAML.

    cv.file_ resolves through CORE.relative_config_path, which expands a leading `~` and
    passes an absolute path through untouched. Nothing in this component does path handling
    of its own, so this is here to catch that changing under us.
    """
    relative, _ = expand()
    with tempfile.TemporaryDirectory() as home:
        outside = write_sam(os.path.join(home))
        absolute, _ = expand(path=outside)
        check([g[gensam.CONF_NAME] for g in absolute[gensam.CONF_GROUPS]]
              == [g[gensam.CONF_NAME] for g in relative[gensam.CONF_GROUPS]],
              "an absolute path outside the config directory imports the same groups")

        previous = os.environ.get("HOME")
        os.environ["HOME"] = home
        try:
            tilde, _ = expand(path="~/fixture.sam")
        finally:
            if previous is None:
                del os.environ["HOME"]
            else:
                os.environ["HOME"] = previous
        check(tilde[gensam.CONF_GROUPS] == absolute[gensam.CONF_GROUPS],
              "a ~/ path resolves to the same file, expanded not taken literally")


if __name__ == "__main__":
    sys.exit(run("gensam sam_file:", globals()))

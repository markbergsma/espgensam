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

from sam_fixture import GHOST_ID, check, make_sam, run, write_sam  # noqa: E402

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402

import gensam  # noqa: E402


MONITORS = [{"unique_id": 1842915}, {"unique_id": 1654321}]


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

def test_imported_groups_come_first_and_default_group_follows():
    config, _ = expand(groups=[hand_written()])
    names = [g[gensam.CONF_NAME] for g in config[gensam.CONF_GROUPS]]
    check(names == ["Listening Position A", "Listening Position B", "Mute Everything"],
          "the file's groups keep their order and precede the hand-written one")
    # default_group is resolved by _validate_groups from the first entry, so the order above
    # is also what the hub applies at startup and what the select lists.
    config = gensam._validate_groups(config)
    check(config[gensam.CONF_DEFAULT_GROUP] == "Listening Position A",
          "default_group falls to the setup file's first group")


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

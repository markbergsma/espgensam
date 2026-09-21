"""Synthetic GLM 5 setup file and the tiny harness the .sam tests share.

Not a test itself. It exists because three test files need the same fixture:
test_sam_import.py (the mapping), test_sam2yaml.py (the YAML emitter) and test_sam_config.py
(the ESPHome component's `sam_file:` expansion).

The fixture is synthetic rather than a real export: captures/ is not tracked, so a committed
test cannot read anyone's GLM configuration. Its shape and its numbers are taken from a real
GLM 5.2 file and from a bus capture of GLM applying it, so the assertions are still about
observed behaviour -- the subwoofer's three per-group levels and AutoPhase delays, the two
device classes' differing slot layouts, and their differing design rates.
"""

import contextlib
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "components", "gensam"))

import sam_import  # noqa: E402


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


#: The two real devices, as a `monitors:` block would name them.
KNOWN = {1842915, 1654321}

#: The third inventory entry, which GLM retained but which was never really present.
GHOST_ID = 2468013


def write_sam(directory, text=None, name="fixture.sam"):
    """Write the fixture into `directory` and return its path."""
    path = os.path.join(directory, name)
    with open(path, "w", encoding="latin-1") as fh:
        fh.write(text if text is not None else make_sam())
    return path


@contextlib.contextmanager
def collect_warnings():
    """Collect what the conversion warns about into a list, instead of logging it.

    Replaces sam_import.warn rather than reading the log, so a test says what it means, does
    not depend on logging configuration, and does not spill warnings into the test output.
    """
    warnings = []
    original = sam_import.warn
    sam_import.warn = warnings.append
    try:
        yield warnings
    finally:
        sam_import.warn = original


def convert(text=None, known=KNOWN):
    """Write the fixture to a temp file, convert it, and capture what it warned about."""
    with tempfile.NamedTemporaryFile("w", suffix=".sam", delete=False, encoding="latin-1") as fh:
        fh.write(text if text is not None else make_sam())
        path = fh.name
    try:
        with collect_warnings() as warnings:
            groups = sam_import.convert(sam_import.parse_sam(path), known)
        return groups, "\n".join(warnings)
    finally:
        os.unlink(path)


# --- harness ---------------------------------------------------------------------------
#
# No framework, matching the C++ host tests alongside these. A failing check is reported and
# counted rather than raised, so one broken case never hides the rest.

_failures = 0


def check(ok, what):
    global _failures
    print(f"  {'[ok]  ' if ok else '[FAIL]'} {what}")
    if not ok:
        _failures += 1


def run(title, namespace):
    """Run every test_* in `namespace` and return a process exit code."""
    global _failures
    print(title)
    for name, fn in sorted(namespace.items()):
        if not (name.startswith("test_") and callable(fn)):
            continue
        try:
            fn()
        except Exception as err:  # noqa: BLE001 - a raising test is a failing test
            # Reported rather than propagated so a regression that trips a refusal path still
            # reads as a failure instead of stopping the run.
            print(f"  [FAIL] {name} raised {type(err).__name__}: {err}")
            _failures += 1
    if _failures:
        print(f"\n{_failures} check(s) FAILED")
        return 1
    print("\nall checks passed")
    return 0

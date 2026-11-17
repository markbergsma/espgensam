#!/usr/bin/env python3
"""Compare all imported device fields and filter slots with the build-time .sam importer.

Run with: python3 tests/test_sam_runtime.py
"""

import math
from pathlib import Path
import subprocess
import tempfile

from sam_fixture import collect_warnings, full_band_sam, make_sam, sam_import, write_sam


ROOT = Path(__file__).resolve().parents[1]


def check_fixture(binary, directory, text, encoding="latin-1"):
    path = write_sam(directory, text)
    if encoding != "latin-1":
        Path(path).write_text(text, encoding=encoding)
    with collect_warnings():
        expected = sam_import.convert(sam_import.parse_sam(path))
    lines = iter(subprocess.check_output([str(binary), path, "--dump"], text=True).splitlines())
    source = {"analog": (1, 1), "aes3_a": (2, 1), "aes3_b": (2, 2), "aes3_sum": (2, 3)}
    lfe = {"none": 0, "aes3_a": 1, "aes3_b": 2, "aes3_sum": 3}
    kinds = {"bypass": 0, "notch": 3, "low_shelf": 1, "high_shelf": 2}
    for group in expected:
        assert next(lines) == "group\t" + group["name"]
        for device in group["devices"]:
            values = list(map(float, next(lines).split("\t")))
            src, channel = source[device["source"]]
            row = [
                device["unique_id"], device["enabled"], device["crossover"] or 85,
                src, channel, lfe[device["lfe_channel"]], device["lfe_level_db"],
                device["level_db"], device["delay_samples"],
            ]
            for band in device["filters"]:
                band = band or {"type": "bypass", "frequency": 0, "gain": 0, "q": 0}
                row.extend([kinds[band["type"]], band["frequency"], band["gain"],
                            band.get("q") or 0])
            assert len(values) == len(row)
            for actual, wanted in zip(values, row):
                assert math.isclose(actual, wanted, rel_tol=1e-6, abs_tol=1e-6), (actual, wanted)
    assert next(lines, None) is None


def main():
    with tempfile.TemporaryDirectory(prefix="espgensam-runtime-") as directory:
        binary = Path(directory) / "test_sam_runtime"
        subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Wextra", "-fsanitize=address,undefined",
             "-g", "-o", str(binary), str(ROOT / "tests/test_sam_runtime.cpp"),
             str(ROOT / "components/gensam/sam_runtime.cpp")], check=True,
        )
        for fixture in [
            make_sam(),
            full_band_sam(),
            make_sam().replace("Group_Crossover:90", "Group_Crossover:70"),
            make_sam().replace("LFE_Channel:0", "LFE_Channel:2").replace("LFE_Level:0", "LFE_Level:-4"),
            make_sam().replace("Group_Sensitivity:0", "Group_Sensitivity:-1"),
        ]:
            check_fixture(binary, directory, fixture)
        accented = make_sam().replace("Listening Position A", "Lähikenttä")
        for encoding in ("latin-1", "utf-8"):
            check_fixture(binary, directory, accented, encoding)
    print("Runtime SAM importer matches Python mapping; all checks passed")


if __name__ == "__main__":
    main()

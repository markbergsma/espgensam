#!/usr/bin/env python3
"""Verify the shared package's web control order and absence of the removed Dim control."""

from pathlib import Path
import subprocess
import sys

import yaml

ROOT = Path(__file__).resolve().parents[1]


def main():
    if "--baseline" in sys.argv:
        source = subprocess.check_output(
            ["git", "show", "HEAD:packages/gensam.yaml"], cwd=ROOT, text=True
        )
    else:
        source = (ROOT / "packages/gensam.yaml").read_text()
    package = yaml.safe_load(source)
    config = package["gensam"]
    keys = ("volume_db", "system_mute", "group_select", "calibration_bypass",
            "rediscover_button")
    weights = [config[key]["web_server"]["sorting_weight"] for key in keys]
    assert weights == [10, 20, 30, 40, 50], weights
    assert all(config[key]["web_server"]["sorting_group_id"] == "system_controls"
               for key in keys if key != "group_select")
    assert config["group_select"]["web_server"]["sorting_group_id"] == "group_controls"
    assert any(group["id"] == "group_controls" for group in package["web_server"]["sorting_groups"])
    group = next(group for group in package["web_server"]["sorting_groups"]
                 if group["id"] == "system_controls")
    assert group["sorting_weight"] < -1
    assert "dim" not in config
    print("Web controls ordered volume, mute, group, bypass, rediscover; Dim absent")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Capture one controlled RS-485 measurement run from a live device.

Comparing transceiver hardware means comparing rates between runs, and the counters in the
`Stats:` line are cumulative since boot. This script waits for the bus to settle, marks the
counters, accumulates a fixed number of frames, aborts loudly if the device reboots or yields
to a GLM adapter mid-run, and prints one labelled row ready to paste into the comparison
document.

USAGE

    python3 tools/capture_run.py --label "jumper-out, adapter absent" \\
        --yaml espgensam-waveshare-local.yaml --device espgensam-waveshare.local

Change exactly one thing about the setup between runs, and give each run a label saying what it
was. Options: --frames (default 4000, the sample size at which a 30% change is resolvable against
the reference baseline), --settle (Stats lines to discard after discovery completes, default 2).

The raw log is written to captures/, which is gitignored.
"""

import argparse
import datetime
import pathlib
import re
import subprocess
import sys

# Each counter is pulled out on its own rather than with one big line regex, so that adding a
# field to the Stats line does not silently break every capture.
FIELDS = {
    "chars": r"Stats: (\d+) chars",
    "bursts": r"chars \([^)]*\), (\d+) bursts",
    "start_rej": r"rx: (\d+) start rej",
    "stop2": r"start rej, (\d+) stop2",
    "framing": r"stop2, (\d+) framing errs",
    "echo_cancelled": r"tx: (\d+) echo cancelled",
    "slow_rel": r"echo cancelled, (\d+) slow rel",
    "slow_rel_max": r"slow rel \(max (\d+) us\)",
    "frames": r"frames: (\d+) ok",
    "invalid": r"ok, (\d+) invalid",
    "crc": r"invalid, (\d+) crc errs",
    "c0": r"crc errs, (\d+) C0 alias",
}

ERROR_FIELDS = ["stop2", "framing", "crc", "c0", "invalid"]


def parse_stats(line):
    """Extract the counter set from a `Stats:` log line, or None if it is not one."""
    if "Stats:" not in line:
        return None
    out = {}
    for name, pattern in FIELDS.items():
        m = re.search(pattern, line)
        if m is None:
            return None  # Unrecognised format; safer to skip than to guess.
        out[name] = int(m.group(1))
    out["yielding"] = "GLM ACTIVE" in line
    m = re.search(r"Monitors: (\d+)", line)
    out["monitors"] = int(m.group(1)) if m else 0
    return out


def rate(num, den, digits=4):
    return f"{num / den * 100:.{digits}f}%" if den else "n/a"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, help="what physical configuration this run is")
    ap.add_argument("--yaml", required=True, help="device YAML, for the API key")
    ap.add_argument("--device", required=True, help="mDNS name or serial port")
    ap.add_argument("--frames", type=int, default=4000, help="frames to accumulate (default 4000)")
    ap.add_argument("--settle", type=int, default=2,
                    help="Stats lines to discard after discovery (default 2)")
    ap.add_argument("--passive", action="store_true",
                    help="listen-only run: another master drives the bus and we yield to it, "
                         "so do not treat yielding as a fault. Measures the receive path "
                         "alone, with nothing of ours on the wire")
    ap.add_argument("--expect-monitors", type=int, default=None,
                    help="abort unless exactly this many monitors are registered; a phantom "
                         "from a corrupted discovery reply is polled forever and never "
                         "answers, which skews every rate against a clean run")
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    slug = re.sub(r"[^a-z0-9]+", "-", args.label.lower()).strip("-")
    captures = pathlib.Path("captures")
    captures.mkdir(exist_ok=True)
    logpath = captures / f"run-{stamp}-{slug}.log"

    print(f"[capture] label   : {args.label}")
    print(f"[capture] device  : {args.device}")
    print(f"[capture] target  : {args.frames} frames")
    print(f"[capture] raw log : {logpath}")
    print("[capture] waiting for discovery to complete, then marking...", flush=True)

    proc = subprocess.Popen(
        ["esphome", "logs", args.yaml, "--device", args.device],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
    )

    base = None
    seen_since_discovery = 0
    last = None
    status = "completed"

    try:
        with open(logpath, "w") as raw:
            for line in proc.stdout:
                raw.write(line)
                raw.flush()
                s = parse_stats(line)
                if s is None:
                    continue

                if s["yielding"] and not args.passive:
                    print("[capture] ABORT: yielded to an external GLM master. "
                          "Take the adapter off the bus and rerun, or pass --passive.")
                    status = "aborted-yielding"
                    break
                if args.passive and not s["yielding"] and base is not None:
                    print("\n[capture] ABORT: stopped yielding mid-run, so the bus went quiet "
                          "and we are no longer measuring another master's traffic.")
                    status = "aborted-not-yielding"
                    break

                if base is None:
                    # Discovery clusters its errors in the first seconds and then stops; excluding
                    # it is the whole point of marking rather than reading cumulative totals.
                    if s["monitors"] == 0:
                        continue
                    seen_since_discovery += 1
                    if seen_since_discovery <= args.settle:
                        continue
                    if args.expect_monitors is not None and s["monitors"] != args.expect_monitors:
                        print(f"[capture] ABORT: {s['monitors']} monitors registered, expected "
                              f"{args.expect_monitors}. Rediscover and rerun.")
                        status = "aborted-monitor-count"
                        break
                    base = s
                    print(f"[capture] marked at {s['chars']} chars / {s['frames']} frames "
                          f"({s['monitors']} monitors). Accumulating...", flush=True)
                    continue

                if s["frames"] < base["frames"] or s["chars"] < base["chars"]:
                    print("[capture] ABORT: counters went backwards - the device rebooted "
                          "mid-run. Sample discarded.")
                    status = "aborted-reboot"
                    break

                if s["monitors"] != base["monitors"]:
                    print(f"\n[capture] ABORT: monitor count changed {base['monitors']} -> "
                          f"{s['monitors']} mid-run. Sample discarded.")
                    status = "aborted-monitor-change"
                    break

                last = s
                done = s["frames"] - base["frames"]
                if done >= args.frames:
                    break
                print(f"\r[capture] {done}/{args.frames} frames", end="", flush=True)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print()
    if status != "completed" or base is None or last is None:
        print(f"[capture] run not usable ({status}).")
        return 1

    d = {k: last[k] - base[k] for k in FIELDS}
    clean = all(d[f] == 0 for f in ERROR_FIELDS)

    print()
    print(f"=== {args.label} ===")
    print(f"chars {d['chars']}, frames {d['frames']}, bursts {d['bursts']}")
    print(f"  start rej   {d['start_rej']:6d}   {rate(d['start_rej'], d['chars'])} of chars")
    print(f"  stop2       {d['stop2']:6d}   {rate(d['stop2'], d['chars'])} of chars")
    print(f"  framing     {d['framing']:6d}   {rate(d['framing'], d['chars'])} of chars")
    print(f"  crc errs    {d['crc']:6d}   {rate(d['crc'], d['frames'])} of frames")
    print(f"  C0 alias    {d['c0']:6d}   {rate(d['c0'], d['frames'])} of frames")
    print(f"  invalid     {d['invalid']:6d}   {rate(d['invalid'], d['frames'])} of frames")
    print(f"  echo cancel {d['echo_cancelled']:6d}")
    print(f"  slow rel    {d['slow_rel']:6d}   max {last['slow_rel_max']} us")
    print(f"  {'CLEAN - no errors of any kind' if clean else 'errors present'}")
    print()
    print("markdown row:")
    print(f"| {args.label} | {d['chars']} | {d['frames']} | {d['framing']} | {d['stop2']} | "
          f"{d['crc']} | {d['c0']} | {d['start_rej']} |")
    print()
    print(f"raw log: {logpath}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

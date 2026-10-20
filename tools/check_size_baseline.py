#!/usr/bin/env python3
"""tools/check_size_baseline.py -- fail when the engine footprint grows.

Reads the committed size_baseline.txt, compares it with the measurement
printed by tools/size_measure.sh and fails when .text or .bss grew by
more than the allowed percentage (5 % by default).

    python3 tools/check_size_baseline.py [baseline-file]

The baseline is produced by `sh tools/size_measure.sh`; the numbers
depend on the compiler version, so the tolerance exists to absorb
toolchain noise, not real growth.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
TOLERANCE = 0.05


def read_baseline(path):
    values = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip().split()[0] if val.strip() else ""
            try:
                values[key] = int(val)
            except ValueError:
                continue
    return values


def measure():
    script = os.path.join(HERE, "size_measure.sh")
    out = subprocess.run(["sh", script], cwd=ROOT, check=True,
                         stdout=subprocess.PIPE).stdout.decode()
    values = {}
    for token in out.split():
        if "=" in token:
            key, _, val = token.partition("=")
            try:
                values[key] = int(val)
            except ValueError:
                pass
    return values


def main():
    baseline_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "size_baseline.txt")
    if not os.path.isfile(baseline_path):
        print("FAIL: baseline file %s is missing" % baseline_path)
        return 1
    baseline = read_baseline(baseline_path)
    current = measure()
    if not current:
        print("FAIL: no measurement produced")
        return 1

    status = 0
    print("size baseline check (%s)" % os.path.basename(baseline_path))
    for key in ("text", "bss", "data", "total"):
        if key not in baseline or key not in current:
            continue
        base = baseline[key]
        now = current[key]
        delta = now - base
        pct = (float(delta) / float(base)) * 100.0 if base else 0.0
        flag = "ok"
        if pct > TOLERANCE * 100.0:
            flag = "TOO BIG"
            status = 1
        print("  %-6s baseline=%-8d current=%-8d delta=%+d (%+.2f%%) %s" %
              (key, base, now, delta, pct, flag))
    if status != 0:
        print("FAIL: the engine footprint grew by more than %.0f%%" %
              (TOLERANCE * 100.0))
    else:
        print("OK: within %.0f%% of the baseline" % (TOLERANCE * 100.0))
    return status


if __name__ == "__main__":
    sys.exit(main())

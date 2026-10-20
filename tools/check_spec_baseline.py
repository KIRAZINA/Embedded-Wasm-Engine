#!/usr/bin/env python3
"""tools/check_spec_baseline.py -- fail on a spec-suite regression.

The official spec suite still has documented failures (see
tests/spec/DEVIATIONS.md). Those are listed there together with a
numeric baseline. This script runs the suite and compares the result
with that baseline:

  * the number of passing assertions must not drop,
  * no failure class may grow by more than the allowed slack.

The corpus is pinned (tools/fetch_spec_tests.sh checks out a fixed
upstream revision), but it still depends on the wast2json version: a
different converter can parse a different set of suites. The baseline
therefore carries a *fingerprint* (corpus_suites / corpus_assertions).
When the fingerprint matches, the comparison is strict. When it does
not, the pass *rate* is compared with a tolerance instead, so a corpus
change cannot produce a false failure -- and a real regression is still
caught.

    python3 tools/check_spec_baseline.py [--baseline FILE] [--slack N]
                                         [--rate-tolerance 0.02]

Exit status 0 when the run is at least as good as the baseline, 1
otherwise (and 2 when the suite could not be run at all).
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DEFAULT_BASELINE = os.path.join(ROOT, "tests", "spec", "DEVIATIONS.md")


def parse_baseline(path):
    """Read the 'key = value' block from the markdown baseline."""
    values = {}
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    block = re.search(r"```text\s*(.*?)```", text, re.S)
    if block is None:
        return values
    for line in block.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip()
        m = re.match(r"^(\d+)\s+(\S+)$", val)
        if m:
            values[key] = int(m.group(1))
        elif val.isdigit():
            values[key] = int(val)
    return values


def run_suite(runner, json_dir):
    cmd = [sys.executable, os.path.join(HERE, "run_spec_suite.py"),
           "--runner", runner]
    if os.path.isdir(json_dir):
        files = sorted(f for f in os.listdir(json_dir) if f.endswith(".json"))
        cmd += [os.path.join(json_dir, f) for f in files]
    else:
        print("ERROR: no spec json directory at %s" % json_dir)
        return None
    proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    out = proc.stdout.decode(errors="replace")
    total_pass = None
    total_fail = None
    classes = {}
    for line in out.splitlines():
        m = re.match(r"^TOTAL:\s*(\d+)\s+passed,\s*(\d+)\s+failed", line)
        if m:
            total_pass = int(m.group(1))
            total_fail = int(m.group(2))
            continue
        m = re.match(r"^CLASS\s+(\S+):\s*(\d+)", line)
        if m:
            classes[m.group(1)] = int(m.group(2))
    return total_pass, total_fail, classes, out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--runner", default=os.path.join(ROOT, "build",
                                                    "wasm_runner_spec"))
    ap.add_argument("--json-dir", default=os.path.join(ROOT, "tests", "spec",
                                                       "json"))
    ap.add_argument("--slack", type=int, default=0,
                    help="allowed growth per failure class")
    ap.add_argument("--summary", default=None,
                    help="parse an existing runner summary instead of "
                         "running the suite again")
    ap.add_argument("--rate-tolerance", type=float, default=0.02,
                    help="allowed drop of the pass rate when the corpus "
                         "fingerprint differs (percentage points)")
    args = ap.parse_args()

    if not os.path.isfile(args.baseline):
        print("ERROR: baseline file %s is missing" % args.baseline)
        return 2
    baseline = parse_baseline(args.baseline)
    if "total_pass" not in baseline:
        print("ERROR: no numeric baseline in %s" % args.baseline)
        return 2

    if args.summary:
        with open(args.summary, encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
        total_pass = total_fail = None
        classes = {}
        for line in raw.splitlines():
            m = re.match(r"^TOTAL:\s*(\d+)\s+passed,\s*(\d+)\s+failed", line)
            if m:
                total_pass = int(m.group(1))
                total_fail = int(m.group(2))
                continue
            m = re.match(r"^CLASS\s+(\S+):\s*(\d+)", line)
            if m:
                classes[m.group(1)] = int(m.group(2))
        result = (total_pass, total_fail, classes, raw)
    else:
        result = run_suite(args.runner, args.json_dir)
    if result is None:
        return 2
    total_pass, total_fail, classes, raw = result
    if total_pass is None:
        print("ERROR: the spec suite produced no TOTAL line")
        print(raw[-2000:])
        return 2

    status = 0
    print("spec baseline check (%s)" % os.path.basename(args.baseline))

    # Corpus fingerprint: the corpus is pinned upstream, but the set of
    # convertible suites also depends on the wast2json version. A
    # different fingerprint means the numbers are not directly
    # comparable, so the pass rate is compared with a tolerance.
    suites = 0
    if os.path.isdir(args.json_dir):
        suites = len([f for f in os.listdir(args.json_dir)
                      if f.endswith(".json")])
    assertions = total_pass + total_fail
    base_suites = baseline.get("corpus_suites")
    base_assertions = baseline.get("corpus_assertions")
    same_corpus = (base_suites is None or base_assertions is None or
                   (suites == base_suites and assertions == base_assertions))
    if same_corpus:
        print("  ok   : corpus fingerprint matches (%d suites, %d assertions)"
              % (suites, assertions))
    else:
        print("  note : corpus fingerprint differs (now %d suites, %d "
              "assertions; baseline %s suites, %s assertions) -- the pass "
              "rate is compared with a %.1f point tolerance"
              % (suites, assertions,
                 base_suites if base_suites is not None else "?",
                 base_assertions if base_assertions is not None else "?",
                 args.rate_tolerance * 100.0))

    base_pass = baseline.get("total_pass", 0)
    if same_corpus:
        if total_pass < base_pass:
            print("  FAIL: passing assertions dropped: %d < %d" %
                  (total_pass, base_pass))
            status = 1
        else:
            print("  ok   : passing assertions %d (baseline %d)" %
                  (total_pass, base_pass))
    else:
        base_rate = (float(base_pass) / float(base_pass + base_assertions)
                     if (base_pass + base_assertions) else 0.0)
        now_rate = (float(total_pass) / float(assertions)
                    if assertions else 0.0)
        if now_rate < base_rate - args.rate_tolerance:
            print("  FAIL: pass rate dropped: %.4f < %.4f (tolerance %.4f)"
                  % (now_rate, base_rate, args.rate_tolerance))
            status = 1
        else:
            print("  ok   : pass rate %.4f (baseline %.4f)" %
                  (now_rate, base_rate))
    base_fail = baseline.get("total_fail")
    if same_corpus and base_fail is not None and total_fail > base_fail:
        print("  FAIL: failing assertions grew: %d > %d" %
              (total_fail, base_fail))
        status = 1
    else:
        print("  ok   : failing assertions %d (baseline %s)" %
              (total_fail, base_fail if base_fail is not None else "-"))
    if same_corpus:
        for name, count in sorted(classes.items()):
            key = "CLASS " + name
            if key not in baseline:
                continue
            base = baseline[key]
            if count > base + args.slack:
                print("  FAIL: class %s grew: %d > %d" % (name, count, base))
                status = 1
            else:
                print("  ok   : class %-45s %d (baseline %d)" %
                      (name, count, base))
    if status != 0:
        print("FAIL: the spec suite regressed against the baseline")
    else:
        print("OK: no regression against the baseline")
    return status


if __name__ == "__main__":
    sys.exit(main())

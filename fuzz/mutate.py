#!/usr/bin/env python3
"""fuzz/mutate.py -- corpus-driven mutation driver for the WASM engine.

Builds a corpus of VALID modules (demo/*.wasm plus the official spec
corpus), mutates them (bit flips, truncation, section-size corruption,
random byte overwrite) and pushes every mutant through the zero-allocation
C pipeline (fuzz/parse_harness.c or fuzz/loop_driver.c).

The C side never allocates; this script only decides *what* to feed it
and triages the results.

Usage:
    python3 fuzz/mutate.py --out build/fuzz/corpus --rounds 50
    python3 fuzz/mutate.py --out build/fuzz/corpus --runner build/fuzz/parse_harness
    python3 fuzz/mutate.py --seed 1234 --rounds 10 --quiet

Exit status: 0 when no crash was found, 1 when at least one crash was
reproduced (the minimized input is written to <out>/crashes/).
"""

import argparse
import os
import random
import subprocess
import sys

WASM_HEADER = 8


def collect_seeds(dirs, limit):
    """Return [(path, bytes)] for every *.wasm file below `dirs`."""
    seeds = []
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            for name in sorted(files):
                if not name.endswith(".wasm"):
                    continue
                path = os.path.join(root, name)
                try:
                    with open(path, "rb") as fh:
                        data = fh.read()
                except OSError:
                    continue
                if len(data) >= WASM_HEADER:
                    seeds.append((path, data))
                if len(seeds) >= limit:
                    return seeds
    return seeds


def read_uleb(data, pos):
    """Return (value, next_pos) for an unsigned LEB128 at `pos`."""
    value = 0
    shift = 0
    while pos < len(data):
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return value, pos
        shift += 7
        if shift > 35:
            break
    return value, pos


def section_offsets(data):
    """Yield (size_field_offset, size_bytes_len) for every section."""
    pos = WASM_HEADER
    out = []
    while pos < len(data):
        pos += 1  # section id
        size_off = pos
        value, nxt = read_uleb(data, pos)
        out.append((size_off, nxt - size_off))
        pos = nxt + value
        if pos > len(data):
            break
    return out


def mutate_bitflip(data, rng):
    out = bytearray(data)
    for _ in range(rng.randint(1, 8)):
        pos = rng.randrange(len(out))
        out[pos] ^= 1 << rng.randrange(8)
    return bytes(out)


def mutate_truncate(data, rng):
    if len(data) <= WASM_HEADER + 1:
        return data
    return data[: rng.randrange(WASM_HEADER, len(data))]


def mutate_section_size(data, rng):
    """Rewrite one section's size LEB in place (same encoded length)."""
    out = bytearray(data)
    secs = section_offsets(out)
    if not secs:
        return mutate_random_byte(data, rng)
    off, length = secs[rng.randrange(len(secs))]
    mask = (1 << (7 * length)) - 1
    value = rng.randrange(0, mask + 1)
    for i in range(length):
        byte = value & 0x7F
        value >>= 7
        if i + 1 < length:
            byte |= 0x80
        out[off + i] = byte
    return bytes(out)


def mutate_random_byte(data, rng):
    out = bytearray(data)
    if not out:
        return bytes(out)
    out[rng.randrange(len(out))] = rng.randrange(256)
    return bytes(out)


def mutate_header(data, rng):
    out = bytearray(data)
    out[rng.randrange(4)] = rng.randrange(256)
    return bytes(out)


MUTATIONS = [
    mutate_bitflip,
    mutate_truncate,
    mutate_section_size,
    mutate_random_byte,
    mutate_header,
]


def run_runner(runner, path):
    """Run the harness on `path`; return (returncode, crashed)."""
    try:
        proc = subprocess.run(
            [runner, path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        return None, True
    except OSError as exc:
        print("cannot run harness: %s" % exc, file=sys.stderr)
        return None, False
    # Negative return code == killed by a signal (SIGSEGV/SIGABRT/...).
    crashed = proc.returncode is not None and proc.returncode < 0
    return proc.returncode, crashed


def minimize(data, runner):
    """Shrink a crashing input: truncation first, then byte zeroing."""
    best = data
    # 1. truncation (binary search over cut points)
    lo, hi = 1, len(best)
    while lo < hi:
        mid = (lo + hi) // 2
        cand = best[:mid]
        _rc, crashed = run_runner(runner, _write_temp(cand))
        if crashed:
            best = cand
            hi = mid
        else:
            lo = mid + 1
    # 2. byte zeroing
    i = 0
    while i < len(best):
        cand = best[:i] + b"\x00" + best[i + 1:]
        _rc, crashed = run_runner(runner, _write_temp(cand))
        if crashed:
            best = cand
        else:
            i += 1
    return best


_TMP = [None]


def _write_temp(data):
    if _TMP[0] is None:
        import tempfile

        fd, path = tempfile.mkstemp(prefix="wasm_mutant_", suffix=".wasm")
        os.close(fd)
        _TMP[0] = path
    with open(_TMP[0], "wb") as fh:
        fh.write(data)
    return _TMP[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seed", type=int, default=0, help="PRNG seed (0 = time)")
    ap.add_argument("--rounds", type=int, default=20,
                    help="mutants generated per seed module")
    ap.add_argument("--out", default="build/fuzz/corpus",
                    help="output directory for the generated corpus")
    ap.add_argument("--corpus", action="append", default=None,
                    help="directory to harvest seed modules from (repeatable)")
    ap.add_argument("--runner", default=None,
                    help="harness binary to run on every mutant")
    ap.add_argument("--max-seeds", type=int, default=200,
                    help="maximum number of seed modules to use")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    corpus_dirs = args.corpus or ["demo", "tests/spec/json"]
    rng = random.Random(args.seed if args.seed else None)
    seeds = collect_seeds(corpus_dirs, args.max_seeds)
    if not seeds:
        print("no seed modules found in %s" % ", ".join(corpus_dirs),
              file=sys.stderr)
        return 2

    os.makedirs(args.out, exist_ok=True)
    crash_dir = os.path.join(args.out, "crashes")
    if args.runner:
        os.makedirs(crash_dir, exist_ok=True)

    mutants = 0
    crashes = 0
    for path, data in seeds:
        for _ in range(args.rounds):
            fn = MUTATIONS[rng.randrange(len(MUTATIONS))]
            mutant = fn(data, rng)
            name = "%s.%04d.wasm" % (
                os.path.basename(path).replace(".wasm", ""), mutants)
            with open(os.path.join(args.out, name), "wb") as fh:
                fh.write(mutant)
            mutants += 1
            if args.runner:
                rc, crashed = run_runner(args.runner,
                                         os.path.join(args.out, name))
                if crashed:
                    crashes += 1
                    small = minimize(mutant, args.runner)
                    cpath = os.path.join(crash_dir, "crash_%03d.wasm" % crashes)
                    with open(cpath, "wb") as fh:
                        fh.write(small)
                    if not args.quiet:
                        print("CRASH %s -> %s (%d -> %d bytes)"
                              % (name, cpath, len(mutant), len(small)),
                              file=sys.stderr)

    if not args.quiet:
        print("seeds=%d mutants=%d crashes=%d out=%s"
              % (len(seeds), mutants, crashes, args.out))
    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())

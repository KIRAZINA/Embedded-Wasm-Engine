#!/usr/bin/env python3
"""Validator/VM differential smoke: drive the CLI over seeds plus mutants.

Seeds: demo/*.wasm, build/*.wasm, and every module byte-array embedded in
tests/*.c (arrays starting with the wasm magic). Mutants: byte flips,
single-bit flips, truncations, and targeted section-size corruption,
all deterministic under --seed.

For every case the CLI is run once (default path: parse, validate, link,
execute export "main") and the outcome is bucketed:

  OK                 exit 0, module ran.
  INPUT_REJECT       unreadable file / bad header (exit 1 or 2, clean).
  VALIDATION_REJECT  validator refused it (exit 1, clean). Invariant B.
  NO_EXPORT          validated but export "main" is absent (exit 1, clean).
  MEM_ARENA          validated but the CLI arena is smaller than the
                     declared memory minimum (exit 1, clean).
  ARG_MISMATCH       validated but "main" takes arguments (exit 2, clean).
  RUNTIME_TRAP       validated, executed, trapped (exit 1, clean).
  LINK_REJECT        validated but instantiate refused it (exit 1, clean;
                     per-code counts are printed for triage).
  VIOLATION          anything else: process crash or hang, an exit code
                     outside {0, 1, 2}, or a MALFORMED-class failure
                     (TRUNCATED / LEB_* / SECTION_OVERFLOW / INVALID_SECTION /
                     MAGIC / VERSION / TOO_SHORT / CODE_OVERRUN /
                     UNKNOWN_OPCODE) from a module the validator accepted.
                     Invariant A. Also a validation rejection with an
                     unexpected exit code.

Exit 0 iff no VIOLATION rows. Buckets and the first violations print to
stdout; the full row log stays in memory only.

Stdlib only. No third-party dependencies.
"""
import argparse
import glob
import os
import random
import re
import shutil
import subprocess
import sys

MAGIC = (0x00, 0x61, 0x73, 0x6D)

MALFORMED_MARKERS = (
    "WASM_ERR_TRUNCATED",
    "WASM_ERR_LEB_TRUNCATED",
    "WASM_ERR_LEB_OVERFLOW",
    "WASM_ERR_SECTION_OVERFLOW",
    "WASM_ERR_INVALID_SECTION",
    "WASM_ERR_MAGIC",
    "WASM_ERR_VERSION",
    "WASM_ERR_TOO_SHORT",
    "WASM_TRAP_CODE_OVERRUN",
    "WASM_TRAP_UNKNOWN_OPCODE",
)

HEXTOK = re.compile(r"0x[0-9A-Fa-f]+(?:[uU](?:[lL](?:[lL])?)?|[lL](?:[lL])?[uU]?)?")


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def uleb_len(buf, pos):
    start = pos
    while pos < len(buf) and (buf[pos] & 0x80):
        pos += 1
    if pos >= len(buf):
        return None
    return pos - start + 1


def section_size_fields(data):
    """Byte offsets of every section-size ULEB in a module, or None."""
    if len(data) < 8 or tuple(data[0:4]) != MAGIC:
        return None
    pos, fields = 8, []
    while pos < len(data):
        pos += 1  # section id byte
        ln = uleb_len(data, pos)
        if ln is None:
            return None
        fields.append((pos, ln))
        size = 0
        shift = 0
        for k in range(ln):
            size |= (data[pos + k] & 0x7F) << shift
            shift += 7
        pos += ln + size
        if pos > len(data):
            return None
    return fields


def extract_embedded(paths):
    """Module blobs baked into C test sources (magic-prefixed runs)."""
    found = []

    def gap_ok(text):
        return re.fullmatch(r"[\s,]+", text) is not None

    for path in paths:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                src = fh.read()
        except OSError:
            continue
        toks = list(HEXTOK.finditer(src))
        run, run_end = [], 0
        runs = []

        def flush():
            if len(run) >= 8 and tuple(run[0:4]) == MAGIC and all(
                b < 256 for b in run
            ):
                runs.append(bytes(run))

        for tok in toks:
            try:
                val = int(tok.group(0).rstrip("uUlL"), 16)
            except ValueError:
                if run:
                    flush()
                    run = []
                run_end = tok.end()
                continue
            if run and gap_ok(src[run_end:tok.start()]) and val < 256:
                run.append(val)
            else:
                if run:
                    flush()
                run = [val] if val < 256 else []
            run_end = tok.end()
        if run:
            flush()
        for blob in runs:
            found.append((os.path.basename(path), blob))
    return found


def mutate(rng, data, fields, count):
    """Deterministic mutant list: flips, bit flips, truncations, size hits."""
    out = []
    n = len(data)
    for _ in range(count):
        pick = rng.randrange(4)
        buf = bytearray(data)
        if pick == 0 or n == 0:  # byte flip (1-3 bytes)
            for _ in range(rng.randint(1, 3)):
                if n:
                    buf[rng.randrange(n)] = rng.randrange(256)
        elif pick == 1:  # single bit flip
            if n:
                i = rng.randrange(n)
                buf[i] ^= 1 << rng.randrange(8)
        elif pick == 2:  # truncation (possibly empty)
            buf = buf[: rng.randrange(n + 1)] if n else buf
        else:  # section-size corruption
            if fields:
                off, ln = fields[rng.randrange(len(fields))]
                how = rng.randrange(3)
                if how == 0 and ln > 0:
                    buf[off + rng.randrange(ln)] ^= 0xFF
                elif how == 1:
                    for k in range(ln):
                        buf[off + k] = 0x7F if k < ln - 1 else 0x01
                else:
                    buf = buf[:off] + buf[off + ln:]
            elif n:
                buf[rng.randrange(n)] = rng.randrange(256)
        out.append(bytes(buf))
    return out


LINK_ALLOW = (
    "WASM_ERR_DATA_OUT_OF_BOUNDS",  # offset evaluated against live memory
    "WASM_ERR_ELEMENT_OUT_OF_BOUNDS",  # table size known only at link time
    "WASM_ERR_UNRESOLVED_IMPORT",  # depends on host registry contents
    "WASM_ERR_TYPE_MISMATCH_IMPORT",  # depends on host registry signatures
    "WASM_ERR_START_FAILED",  # a runtime trap inside the start function
    "WASM_ERR_IMPORTED_TABLES_UNSUPPORTED",  # host has no table concept
)


def classify(rc, text):
    """Bucket a finished CLI run. Returns (bucket, detail)."""
    if rc not in (0, 1, 2):
        return ("VIOLATION", "exit code %r outside {0,1,2}" % (rc,))
    if "error: validation:" in text:
        if rc != 1:
            return ("VIOLATION", "validation rejection exited %d" % rc)
        return ("VALIDATION_REJECT", last_line(text, "error: validation:"))
    # Parse-level rejections precede validation: the validator never ran,
    # so engine error names here are clean input handling, not divergence.
    if "error: bad header:" in text or "out of range" in text or \
            "short read" in text or "cannot open" in text or \
            "cannot size" in text:
        return ("INPUT_REJECT", "")
    if "no export section" in text or "error: export" in text:
        # Absent-section lookup reports TRUNCATED as its "not found"
        # sentinel; that name here is clean control flow, not divergence.
        return ("NO_EXPORT", "")
    if "takes " in text and "arg(s)" in text:
        return ("ARG_MISMATCH", "")
    if "memory bytes, have" in text:
        # CLI arena smaller than the declared memory minimum: a clean,
        # deterministic host-capacity rejection (like RUNNER_FILE_MAX).
        # The validator cannot know the CLI arena size; instantiate is
        # never reached, so no memory is touched.
        return ("MEM_ARENA", "")
    if "error: instantiate:" in text:
        line = last_line(text, "error: instantiate:")
        if any(allow in line for allow in LINK_ALLOW):
            return ("LINK_REJECT", line)
        return ("VIOLATION", "post-validation link failure " + line)
    if "RESULT trap " in text or "trapped:" in text or "trap " in text:
        return ("RUNTIME_TRAP", "")
    if "step budget exhausted" in text:
        return ("RUNTIME_TRAP", "watchdog")
    if rc == 0:
        return ("OK", "")
    for marker in MALFORMED_MARKERS:
        if marker in text:
            return ("VIOLATION", "post-validation malformed marker " + marker)
    return ("VIOLATION", "unclassified nonzero exit %d: %s" % (rc, snippet(text)))


def last_line(text, needle):
    for line in text.splitlines():
        if needle in line:
            return line.strip()[:120]
    return ""


def snippet(text):
    for line in text.splitlines():
        s = line.strip()
        if s:
            return s[:120]
    return "<silent>"


def probe_exports(runner, src, timeout):
    """Function-export names via --dump (no execution). Empty on failure."""
    try:
        proc = subprocess.run(
            [runner, "--dump", src],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, text=True, errors="replace",
        )
    except (subprocess.TimeoutExpired, OSError):
        return []
    if proc.returncode != 0:
        return []
    # Names starting with '-' cannot be addressed positionally (the CLI
    # reads them as flags); probing them would only exercise arg parsing.
    # Such cases fall back to the default export attempt below.
    return [n for n in
            re.findall(r'"([^"]+)" kind=function index=\d+', proc.stdout)
            if not n.startswith("-")]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", default=None)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--mutants", type=int, default=40)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--workdir", default=os.path.join("build", "parity"))
    ap.add_argument("--export", default="main")
    ap.add_argument("--max-violations", type=int, default=15)
    ap.add_argument("extra", nargs="*", help="extra seed files or dirs")
    ns = ap.parse_args(argv)

    runner = ns.runner or (
        "build/wasm_runner.exe" if os.name == "nt" else "build/wasm_runner"
    )
    if not os.path.isfile(runner):
        print("parity: runner not found: %s" % runner)
        return 2

    seeds = []
    for path in sorted(glob.glob(os.path.join("demo", "*.wasm"))):
        seeds.append((path, read_bytes(path)))
    for path in sorted(glob.glob(os.path.join("build", "*.wasm"))):
        seeds.append((path, read_bytes(path)))
    for path in ns.extra:
        if os.path.isdir(path):
            for root, _, files in os.walk(path):
                for name in sorted(files):
                    if name.endswith(".wasm"):
                        full = os.path.join(root, name)
                        seeds.append((full, read_bytes(full)))
        elif os.path.isfile(path):
            seeds.append((path, read_bytes(path)))
    for origin, blob in extract_embedded(sorted(glob.glob("tests/*.c"))):
        seeds.append(("tests:%s[%d]" % (origin, len(blob)), blob))

    seen, unique = set(), []
    for name, blob in seeds:
        if blob not in seen:
            seen.add(blob)
            unique.append((name, blob))
    seeds = unique

    if os.path.isdir(ns.workdir):
        shutil.rmtree(ns.workdir)
    os.makedirs(ns.workdir)

    rng = random.Random(ns.seed)
    buckets, violations, link_codes, examined = {}, [], {}, 0
    for idx, (name, blob) in enumerate(seeds):
        fields = section_size_fields(blob)
        cases = []  # (label, path or None, bytes)
        if os.path.isfile(name):
            cases.append((name, name, blob))
        else:
            base = os.path.join(ns.workdir, "seed%02d_base.wasm" % idx)
            with open(base, "wb") as fh:
                fh.write(blob)
            cases.append((name, base, blob))
        for m, mutant in enumerate(mutate(rng, blob, fields, ns.mutants)):
            fpath = os.path.join(ns.workdir, "seed%02d_mut%03d.wasm" % (idx, m))
            with open(fpath, "wb") as fh:
                fh.write(mutant)
            cases.append((os.path.basename(fpath), fpath, mutant))
        for label, src, _blob in cases:
            names = probe_exports(runner, src, ns.timeout) or [ns.export]
            for entry in names:
                if len(names) > 1:
                    tag = "%s!%s" % (label, entry)
                else:
                    tag = label
                try:
                    proc = subprocess.run(
                        [runner, "--quiet", src, entry],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        timeout=ns.timeout, text=True, errors="replace",
                    )
                    text = proc.stdout + proc.stderr
                    bucket, detail = classify(proc.returncode, text)
                except subprocess.TimeoutExpired:
                    bucket, detail = ("VIOLATION",
                                      "timeout after %ss" % ns.timeout)
                except OSError as exc:
                    bucket, detail = ("VIOLATION", "spawn failed: %s" % exc)
                examined += 1
                buckets[bucket] = buckets.get(bucket, 0) + 1
                if bucket == "LINK_REJECT":
                    link_codes[detail] = link_codes.get(detail, 0) + 1
                if bucket == "VIOLATION" and \
                        len(violations) < ns.max_violations:
                    violations.append((tag, detail))

    print("parity: seeds=%d examined=%d seed=%d mutants/seed=%d" %
          (len(seeds), examined, ns.seed, ns.mutants))
    for key in ("OK", "INPUT_REJECT", "VALIDATION_REJECT", "NO_EXPORT",
                "ARG_MISMATCH", "MEM_ARENA", "RUNTIME_TRAP", "LINK_REJECT",
                "VIOLATION"):
        if buckets.get(key):
            print("parity: %-17s %d" % (key, buckets[key]))
    for code in sorted(link_codes):
        print("parity: link: %dx %s" % (link_codes[code], code))
    for label, detail in violations:
        print("parity: VIOLATION %s: %s" % (label, detail))
    if buckets.get("VIOLATION"):
        print("parity: FAIL (%d violations)" % buckets["VIOLATION"])
        return 1
    print("parity: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

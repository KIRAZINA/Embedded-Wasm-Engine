#!/usr/bin/env python3
"""Run wast2json output against the zero-allocation wasm CLI.

The suite is deliberately stdlib-only. Prepare files with, for example:
  wast2json --spec ./path/to/file.wast -o ./path/to/file.json
The JSON and referenced .wasm files are supplied by the user.
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys

CURATED = ["i32", "i64", "f32", "f64", "i32_cmp", "f32_cmp",
           "f64_cmp", "br", "br_if", "br_table", "block", "loop", "if",
           "call", "call_indirect", "global", "memory", "select",
           "unreachable", "conversions", "fac", "forward", "int_exprs"]


def bits_float(value, width):
    if isinstance(value, str) and value.lower().startswith("0x"):
        return int(value, 16)
    number = float(value)
    return (struct.unpack(">I", struct.pack(">f", number))[0] if width == 32
            else struct.unpack(">Q", struct.pack(">d", number))[0])


def nan_bits(bits, width, kind):
    if width == 32:
        exp, frac = (bits >> 23) & 0xff, bits & 0x7fffff
        canonical = bits == 0x7fc00000
    else:
        exp, frac = (bits >> 52) & 0x7ff, bits & 0xfffffffffffff
        canonical = bits == 0x7ff8000000000000
    if exp != (0xff if width == 32 else 0x7ff) or frac == 0:
        return False
    return kind == "nan:arithmetic" or canonical


def expected_lines(expected):
    lines = []
    for item in expected:
        typ, value = item.get("type"), item.get("value")
        if typ == "i32":
            lines.append("RESULT i32 " + str(int(value, 0) if isinstance(value, str) else int(value)))
        elif typ == "i64":
            lines.append("RESULT i64 " + str(int(value, 0) if isinstance(value, str) else int(value)))
        elif typ in ("f32", "f64"):
            width = 32 if typ == "f32" else 64
            if isinstance(value, str) and value.startswith("nan:"):
                lines.append((typ, value))
            else:
                lines.append("RESULT %s 0x%0*x" % (typ, width // 4, bits_float(value, width)))
        else:
            lines.append("RESULT void")
    return lines


def result_matches(output, expected):
    actual = [line.strip() for line in output.splitlines() if line.startswith("RESULT ")]
    wanted = expected_lines(expected)
    if len(actual) != len(wanted):
        return False
    for got, want in zip(actual, wanted):
        if isinstance(want, tuple):
            parts = got.split()
            if len(parts) != 3 or parts[0] != "RESULT" or parts[1] != want[0]:
                return False
            if not nan_bits(int(parts[2], 16), 32 if want[0] == "f32" else 64, want[1]):
                return False
        elif got != want:
            return False
    return True


class Harness:
    def __init__(self, runner, root, timeout):
        self.runner, self.root, self.timeout = runner, root, timeout
        self.modules = {}
        self.pass_count = self.fail_count = self.skip_count = 0

    def run(self, args):
        try:
            return subprocess.run(args, cwd=self.root, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=self.timeout)
        except (OSError, subprocess.TimeoutExpired):
            return None

    def module_path(self, command):
        filename = command.get("filename")
        if not filename:
            return None
        return filename if os.path.isabs(filename) else os.path.join(self.root, filename)

    def action(self, action):
        module = action.get("module")
        path = self.modules.get(module) if module else self.modules.get(None)
        if not path:
            return None
        args = [self.runner, "--quiet", "--result-line", path, action.get("field", "main")]
        for value in action.get("args", []):
            typ, raw = value.get("type"), value.get("value")
            args.extend(["--arg", "%s:%s" % (typ, raw)])
        return self.run(args)

    def check(self, command):
        typ = command.get("type")
        if typ == "module":
            path = self.module_path(command)
            if path is None:
                self.skip_count += 1
            else:
                self.modules[command.get("name")] = path
            return
        if typ == "register":
            self.skip_count += 1
            return
        if typ == "invoke":
            result = self.action(command.get("action", command))
            if result and result.returncode == 0: self.pass_count += 1
            else: self.fail_count += 1
            return
        if not typ.startswith("assert_"):
            self.skip_count += 1
            return
        module = command.get("module")
        if typ in ("assert_invalid", "assert_malformed", "assert_uninstantiable"):
            path = self.module_path(command) or self.modules.get(module)
            result = self.run([self.runner, "--quiet", path]) if path else None
            ok = result is not None and result.returncode != 0
        else:
            result = self.action(command.get("action", {}))
            if typ == "assert_return":
                ok = result is not None and result.returncode == 0 and result_matches(result.stdout, command.get("expected", []))
            elif typ == "assert_trap":
                ok = result is not None and result.returncode != 0 and "RESULT trap " in result.stdout
            else:
                ok = result is not None and result.returncode != 0
        if ok: self.pass_count += 1
        else: self.fail_count += 1

    def file(self, path):
        self.modules = {}
        try:
            data = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError) as exc:
            print("SKIP %s: %s" % (path, exc)); self.skip_count += 1; return
        before = self.pass_count, self.fail_count, self.skip_count
        for command in data.get("commands", []): self.check(command)
        after = self.pass_count, self.fail_count, self.skip_count
        print("%s: %d passed, %d failed, %d skipped" %
              (os.path.basename(path), after[0]-before[0], after[1]-before[1], after[2]-before[2]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("json", nargs="*", help="wast2json files")
    parser.add_argument("--runner", default=None)
    parser.add_argument("--root", default=".")
    parser.add_argument("--filter", default="")
    parser.add_argument("--timeout", type=float, default=2.0)
    ns = parser.parse_args()
    runner = ns.runner or ("build/wasm_runner.exe" if os.name == "nt" else "build/wasm_runner")
    files = [x for x in ns.json if ns.filter in os.path.basename(x)]
    if not ns.json:
        files = []
        for name in CURATED:
            if ns.filter not in name:
                continue
            candidate = os.path.join(ns.root, name + ".json")
            if os.path.exists(candidate):
                files.append(candidate)
            else:
                print("SKIP %s.json: not found" % name)
                # Missing curated inputs are explicit skips, not silent absence.
                # The total below remains useful in CI without bundled binaries.
    harness = Harness(runner, ns.root, ns.timeout)
    if not ns.json:
        harness.skip_count += sum(1 for name in CURATED
                                  if ns.filter in name and
                                  not os.path.exists(os.path.join(ns.root, name + ".json")))
    for path in files: harness.file(path)
    print("TOTAL: %d passed, %d failed, %d skipped" %
          (harness.pass_count, harness.fail_count, harness.skip_count))
    return 1 if harness.fail_count else 0


if __name__ == "__main__":
    sys.exit(main())

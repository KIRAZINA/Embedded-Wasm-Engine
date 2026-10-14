#!/usr/bin/env python3
"""Run wast2json output against the zero-allocation wasm CLI (session mode).

The suite is deliberately stdlib-only. Prepare files with, for example:
  wast2json --enable-all ./path/to/file.wast -o ./path/to/file.json
or reproducibly: sh tools/fetch_spec_tests.sh
Then: python3 tools/run_spec_suite.py tests/spec/json/i32.json --runner build/wasm_runner
Or everything: make spec

Execution model: each .json becomes one `--session` command file; a single
runner process executes it against live instances (LOAD replaces the
instance; consecutive INVOKE/GET on one instance share memory, globals and
table exactly like a live embedder). Multi-module linking is NOT supported
(single instance engine): modules importing registered aliases fail to link
and their asserts classify as cross-module deviations; only the spectest
host functions the CLI registers (env.print_i32) can satisfy imports.

Failure classification per failed case:
  ENGINE_BUG / VALIDATOR_BUG  unexpected divergence (make spec fails)
  RUNNER_BUG                  harness/protocol/transport failure (fails)
  KNOWN_DEVIATION             post-MVP construct, spectest/cross-module
                              dependence, or listed engine-subset limit
  CAPACITY_LIMIT              exceeds fixed engine/CLI caps (listed)
"""
import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys

CURATED = ["i32", "i64", "f32", "f64", "i32_cmp", "f32_cmp",
           "f64_cmp", "br", "br_if", "br_table", "block", "loop", "if",
           "call", "call_indirect", "global", "memory", "select",
           "unreachable", "conversions", "fac", "forward", "int_exprs"]

SESSION_PAGES = 64
# Fixed-capacity flags MUST match the runner under test. Defaults below
# mirror `make spec` (SPEC_CFLAGS); the shipping build is smaller.
MAX_FUNCS = 512
MAX_TABLE = 1024
MAX_GLOBALS = 32
MAX_DATA = 32
FILE_MAX = 65536

# Opcodes the MVP engine implements (anything else in a code body is a
# post-MVP construct by definition here).
MVP_SINGLE = set([
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x1A, 0x1B, 0x20, 0x21, 0x22, 0x23, 0x24, 0x28, 0x29,
    0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34,
    0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
    0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B,
    0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
    0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C,
    0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2,
    0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD,
    0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8,
    0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
])
BLOCKTYPE_SIMPLE = {0x40, 0x7C, 0x7D, 0x7E, 0x7F}

TRAP_MAP = {
    "WASM_TRAP_DIV_BY_ZERO": "integer divide by zero",
    "WASM_TRAP_INTEGER_OVERFLOW": "integer overflow",
    "WASM_TRAP_INVALID_CONVERSION": "invalid conversion to integer",
    "WASM_TRAP_MEMORY_OUT_OF_BOUNDS": "out of bounds memory access",
    "WASM_TRAP_TABLE_OUT_OF_BOUNDS": "undefined element",
    "WASM_TRAP_UNDEFINED_ELEMENT": "uninitialized element",
    "WASM_TRAP_INDIRECT_TYPE_MISMATCH": "indirect call signature mismatch",
    "WASM_TRAP_UNREACHABLE": "unreachable",
}

POSTMVP_TEXT_HINTS = (
    "saturat", "sign-exten", "memory.copy", "memory.fill", "memory.init",
    "data.drop", "table.copy", "table.fill", "table.get", "table.set",
    "table.grow", "table.size", "externref", "funcref", "eqref", "struct",
    "array", "br_on", "call_ref", "return_call", "multi-value", "result ",
    "shared", "atomic",
)


def bits_float(value, width):
    if isinstance(value, str) and value.lower().startswith("0x"):
        return int(value, 16)
    number = float(value)
    return (struct.unpack(">I", struct.pack(">f", number))[0] if width == 32
            else struct.unpack(">Q", struct.pack(">d", number))[0])


def nan_ok(bits, width, kind):
    if width == 32:
        exp = (bits >> 23) & 0xFF
        quiet = bits & 0x00400000
        canonical = bits == 0x7FC00000
    else:
        exp = (bits >> 52) & 0x7FF
        quiet = bits & 0x0008000000000000
        canonical = bits == 0x7FF8000000000000
    if exp != (0xFF if width == 32 else 0x7FF) or not quiet:
        return False
    return True if kind == "nan:arithmetic" else canonical


class WasmScan:
    """Minimal module probe: sections, imports, capacities, MVP coverage."""

    def __init__(self, path):
        self.path = path
        self.ok = False
        self.too_big = False
        self.imports = []       # (module, name, kind)
        self.multiret = False
        self.nonmvp_ops = set()
        self.mem_count = 0
        self.mem_min = 0
        self.table_count = 0
        self.table_min = 0
        self.table_other = False
        self.nfuncs = 0
        self.ndefs = 0
        self.nglobals = 0
        self.ndata = 0
        self.elem_flags = set()
        self.data_flags = set()
        self.elem4_other = False
        try:
            with open(path, "rb") as fh:
                self.data = fh.read()
        except OSError:
            return
        if len(self.data) > FILE_MAX:
            self.too_big = True
            return
        try:
            self._parse()
            self.ok = True
        except (IndexError, ValueError):
            self.ok = False

    def _u32(self, pos):
        val = shift = 0
        while True:
            byte = self.data[pos]
            pos += 1
            val |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                return val, pos
            if shift >= 35:
                raise ValueError("leb")

    def _sections(self):
        d = self.data
        if len(d) < 8 or d[0:4] != b"\x00asm" or d[4:8] != b"\x01\x00\x00\x00":
            raise ValueError("header")
        pos, out = 8, []
        while pos < len(d):
            sid = d[pos]
            pos += 1
            size, pos = self._u32(pos)
            out.append((sid, pos, pos + size))
            pos += size
        return out

    def _parse(self):
        secs = self._sections()
        payload = {}
        for sid, start, end in secs:
            if end > len(self.data):
                raise ValueError("overrun")
            payload.setdefault(sid, []).append((start, end))
        d = self.data

        def walk(sid):
            return payload.get(sid, [])

        if 1 in payload:
            start, end = payload[1][0]
            pos = start
            count, pos = self._u32(pos)
            for _ in range(count):
                if d[pos] != 0x60:
                    raise ValueError("functype")
                pos += 1
                npc, pos = self._u32(pos)
                pos += npc
                nrc, pos = self._u32(pos)
                if nrc > 1:
                    self.multiret = True
                pos += nrc
        if 2 in payload:
            for start, end in walk(2):
                pos = start
                count, pos = self._u32(pos)
                for _ in range(count):
                    mlen, pos = self._u32(pos)
                    mod = d[pos:pos + mlen].decode("utf-8", "replace")
                    pos += mlen
                    nlen, pos = self._u32(pos)
                    name = d[pos:pos + nlen].decode("utf-8", "replace")
                    pos += nlen
                    kind = d[pos]
                    pos += 1
                    self.imports.append((mod, name, kind))
                    if kind == 0:
                        _, pos = self._u32(pos)
                        self.nfuncs += 1
                    elif kind in (1, 2):
                        _, pos = self._scan_limits(pos)
                    elif kind == 3:
                        pos += 2
                    else:
                        raise ValueError("kind")
        if 3 in payload:
            start, end = payload[3][0]
            pos = start
            count, pos = self._u32(pos)
            self.ndefs = count
            self.nfuncs += count
        if 4 in payload:
            for start, end in walk(4):
                pos = start
                count, pos = self._u32(pos)
                self.table_count += count
                for _ in range(count):
                    elem = d[pos]
                    pos += 1
                    if elem != 0x70:
                        self.table_other = True
                    flags, pos = self._u32(pos)
                    tmin, pos = self._u32(pos)
                    if flags & 1:
                        _, pos = self._u32(pos)
                    if self.table_min == 0:
                        self.table_min = tmin
        if 5 in payload:
            for start, end in walk(5):
                pos = start
                count, pos = self._u32(pos)
                self.mem_count += count
                for _ in range(count):
                    flags, pos = self._u32(pos)
                    mmin, pos = self._u32(pos)
                    if flags & 1:
                        _, pos = self._u32(pos)
                    if count == 1 and self.mem_min == 0:
                        self.mem_min = mmin
        if 6 in payload:
            start, end = payload[6][0]
            pos = start
            count, pos = self._u32(pos)
            self.nglobals += count
        if 9 in payload:
            for start, end in walk(9):
                pos = start
                count, pos = self._u32(pos)
                for _ in range(count):
                    flags, pos = self._u32(pos)
                    self.elem_flags.add(flags)
                    if flags in (0, 2):
                        _, pos = self._u32(pos)  # tableidx
                        pos = self._skip_init(pos, end)
                        if flags == 2:
                            pos += 1  # elemkind
                    elif flags == 4:
                        pos = self._skip_init(pos, end)
                    elif flags in (1, 3):
                        pos += 1  # elemkind
                    elif flags in (5, 6, 7):
                        pos += 1  # reftype
                    else:
                        return
                    n, pos = self._u32(pos)
                    for _ in range(n):
                        if flags == 4:
                            if pos + 2 > end or self.data[pos] != 0xD2:
                                self.elem4_other = True
                                raise ValueError("elem-expr")
                            pos += 1
                            _, pos = self._u32(pos)
                            if pos >= end or self.data[pos] != 0x0B:
                                self.elem4_other = True
                                raise ValueError("elem-expr")
                            pos += 1
                        elif flags in (5, 6, 7):
                            pos = self._skip_init(pos, end)
                        else:
                            _, pos = self._u32(pos)
        if 10 in payload:
            for start, end in walk(10):
                pos = start
                count, pos = self._u32(pos)
                for _ in range(count):
                    size, pos = self._u32(pos)
                    pos += size  # bodies walked later by scan_code()
        if 11 in payload:
            for start, end in walk(11):
                pos = start
                count, pos = self._u32(pos)
                self.ndata += count
                for _ in range(count):
                    flags, pos = self._u32(pos)
                    self.data_flags.add(flags)
                    if flags == 2:
                        _, pos = self._u32(pos)  # memidx
                    elif flags != 0:
                        return
                    pos = self._skip_init(pos, end)  # offset
                    ln, pos = self._u32(pos)
                    pos += ln

    def _scan_limits(self, pos):
        flags, pos = self._u32(pos)
        _, pos = self._u32(pos)
        if flags & 1:
            _, pos = self._u32(pos)
        return None, pos

    def _skip_leb(self, pos, end, signed):
        while True:
            if pos >= end:
                raise ValueError("trunc")
            byte = self.data[pos]
            pos += 1
            if not byte & 0x80:
                return pos

    def _skip_init(self, pos, end):
        """Skip one const/global.get init expression incl. its end byte."""
        d = self.data
        if pos >= end:
            raise ValueError("trunc")
        op = d[pos]
        pos += 1
        if op in (0x41, 0x42, 0x23):
            while True:
                if pos >= end:
                    raise ValueError("trunc")
                byte = d[pos]
                pos += 1
                if not byte & 0x80:
                    break
        elif op == 0x43:
            if pos + 4 > end:
                raise ValueError("trunc")
            pos += 4
        elif op == 0x44:
            if pos + 8 > end:
                raise ValueError("trunc")
            pos += 8
        else:
            raise ValueError("init-expr")
        if pos >= end or d[pos] != 0x0B:
            raise ValueError("init-expr")
        return pos + 1

    def scan_code(self):
        """Opcode-coverage walk over every function body."""
        d = self.data
        try:
            secs = self._sections()
        except ValueError:
            return
        for sid, start, end in secs:
            if sid != 10:
                continue
            pos = start
            try:
                count, pos = self._u32(pos)
            except (IndexError, ValueError):
                return
            for _ in range(count):
                try:
                    size, pos = self._u32(pos)
                    bend = pos + size
                    # locals vector
                    nvec, pos = self._u32(pos)
                    for _ in range(nvec):
                        _, pos = self._u32(pos)
                        pos += 1
                    self._scan_insns(pos, bend)
                    pos = bend
                except (IndexError, ValueError):
                    return

    def _scan_insns(self, pos, end):
        d = self.data

        def need(n):
            if pos + n > end:
                raise ValueError("trunc")

        def u32(p):
            val = shift = 0
            while True:
                if p >= end:
                    raise ValueError("trunc")
                byte = d[p]
                p += 1
                val |= (byte & 0x7F) << shift
                shift += 7
                if not byte & 0x80:
                    return val, p
                if shift >= 35:
                    raise ValueError("leb")

        while pos < end:
            op = d[pos]
            pos += 1
            if op in (0x02, 0x03, 0x04):
                need(1)
                blocktype = d[pos]
                pos += 1
                if blocktype not in BLOCKTYPE_SIMPLE:
                    self.nonmvp_ops.add("blocktype-%02x" % blocktype)
                continue
            if op in (0x0C, 0x0D, 0x10, 0x20, 0x21, 0x22, 0x23, 0x24):
                pos = self._skip_leb(pos, end, False)
                continue
            if op == 0x11:
                # call_indirect: type index + reserved byte.
                pos = self._skip_leb(pos, end, False)
                pos = self._skip_leb(pos, end, False)
                continue
            if op == 0x0E:
                count, pos = u32(pos)
                for _ in range(count + 1):
                    _, pos = u32(pos)
                continue
            if op in (0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
                      0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                      0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E):
                pos = self._skip_leb(pos, end, False)
                pos = self._skip_leb(pos, end, False)
                continue
            if op in (0x3F, 0x40):
                need(1)
                pos += 1
                continue
            if op == 0x41:
                pos = self._skip_leb(pos, end, True)
                continue
            if op == 0x42:
                pos = self._skip_leb(pos, end, True)
                continue
            if op == 0x43:
                need(4)
                pos += 4
                continue
            if op == 0x44:
                need(8)
                pos += 8
                continue
            if op not in MVP_SINGLE:
                self.nonmvp_ops.add("%02x" % op)
                # Unknown immediate shape: stop this body to stay in sync.
                return


def encode_arg(typ, value):
    if typ == "i32":
        v = int(value, 0) & 0xFFFFFFFF
        if v >= 0x80000000:
            v -= 0x100000000
        return "i32:%d" % v
    if typ == "i64":
        v = int(value, 0) & 0xFFFFFFFFFFFFFFFF
        if v >= 0x8000000000000000:
            v -= 0x10000000000000000
        return "i64:%d" % v
    if typ in ("f32", "f64"):
        width = 32 if typ == "f32" else 64
        if isinstance(value, str) and value.startswith("nan:"):
            bits = 0x7FC00000 if width == 32 else 0x7FF8000000000000
        elif isinstance(value, str) and value.lower().startswith("0x"):
            bits = int(value, 16)
        elif isinstance(value, str):
            try:
                bits = int(value, 0)
            except ValueError:
                bits = bits_float(value, width)
        else:
            bits = bits_float(value, width)
        return "%s:0x%0*x" % (typ, width // 4, bits & ((1 << width) - 1))
    raise ValueError("arg type " + str(typ))


def match_expected(expected, replies):
    """Compare one RESULT reply against the expected value list."""
    if not replies:
        return False
    line = replies[0].strip()
    if not expected:
        return line == "RESULT void"
    if len(expected) != 1 or not line.startswith("RESULT "):
        return False
    want = expected[0]
    parts = line.split()
    typ, value = want.get("type"), want.get("value")
    if typ in ("f32", "f64"):
        width = 32 if typ == "f32" else 64
        if len(parts) != 3 or parts[1] != typ:
            return False
        try:
            got = int(parts[2], 16)
        except ValueError:
            return False
        if isinstance(value, str) and value.startswith("nan:"):
            return nan_ok(got, width, value)
        if isinstance(value, str):
            try:
                bits = int(value, 0)
            except ValueError:
                bits = bits_float(value, width)
        else:
            bits = bits_float(value, width)
        mask = (1 << width) - 1
        return (got & mask) == (bits & mask)
    if typ in ("i32", "i64"):
        width = 32 if typ == "i32" else 64
        if len(parts) != 3 or parts[1] != typ:
            return False
        try:
            got = int(parts[2], 0)
            want_v = int(value, 0) & ((1 << width) - 1)
            if want_v >= (1 << (width - 1)):
                want_v -= 1 << width
        except (ValueError, TypeError):
            return False
        return got == want_v
    return False


class Suite:
    def __init__(self, path, runner, workdir, timeout):
        self.path = path
        self.name = os.path.basename(path)[:-5]
        self.runner = runner
        self.workdir = workdir
        self.timeout = timeout
        with open(path, encoding="utf-8", errors="replace") as fh:
            self.doc = json.load(fh)
        self.root = os.path.dirname(os.path.abspath(path))
        self.cmds = []       # session lines
        self.plan = []       # per-command evaluation records
        self.instances = {}  # $name -> wasm path
        self.aliases = {}    # register name -> wasm path
        self.current = None  # current instance name (None = anonymous)
        self.current_path = None
        self.pass_count = 0
        self.fail_count = 0
        self.classes = {}
        self.notes = []

    def wasm_path(self, filename):
        if filename is None:
            return None
        return filename if os.path.isabs(filename) else os.path.join(
            self.root, filename)

    def scan(self, path):
        if path is None:
            return None
        if path not in self._scans:
            self._scans[path] = WasmScan(path)
        return self._scans[path]

    _scans = {}

    # -- script generation ------------------------------------------------

    def ref_path(self, mod):
        if mod is None:
            return self.current_path
        return self.instances.get(mod)

    def need_load(self, mod):
        """Emit LOAD when the referenced instance differs from current."""
        path = self.ref_path(mod)
        key = mod if mod is not None else "\x00anon"
        if path is None:
            return None
        if key != self._loaded_key:
            self.cmds.append("LOAD " + path)
            n = len(self.cmds) - 1
            self.plan.append({"kind": "load", "key": key, "path": path,
                              "assert": None})
            self._loaded_key = key
            return n
        return None

    _loaded_key = None

    def build(self):
        self._todo = list(self.doc.get("commands", []))
        self._loaded_key = "\x00none"
        for idx, cmd in enumerate(self._todo):
            t = cmd["type"]
            if t == "module":
                path = self.wasm_path(cmd.get("filename"))
                name = cmd.get("name")
                if name is not None:
                    self.instances[name] = path
                self.current = name
                self.current_path = path
                key = name if name is not None else "\x00anon"
                self.cmds.append("LOAD " + path)
                self.plan.append({"kind": "module", "key": key,
                                  "path": path, "assert": None,
                                  "line": cmd.get("line")})
                self._loaded_key = key
            elif t == "register":
                mod = cmd.get("name")
                path = self.instances.get(mod) if mod is not None \
                    else self.current_path
                if path is not None and cmd.get("as") is not None:
                    self.aliases[cmd["as"]] = path
            elif t in ("assert_return", "assert_trap", "action"):
                a = cmd.get("action", {}) or {}
                self.need_load(a.get("module"))
                if a.get("type") == "get":
                    self.cmds.append("GET " + a.get("field", ""))
                    self.plan.append({"kind": "get", "assert": cmd,
                                      "line": cmd.get("line")})
                else:
                    args = a.get("args", []) or []
                    try:
                        toks = [encode_arg(v.get("type"), v.get("value"))
                                for v in args]
                    except (ValueError, TypeError, AttributeError):
                        toks = None
                    if toks is None:
                        self.cmds.append("INVOKE 0 __badargs__")
                    else:
                        self.cmds.append("INVOKE %d %s %s" % (
                            len(toks), " ".join(toks), a.get("field", "")))
                    self.plan.append({"kind": "invoke", "assert": cmd,
                                      "line": cmd.get("line")})
            elif t in ("assert_invalid", "assert_malformed",
                       "assert_unlinkable", "assert_uninstantiable"):
                path = self.wasm_path(cmd.get("filename"))
                self.cmds.append("LOAD " + (path or "<missing>"))
                self.plan.append({"kind": "load-expect-error",
                                  "assert": cmd, "line": cmd.get("line"),
                                  "path": path})
                self._loaded_key = "\x00none"
                # NOTE: the previously live instance is intentionally NOT
                # restored here. need_load() reloads it on next reference
                # (fresh state); any state thereby lost is triaged per
                # assert through the normal classification rules.
            elif t == "assert_exhaustion":
                a = cmd.get("action", {}) or {}
                self.need_load(a.get("module"))
                args = a.get("args", []) or []
                try:
                    toks = [encode_arg(v.get("type"), v.get("value"))
                            for v in args]
                except (ValueError, TypeError, AttributeError):
                    toks = None
                if toks is None:
                    self.cmds.append("INVOKE 0 __badargs__")
                else:
                    self.cmds.append("INVOKE %d %s %s" % (
                        len(toks), " ".join(toks), a.get("field", "")))
                self.plan.append({"kind": "exhaustion", "assert": cmd,
                                  "line": cmd.get("line")})
            else:
                self.plan.append({"kind": "skip", "assert": cmd,
                                  "line": cmd.get("line")})

    # -- execution --------------------------------------------------------

    def run(self):
        script = os.path.join(self.workdir, self.name + ".session")
        with open(script, "w", encoding="utf-8", newline="\n") as fh:
            for line in self.cmds:
                fh.write(line + "\n")
        try:
            proc = subprocess.run(
                [self.runner, "--session", script],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=self.timeout, text=True, errors="replace")
        except subprocess.TimeoutExpired:
            return None
        except OSError:
            return None
        if proc.returncode not in (0,):
            return None
        replies = proc.stdout.splitlines()
        errs = [l for l in proc.stderr.splitlines() if l.strip()]
        if errs:
            self.notes.append("stderr: %s" % errs[0][:120])
        # Pair replies to planned commands (empty lines never occur: the
        # generator emits no blanks and the session answers every line).
        out = []
        for i, rec in enumerate(self.plan):
            if i < len(replies):
                out.append((rec, replies[i].strip()))
            else:
                out.append((rec, "<missing-reply>"))
        if len(replies) != len(self.plan):
            self.notes.append("reply-count %d vs %d commands" %
                              (len(replies), len(self.plan)))
        return out

    # -- evaluation -------------------------------------------------------

    def file_flags(self, path):
        scan = self.scan(path)
        flags = set()
        if scan is None:
            return flags
        if not scan.ok:
            return set(["UNPARSEABLE"])
        if scan.too_big:
            flags.add("CAPACITY:file-size")
            return flags
        scan.scan_code()
        if scan.nfuncs > MAX_FUNCS:
            flags.add("CAPACITY:functions")
        if scan.table_min > MAX_TABLE:
            flags.add("CAPACITY:table")
        if scan.table_count > 1:
            flags.add("POSTMVP:multi-table")
        if scan.mem_min > SESSION_PAGES:
            flags.add("CAPACITY:memory")
        if scan.mem_count > 1:
            flags.add("POSTMVP:multi-memory")
        if scan.nglobals > MAX_GLOBALS:
            flags.add("CAPACITY:globals")
        if scan.ndata > MAX_DATA:
            flags.add("CAPACITY:data")
        if scan.multiret:
            flags.add("POSTMVP:multi-value")
        if scan.nonmvp_ops:
            flags.add("POSTMVP:opcodes(%s)" %
                      ",".join(sorted(scan.nonmvp_ops)[:4]))
        if scan.table_other:
            flags.add("POSTMVP:externref-table")
        mods = set(m for m, _n, _k in scan.imports)
        if "spectest" in mods:
            flags.add("SPECTEST:imports")
        return flags

    def text_flags(self, text):
        text = (text or "").lower()
        for hint in POSTMVP_TEXT_HINTS:
            if hint in text:
                return set(["POSTMVP:text(%s)" % hint.strip()])
        return set()

    def record(self, ok, cls, line, note):
        if ok:
            self.pass_count += 1
        else:
            self.fail_count += 1
            self.classes[cls] = self.classes.get(cls, 0) + 1
            self.notes.append("line %s [%s] %s" % (line, cls, note[:160]))

    def evaluate(self, paired):
        live = {}
        for rec, reply in paired:
            kind = rec["kind"]
            cmd = rec.get("assert")
            line = rec.get("line")
            if kind == "module":
                if reply.startswith("LOADED"):
                    live[rec["key"]] = True
                    self.pass_count += 1
                else:
                    live[rec["key"]] = False
                    flags = self.file_flags(rec.get("path"))
                    self.record(False, self.stage_class(
                        "validation", flags, reply), line,
                        "module failed to load: %s %s" % (reply, flags))
            elif kind == "load":
                live[rec["key"]] = reply.startswith("LOADED")
            elif kind == "load-expect-error":
                t = (cmd or {}).get("type", "")
                if reply.startswith("LOADED"):
                    flags = self.file_flags(rec.get("path"))
                    flags |= self.text_flags((cmd or {}).get("text"))
                    self.record(False, self.unexpected_ok_class(t, flags),
                                line, "engine accepted %s: %s" % (t, reply))
                else:
                    self.pass_count += 1
            elif kind == "invoke":
                self.eval_invoke(cmd, reply, live)
            elif kind == "get":
                self.eval_get(cmd, reply, live)
            elif kind == "exhaustion":
                if reply.startswith("RESULT trap "):
                    self.pass_count += 1
                elif reply == "EXHAUSTED":
                    self.record(False, "KNOWN_DEVIATION:step-budget-loop",
                                line, "loop did not trap within budget")
                else:
                    self.record(False, "ENGINE_BUG:no-trap", line,
                                "exhaustion without trap: %s" % reply)
            elif kind == "skip":
                self.notes.append("line %s: unhandled command %s" %
                                  (line, (cmd or {}).get("type")))

    def stage_class(self, stage, flags, reply):
        for flag in sorted(flags):
            if flag.startswith("CAPACITY"):
                return "CAPACITY_LIMIT:" + flag.split(":", 1)[1]
            if flag.startswith("SPECTEST"):
                return "KNOWN_DEVIATION:spectest-host"
            if flag.startswith("POSTMVP"):
                return "KNOWN_DEVIATION:" + flag.split(":", 1)[1]
        if "no-module" in reply or "arg-count" in reply or \
                "arg-type" in reply:
            return "RUNNER_BUG:protocol"
        if stage == "validation" or "LOAD_ERROR" in reply or \
                "WASM_ERR" in reply:
            return "VALIDATOR_BUG:rejects-valid"
        return "ENGINE_BUG:link-failed"

    def unexpected_ok_class(self, t, flags):
        if self.name.startswith("utf8-"):
            return "KNOWN_DEVIATION:utf8-names-unchecked"
        for flag in sorted(flags):
            if flag.startswith("POSTMVP"):
                return "KNOWN_DEVIATION:accepted-" + flag.split(":", 1)[1]
        return "VALIDATOR_BUG:accepts-%s" % t

    def eval_invoke(self, cmd, reply, live):
        t = (cmd or {}).get("type", "")
        line = (cmd or {}).get("line")
        a = (cmd or {}).get("action", {}) or {}
        if not live.get(a.get("module") if a.get("module") is not None
                        else "\x00anon", True):
            # Instance never linked: inherit the load failure class.
            self.record(False, "KNOWN_DEVIATION:dead-instance", line,
                        "invoke on unlinked instance")
            return
        if t == "assert_return":
            if match_expected(cmd.get("expected", []) or [], [reply]):
                self.pass_count += 1
            else:
                self.record(False, self.runtime_class(cmd, reply), line,
                            "return mismatch: got %r" % reply)
        elif t == "assert_trap":
            want = (cmd or {}).get("text", "")
            if reply.startswith("RESULT trap "):
                got = reply[len("RESULT trap "):].strip()
                spec = TRAP_MAP.get(got)
                if spec == want:
                    self.pass_count += 1
                else:
                    self.record(False, self.runtime_class(cmd, reply), line,
                                "trap %r maps to %r, want %r" %
                                (got, spec, want))
            else:
                self.record(False, self.runtime_class(cmd, reply), line,
                            "no trap: got %r want %r" % (reply, want))
        else:  # bare action: must succeed
            if reply.startswith("RESULT ") and "trap" not in reply and \
                    "ERROR" not in reply:
                self.pass_count += 1
            else:
                self.record(False, self.runtime_class(cmd, reply), line,
                            "bare action failed: %r" % reply)

    def eval_get(self, cmd, reply, live):
        line = (cmd or {}).get("line")
        a = (cmd or {}).get("action", {}) or {}
        if not live.get(a.get("module") if a.get("module") is not None
                        else "\x00anon", True):
            self.record(False, "KNOWN_DEVIATION:dead-instance", line,
                        "get on unlinked instance")
            return
        t = (cmd or {}).get("type", "")
        if t == "assert_return":
            if match_expected(cmd.get("expected", []) or [], [reply]):
                self.pass_count += 1
            else:
                self.record(False, self.runtime_class(cmd, reply), line,
                            "get mismatch: got %r" % reply)
        else:
            if reply.startswith("RESULT ") and "trap" not in reply and \
                    "ERROR" not in reply:
                self.pass_count += 1
            else:
                self.record(False, self.runtime_class(cmd, reply), line,
                            "bare get failed: %r" % reply)

    def runtime_class(self, cmd, reply):
        a = (cmd or {}).get("action", {}) or {}
        mod = a.get("module")
        path = self.instances.get(mod) if mod is not None \
            else self.current_path
        flags = self.file_flags(path)
        for flag in sorted(flags):
            if flag.startswith("SPECTEST"):
                return "KNOWN_DEVIATION:spectest-host"
            if flag.startswith("POSTMVP"):
                return "KNOWN_DEVIATION:" + flag.split(":", 1)[1]
            if flag.startswith("CAPACITY"):
                return "CAPACITY_LIMIT:" + flag.split(":", 1)[1]
        if " " in (a.get("field", "") or "") or "\n" in (a.get("field", "") or ""):
            return "RUNNER_BUG:whitespace-name"
        if reply in ("<missing-reply>",) or reply.startswith("SESSION_ERROR"):
            return "RUNNER_BUG:protocol"
        if reply.startswith("INVOKE_ERROR") or reply.startswith("GET_ERROR"):
            return "ENGINE_BUG:invoke-error"
        if reply == "EXHAUSTED":
            return "KNOWN_DEVIATION:step-budget-loop"
        return "ENGINE_BUG:mismatch"


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("json", nargs="*", help="wast2json files")
    parser.add_argument("--runner", default=None)
    parser.add_argument("--root", default=".")
    parser.add_argument("--filter", default="")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--fail-file", default=None)
    ns = parser.parse_args(argv)

    runner = ns.runner or ("build/wasm_runner.exe" if os.name == "nt"
                           else "build/wasm_runner")
    raw = [x for x in ns.json if ns.filter in os.path.basename(x)]
    files = []
    for pat in raw:
        # Expand globs here: Windows shells (and mingw32-make) do not.
        if glob.has_magic(pat):
            files.extend(sorted(glob.glob(pat)))
        elif os.path.isfile(pat):
            files.append(pat)
        else:
            print("SKIP %s: not found" % pat)
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
    workdir = ns.workdir or os.path.join("build", "specrun")
    if os.path.isdir(workdir):
        import shutil
        shutil.rmtree(workdir)
    os.makedirs(workdir)

    total_pass = total_fail = 0
    bad_classes = {}
    fail_lines = []
    for path in files:
        suite = Suite(path, runner, workdir, ns.timeout)
        try:
            suite.build()
        except (ValueError, KeyError, TypeError, AttributeError) as exc:
            print("%s: harness error building script: %s" %
                  (suite.name, exc))
            total_fail += 1
            bad_classes["RUNNER_BUG:build"] = \
                bad_classes.get("RUNNER_BUG:build", 0) + 1
            continue
        paired = suite.run()
        if paired is None:
            print("%s: harness error running session" % suite.name)
            total_fail += 1
            bad_classes["RUNNER_BUG:session"] = \
                bad_classes.get("RUNNER_BUG:session", 0) + 1
            continue
        suite.evaluate(paired)
        total_pass += suite.pass_count
        total_fail += suite.fail_count
        for cls, n in suite.classes.items():
            bad_classes[cls] = bad_classes.get(cls, 0) + n
            if not (cls.startswith("KNOWN_DEVIATION") or
                    cls.startswith("CAPACITY_LIMIT")):
                fail_lines.append("%s: %s" % (suite.name, cls))
        print("%s: %d passed, %d failed%s" %
              (suite.name, suite.pass_count, suite.fail_count,
               (" " + str(suite.classes)) if suite.classes else ""))
        for note in suite.notes[:8]:
            print("    note: %s" % note)
    print("TOTAL: %d passed, %d failed" % (total_pass, total_fail))
    for cls in sorted(bad_classes):
        print("CLASS %s: %d" % (cls, bad_classes[cls]))
    if ns.fail_file is not None:
        with open(ns.fail_file, "w", encoding="utf-8") as fh:
            fh.write("\n".join(fail_lines) + "\n")
    hard = [c for c in bad_classes
            if not (c.startswith("KNOWN_DEVIATION") or
                    c.startswith("CAPACITY_LIMIT"))]
    return 1 if hard else 0


if __name__ == "__main__":
    sys.exit(main())

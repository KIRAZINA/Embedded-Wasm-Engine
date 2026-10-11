[![CI](https://img.shields.io/badge/CI-GitHub_Actions-blue)](.github/workflows/ci.yml)

# Embedded WebAssembly Interpreter (C11, zero-allocation)

An ultra-lightweight, zero-dependency WebAssembly MVP interpreter written in
standard **C11**, designed for resource-constrained targets (ESP32, STM32,
ARM Cortex-M, bare metal). It parses, links, and executes `.wasm` binaries
using only pre-allocated static storage — **no `malloc`/`calloc`/`realloc`
anywhere**, verified by audit. All builds are warning-free under
`-std=c11 -Wall -Wextra -Werror`.

## Features

- **Binary parser** — magic/version validation, zero-copy section traversal,
  full LEB128 (`u32`/`u64`/`s32`/`s64`) with overflow and truncation guards.
- **Execution core** — `switch`-dispatched interpreter over an operand stack,
  call stack, and per-frame control stack.
- **Integer core** — complete i32 ALU (add/sub/mul/div/rem, bitwise ops,
  `%32`-masked shifts with portable arithmetic `shr_s`, all comparisons),
  plus the full i64 ALU (`%64`-masked shifts, `INT64_MIN/-1` trapping).
  `INT32_MIN/-1` and division-by-zero trap instead of invoking C UB.
- **Float core** - complete f32/f64 MVP set: const (bit-exact LE
  immediates), add/sub/mul/div (x/0 is +/-Inf, 0/0 is NaN, never a trap),
  sqrt (negative -> NaN), min/max (NaN in -> canonical quiet NaN;
  min(+0,-0) = -0, max(+0,-0) = +0), abs/neg/copysign and all four
  reinterprets (pure bit patterns), ceil/floor/trunc, nearest
  (hand-rolled roundTiesToEven), all 12 comparisons, all 8 truncating
  conversions (NaN -> WASM_TRAP_INVALID_CONVERSION, out-of-range ->
  WASM_TRAP_INTEGER_OVERFLOW, exact power-of-two boundaries), all
  int<->float converts plus demote/promote. NaN payloads: const/memory
  round-trips are bit-exact; min/max emit canonical quiet NaNs
  (0x7FC00000, 0x7FF8000000000000); other arithmetic propagates the
  host NaN (quiet bit guaranteed).
- **Structured control flow** — `block`/`loop`/`if`/`else`/`end`,
  `br`/`br_if`/`br_table` with relative labels, `return`, `call`,
  `local.get/set/tee`, untyped `select` over i32/i64/f32/f64.
- **Tables / indirect calls** - one funcref table (fixed storage, element
  segments applied at instantiation with validate-all-then-write) and
  `call_indirect` with full element-by-element signature checks
  (`TABLE_OUT_OF_BOUNDS` / `UNDEFINED_ELEMENT` /
  `INDIRECT_TYPE_MISMATCH` traps; typed host imports callable).
- **Globals / data / start** - mutable and immutable globals
  (`global.get`/`global.set`, `const`-only init expressions, imported
  globals by name), data segments applied to bound memory at
  instantiation (validate-all-then-copy, never partial), and the start
  function (`[]->[]` only) executed last at instantiation.
- **Linear memory** — bounds-checked little-endian `u8/i32/i64/f32/f64`
  loads/stores (unaligned-safe for strict-alignment ARM), page grow with
  zero-filled tails, wraparound-proof offset checks.
- **Host interop** — register native C callbacks (`env.print_i32`, …),
  link function imports with signature checking, resolve exports by name,
  pass CLI integers as arguments, print results.
- **CLI runner** — execute exported functions with a step-budget watchdog,
  or inspect modules with `--dump` (header, section table, type signatures,
  export entries) without executing anything.

## Scope limits (by design)

- No f32/f64 load/store opcodes yet (`0x2A/0x2B/0x38/0x39`): the
  bounds-checked, NaN-payload-exact memory helpers exist and are tested,
  but the interpreter does not dispatch float memory instructions.
- Blocks carry void or i32 results; functions return at most one value.
- Fixed capacities (see Limits); oversized modules trap cleanly instead of
  allocating.
- CLI supports `--arg i32:5`, `--arg i64:-1`, `--arg f32:3.5`, and
  `--arg f64:2e10`; hexadecimal float values use raw bits, e.g.
  `f32:0x3f800000` or `f64:0x3ff0000000000000`.
  or GC extensions.
- Table imports are rejected (`WASM_ERR_IMPORTED_TABLES_UNSUPPORTED`):
  the host has no table concept, so a module importing a table cannot
  link. Only one defined table is supported, and only the first data /
  element section is applied if several are present.

## Layout

```
.
├── Plan.md            # original design spec and phased roadmap
├── Makefile           # all / test / test1..6 / runner / clean (POSIX shell)
├── include/           # leb128.h, wasm_types.h, wasm_parser.h, wasm_stack.h,
│                      #   wasm_memory.h, wasm_frame.h, wasm_control.h,
│                      #   wasm_vm.h, wasm_host.h
├── src/               # leb128.c, wasm_parser.c, wasm_stack.c, wasm_memory.c,
│                      #   wasm_frame.c, wasm_control.c, wasm_vm.c,
│                      #   wasm_host.c, main.c (CLI runner)
├── tests/             # test_phase1.c … test_phase10.c, test_cli.c (assert-based, no framework)
├── demo/              # add.wasm (main(a,b)->a+b), print.wasm (host-callback demo),
│                      #   fadd.wasm (main()->f32 { 1.5 + 2.5 }),
│                      #   dispatch.wasm (main(i)->i32 dispatch via br_table),
│                      #   dispatch2.wasm (add/sub chosen at runtime via call_indirect),
│                      #   hello.wasm (data "Hello" + print loop via env.print_i32)
└── build/             # wasm_runner + 11 test binaries (generated)
```

## Requirements

- A C11 compiler (gcc/clang). POSIX `make` + shell for the Makefile.
- C standard library plus `libm` for the float core (`<math.h>`:
  `sqrt`/`ceil`/`floor`/`trunc`/`isnan` only — `min`/`max`/`nearest` and
  all trunc conversions are hand-implemented for exact WASM semantics).
  The Makefile links it via `LDLIBS ?= -lm` (override or empty it if your
  toolchain needs no `-lm`).

## Build

```sh
make all    # builds build/wasm_runner + all test binaries
make test   # builds everything, runs all suites, prints TOTAL: X passed, Y failed
make clean  # removes build/
```

Windows PowerShell (the Makefile uses `rm`/`mkdir`, which need coreutils —
otherwise invoke the same recipes directly):

```powershell
New-Item -ItemType Directory -Path build | Out-Null
gcc -std=c11 -Wall -Wextra -Werror -Iinclude src/*.c src/main.c -o build/wasm_runner.exe
```

## Usage

Machine clients can use `--quiet --result-line`; successful results are
`RESULT i32 N`, `RESULT i64 N`, `RESULT f32 0xXXXXXXXX`, `RESULT f64
0xXXXXXXXXXXXXXXXX`, or `RESULT void`. Runtime failures use
`RESULT trap NAME`. `--version` prints `WASM_ENGINE_VERSION`.

Run an exported function (defaults to `main`), passing i32 arguments:

```powershell
.\build\wasm_runner.exe demo\add.wasm main 3 4
# [load] 41 bytes from 'demo\add.wasm'
# [parse] header ok
# [link] 1 function(s)
# [export] 'main' -> func 0
# result: 7 (i32)
```

Guest-to-host callback demo (`print.wasm` imports `env.print_i32`):

```powershell
.\build\wasm_runner.exe demo\print.wasm
# ...
# [export] 'main' -> func 1
# 7
# result: 35 (i32)
```

Float demo (`fadd.wasm` computes `1.5 + 2.5` with no arguments):

```powershell
.\build\wasm_runner.exe demo\fadd.wasm main
# [load] 45 bytes from 'demo\fadd.wasm'
# [parse] header ok
# [link] 1 function(s)
# [export] 'main' -> func 0
# result: 4 (f32)
```

Dispatch demo (`dispatch.wasm` selects 100/200/300 via `br_table`):

```powershell
.\build\wasm_runner.exe demo\dispatch.wasm main 0  # result: 100 (i32)
.\build\wasm_runner.exe demo\dispatch.wasm main 2  # result: 300 (i32)
```

Indirect-call demo (`dispatch2.wasm` picks add/sub at runtime via
`call_indirect` over a two-entry table):

```powershell
.\build\wasm_runner.exe demo\dispatch2.wasm main 0 10 4  # result: 14 (i32)
.\build\wasm_runner.exe demo\dispatch2.wasm main 1 10 4  # result: 6 (i32)
```

Data + host-call demo (`hello.wasm` places "Hello" (72,101,108,108,111
as i32 cells) via a data segment, then loops calling `env.print_i32`):

```powershell
.\build\wasm_runner.exe demo\hello.wasm main
```

## WebAssembly 1.0 (MVP) scope

This is an embedded-oriented WebAssembly 1.0 interpreter with zero dynamic
allocation. It is intentionally a bounded implementation: module bytes,
linear-memory backing storage, VM state, and host registries are owned by the
caller or fixed-capacity objects.

All MVP core sections are traversed, with custom section 0 skipped correctly:

| Section | Status |
|---:|---|
| 0 custom | Supported; ignored during traversal and validation |
| 1 type | Supported |
| 2 import | Supported for the implemented host function/global bindings |
| 3 function | Supported |
| 4 table | Supported; one bounded funcref table |
| 5 memory | Supported; one bounded linear memory |
| 6 global | Supported |
| 7 export | Supported |
| 8 start | Supported |
| 9 element | Supported; funcref initialization |
| 10 code | Supported |
| 11 data | Supported |

The interpreter dispatches 123 MVP instruction opcodes, grouped as follows:

| Group | Count | Coverage |
|---|---:|---|
| Control, calls, branches, select | 13 | structured control, direct/indirect calls |
| Constants, locals, globals | 9 | four value types and mutable globals |
| i32 integer | 24 | comparisons, arithmetic, bitwise, shifts, memory load/store |
| i64 integer | 13 | arithmetic, bitwise, shifts |
| f32/f64 floating point | 40 | arithmetic, comparisons, rounding, min/max |
| Conversions and reinterpretation | 22 | truncation, conversion, promote/demote, bit reinterpretation |
| Memory | 2 | `i32.load` and `i32.store` |

Not implemented: threads/atomics, SIMD, GC, multi-value blocks or returns,
bulk-memory operations, reference types, sign-extension operations, tail
calls, multi-memory, and exception handling. Floating-point memory load/store
opcodes are also not dispatched. These omissions are deliberate MVP subset
limits and are not claims of full modern-WebAssembly coverage.

## Quickstart

Requirements are a C11 compiler, Make, and the standard C library plus `libm`.
No package manager, runtime library, or Python dependency is needed for the
core build.

```sh
make
make test
make fuzz SEED=7 ITER=1000000
make sanitize
```

The optional spec harness uses Python 3 standard-library code and user-supplied
`wast2json` output:

```sh
wast2json --spec path/to/test.wast -o path/to/test.json
python3 tools/run_spec_suite.py path/to/test.json --runner build/wasm_runner
```

The harness does not bundle the WebAssembly spec corpus. Missing curated files
are reported as skips, not passes.

## CLI reference

`wasm_runner` exits 0 on success, 1 on load/link/runtime failure, and 2 on
usage or I/O failure. Typed arguments use `--arg i32:5`, `--arg i64:-1`,
`--arg f32:3.5`, or `--arg f64:2e10`; floating-point bit patterns use a
hexadecimal integer, for example `--arg f32:0x3f800000` and
`--arg f64:0x3ff0000000000000`. `--quiet` suppresses banners. `--result-line`
prints exactly one `RESULT ...` line for machine consumers. `--version`
prints `1.0.0`.

## Conformance and fuzz tooling

`make fuzz SEED=1 ITER=1000000` runs the deterministic in-repo smoke fuzzer.
`make sanitize` builds the runner, all tests, and the fuzzer with AddressSanitizer
and UndefinedBehaviorSanitizer, then runs the suite and one-million-iteration
fuzz pass. The sanitizer runtimes must be provided by the selected C toolchain.

The spec harness is `tools/run_spec_suite.py` and uses only Python 3's standard
library. The user supplies `.wast` conversions; no binaries are bundled. A
typical preparation command is:

`wast2json --spec path/to/test.wast -o path/to/test.json`

Then run `python tools/run_spec_suite.py path/to/test.json --runner build/wasm_runner`.
The curated default names are documented in the script; `--filter=NAME` limits
the run. Unsupported JSON command forms are counted as skips and reported.

Conformance evidence in this checkout is bounded by the absence of bundled
`.json` inputs: the curated run reports 0 executed assertions and 23 skipped
files until the user supplies conversions. The deterministic smoke fuzzer
completed 1,000,000 iterations with seed 1 (`parsed=499288`,
`instantiated=51985`, `executed=19112`) without crashes. Sanitizer execution
requires a compiler runtime providing ASan and UBSan; the reference MinGW
host does not provide those libraries, while the committed Ubuntu CI jobs use
native GCC and Clang sanitizer runtimes.

```
# [load] 125 bytes from 'demo\hello.wasm'
# [parse] header ok
# [link] 2 function(s)
# [export] 'main' -> func 1
# 72
# 101
# 108
# 108
# 111
# ok (no result)
```

Result formats: `result: <int> (i32)`, `result: <int> (i64)`,
`result: <%.9g> (f32)` (9 significant digits round-trip any f32),
`result: <%.17g> (f64)` (17 digits round-trip any f64).
Non-finite floats print as C99 `nan`/`inf` (e.g. `result: nan (f32)`).

Inspect a module without executing it:

```powershell
.\build\wasm_runner.exe --dump demo\add.wasm
# [dump] magic: 00 61 73 6d
# [dump] version: 1
# [dump] size: 41 bytes
# [dump] sections:
# [dump]   id=1 name=Type header_off=8 payload_off=10 payload_len=7
# [dump]   id=3 name=Function header_off=17 payload_off=19 payload_len=2
# [dump]   id=7 name=Export header_off=21 payload_off=23 payload_len=8
# [dump]   id=10 name=Code header_off=31 payload_off=33 payload_len=8
# [dump] types: 1
# [dump]   functype 0: params[i32, i32] results[i32]
# [dump] exports: 1
# [dump]   "main" kind=function index=0
```

Exit codes: `0` success, `1` link/runtime error (message names the trap,
e.g. `WASM_TRAP_DIV_BY_ZERO`), `2` usage/I/O error.

## Targets

The core is suitable for freestanding Cortex-M builds. `make arm-cross`
requires `arm-none-eabi-gcc` and `arm-none-eabi-ar`; it compiles every core
source except `src/main.c`, creates `build/arm/libwasm_engine.a`, and links
`build/arm/embedded_demo.elf` for Cortex-M4 with `-specs=nosys.specs` and a
link map. The STM32 demo embeds `demo/add.wasm` in flash, parses and validates
it before instantiation, invokes `main(3, 4)`, and sends the decimal result to
the board-specific `embedded_putc` retarget hook. The checked-in byte array is
regenerated with `python3 tools/bin2c.py demo/add.wasm
targets/stm32/demo_add_wasm.c embedded_demo_wasm`.

`make size-report` prints the measured static engine footprint and contains a
compile-time assertion that `sizeof(wasm_vm_t)` fits the documented 112 KiB
STM32F4 VM budget. On the reference MinGW build this is:

```
wasm_vm_t=105008 bytes
wasm_host_registry_t=3584 bytes
wasm_memory_t=24 bytes
engine_static_footprint=108616 bytes
```

For ESP32 and ESP32-C3, see [targets/esp32/README.md](targets/esp32/README.md)
for the exact ESP-IDF component setup and `idf.py` commands. The component
wrapper links the same zero-allocation core; application code supplies its
static memory arena and host bindings.

## Testing

Standalone `assert`-style suites, no test framework. Current status:
**1599/1599 passing, 0 failed** (`make test` prints `TOTAL: 1599 passed, 0 failed`).

| Suite | Coverage | Checks |
|---|---|---|
| `test_phase1` | LEB128 vectors/overflow/truncation, header + section parsing | 58 |
| `test_phase2` | Stack push/pop/peek, overflow/underflow, LE memory round-trips, OOB + wraparound, page grow | 100 |
| `test_phase3` | Frames, call stack, enter/exit, locals, height restoration | 137 |
| `test_phase4` | i32 ALU, div-by-zero, `MIN/-1`, shifts, comparisons, load/store | 137 |
| `test_phase5` | blocks/loops/branches, factorial, `call`/`return` interop | 74 |
| `test_phase6` | Host callbacks, export lookup, import binding, instantiation, i64 ALU, CLI `--dump` output | 160 |
| `test_cli` | End-to-end runner runs of hand-built `()->i64` modules, exact `result:` rendering incl. `INT64_MIN` | 15 |
| `test_phase10` | Table/element validation, indirect dispatch/bounds/unset/signature matrix, host indirect, recursion, overwrite/reset/scanner, CLI dispatch2 run | 150 |
| `test_phase9` | Global get/set/immutability/init forms/imports, data placement/bounds/caps, start run-once/rejects/host-start, register validation, CLI hello run | 200 |
| `test_phase8` | br_table all-arms/default/unsigned-index/loop-target/valued-carry/malformed/scanner, select over 4 types incl. NaN/mismatch/underflow, CLI dispatch run | 102 |
| `test_phase7` | f32/f64 const/arith/sqrt/min-max/abs-neg-copysign/round/nearest/cmp, all 8 trunc boundaries, converts, reinterprets, NaN propagation, memory NaN round-trips, CLI f32 run + dump | 357 |
| `test_phase11` | static module validation corpus | 102 |
| `test_phase12` | conformance/fuzz tooling helpers | 7 |

## Architecture

```
.wasm bytes -> parse header/sections -> link imports + define functions
      -> bind memory -> resolve export -> push args -> run loop
      -> exit frame -> print result
```

- `wasm_parser` + `leb128`: ingestion; every multi-byte read is an explicit
  little-endian shift/mask, so parsing is endianness-independent.
- `wasm_stack` / `wasm_frame` / `wasm_control`: fixed-capacity operand
  stack (1024 slots), call stack (64 frames, 32 embedded locals each),
  per-frame control stack (32 entries). Overflow/underflow are trap values,
  never crashes.
- `wasm_vm`: fetch/decode/execute loop; typed operand pops; subtractions-form
  bounds checks (`offset > total - size`, computed in 64 bits) defeat
  wraparound attacks.
- `wasm_host`: 16-slot native callback registry; import→host binding with
  functype verification; export lookup; whole-module instantiation into the
  VM's 32-slot function table (imports first, per MVP index space).
- `main`: ≤64 KiB file buffer, 1-page arena, 10M-step watchdog.

## Fixed limits

| Resource | Cap | Overflow behavior |
|---|---|---|
| Operand stack | 1024 values | `WASM_TRAP_STACK_OVERFLOW` |
| Call stack | 64 frames | `WASM_TRAP_CALL_STACK_OVERFLOW` |
| Locals per frame | 32 | `WASM_TRAP_INVALID_LOCAL_INDEX` |
| Control nesting | 32 blocks | `WASM_TRAP_CONTROL_STACK_OVERFLOW` |
| VM functions | 32 | `WASM_TRAP_INVALID_ARGUMENT` |
| Host callbacks | 16 (31-char names) | `WASM_TRAP_INVALID_ARGUMENT` |
| CLI input file | 64 KiB | usage error |
| Branch labels | open depth | `WASM_TRAP_INVALID_BLOCK_LABEL` |
| Indirect table | 128 (`WASM_MAX_TABLE_SIZE`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| Globals (imported + defined) | 32 (`WASM_MAX_GLOBALS`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| Host-provided globals | 16 (`WASM_MAX_HOST_GLOBALS`, `-D` overridable) | `WASM_TRAP_INVALID_ARGUMENT` |
| Data segments | 32 (`WASM_MAX_DATA_SEGMENTS`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| br_table labels | 256 (`WASM_MAX_BR_TABLE_LABELS`, `-D` overridable) | `WASM_TRAP_INVALID_ARGUMENT` |
| Validation value stack | 1024 (`WASM_VALIDATE_STACK_MAX`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| Validation control depth | 32 (`WASM_MAX_CONTROL_STACK_DEPTH`, shared with execution) | `WASM_ERR_INVALID_BLOCK` |

Fuzz-smoke defaults are also overrideable at compile time: `FUZZ_MAX_BYTES`
256, `FUZZ_PAGE_COUNT` 1, and `FUZZ_STEP_BUDGET` 10.

## Memory footprint

Static engine cost (measured by `make size-report`):

```
sizeof(vm)=105008  sizeof(registry)=3584  sizeof(mem)=24
engine = 108616 bytes  (limit 131072)  →  < 128 KiB ✓
```

Guest linear memory and the module image live in application-owned buffers
the engine only borrows, so an embedded target pays the ~101.6 KiB engine
cost plus whatever arena it provisions.

## Roadmap status vs `Plan.md`

Phases 1–4 of the plan are implemented (delivered as six incremental phases
here); host integration works through the generic callback registry, proven
with `print_i32`. The f32/f64 arithmetic gap is closed (Phase 1 float work,
proven by `test_phase7` + `demo/fadd.wasm`). Globals, data segments, and
the start function are implemented (Phase 3 work, proven by `test_phase9`
+ `demo/hello.wasm`). Tables, element segments, and `call_indirect` are
implemented (Phase 4 work, proven by `test_phase10` + `demo/dispatch2.wasm`),
completing the full MVP section set. Deliberately still open:
multi-value blocks/functions, f32/f64 load/store opcodes, and
physical deployment to an ESP32/STM32 target (GPIO/LED bindings would
register through the existing host table).

## Baseline status (`v0.0.0-baseline`, 2026-10-11)

What works (measured, `mingw32-make CC=gcc test` on MinGW gcc 8.1.0):
warning-free build under `-std=c11 -Wall -Wextra -Wpedantic -Werror`;
`TOTAL: 681 passed, 0 failed` across all 7 suites (phase1..6 = 666 as
previously claimed — claim confirmed, no discrepancy — plus 15 new CLI checks).

What was fixed for this baseline: `Makefile` gains the invariant-mandated
`-Wpedantic` (was missing); `src/main.c` i64 printing no longer uses
`PRId64`, which `-Wpedantic` rejects on MinGW (`ms_printf` `I64` modifier),
rendering decimals with plain `%s` instead — covered by `tests/test_cli.c`.
`make test` now prints per-suite `RESULT` lines plus a final
`TOTAL: X passed, Y failed` and exits non-zero on any failure (verified
with a missing-suite negative check). The empty leftover `-p/` directory
was deleted. `Plan.md` (referenced above and in Layout) is absent from the
source dump, so it is not tracked; `PROJECT_ARCHITECTURE.md` (forensic
analysis found in the dump) is tracked.

Still missing as of this baseline (float arithmetic, br_table/select,
globals, data, start, tables, and indirect calls since closed, see above):
module validation
beyond header/sections/imports, fuzzing, CI. Known deviation: a function
body's trailing `end` byte traps (`WASM_TRAP_CONTROL_STACK_UNDERFLOW` —
no implicit function-level block), so tool-produced modules must be emitted
without it, as `demo/add.wasm` already is. Older capacity constants are not
yet `#ifndef`-guarded (invariant I5 gap, pre-existing); the new
`WASM_MAX_BR_TABLE_LABELS` is guarded and `-D` overridable.

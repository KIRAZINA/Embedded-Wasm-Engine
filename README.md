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
  call stack, and one shared structured-control pool (bounded nesting).
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

- Blocks carry void or i32 results; functions return at most one value.
- Fixed capacities (see Limits); oversized modules trap cleanly instead of
  allocating.
- CLI supports `--arg i32:5`, `--arg i64:-1`, `--arg f32:3.5`, and
  `--arg f64:2e10`; hexadecimal float values use raw bits, e.g.
  `f32:0x3f800000` or `f64:0x3ff0000000000000`.
- Table imports are rejected (`WASM_ERR_IMPORTED_TABLES_UNSUPPORTED`):
  the host has no table concept, so a module importing a table cannot
  link. Only one defined table is supported, and only the first data /
  element section is applied if several are present.
- Memory imports are rejected (`WASM_ERR_IMPORTED_MEMORIES_UNSUPPORTED`):
  the host has no memory object to bind, so a module importing a memory
  cannot link (accepting it would silently misdirect `memory.size`,
  `memory.grow`, and data segments). Memory exports are accepted but
  inert. At most one table and at most one memory; duplicates are
  malformed, never silently shadowed.
- Function bodies must end with an explicit function-level `end` (`0x0B`)
  byte (MVP strictness — there is no implicit-end deviation); bodies that
  fall off the code window are rejected as malformed. At runtime a
  function-level `end` is a no-op marker and never traps.

## Layout

```
.
├── Makefile           # all / test / test1..14 / runner / clean (POSIX shell)
├── include/           # leb128.h, wasm_types.h, wasm_parser.h, wasm_stack.h,
│                      #   wasm_memory.h, wasm_frame.h, wasm_control.h,
│                      #   wasm_vm.h, wasm_host.h
├── src/               # leb128.c, wasm_parser.c, wasm_stack.c, wasm_memory.c,
│                      #   wasm_frame.c, wasm_control.c, wasm_vm.c,
│                      #   wasm_host.c, main.c (CLI runner)
├── tests/             # harness.h + test_phase1.c … test_phase14.c, test_cli.c
├── fuzz/              # the fuzzing pipeline and its drivers (zero allocation)
├── realworld/         # real clang --target=wasm32 modules + the native comparison
├── tools/             # spec harness, size report, baseline checks, generators
├── docs/              # SPEC_STATUS.md, OPCODE_MATRIX.md (generated)
├── demo/              # add.wasm (main(a,b)->a+b), print.wasm (host-callback demo),
│                      #   fadd.wasm (main()->f32 { 1.5 + 2.5 }),
│                      #   dispatch.wasm (main(i)->i32 dispatch via br_table),
│                      #   dispatch2.wasm (add/sub chosen at runtime via call_indirect),
│                      #   hello.wasm (data "Hello" + print loop via env.print_i32)
└── build/             # wasm_runner + 15 test binaries (generated)
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
# [load] 42 bytes from 'demo\add.wasm'
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
# [load] 46 bytes from 'demo\fadd.wasm'
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

The interpreter dispatches the **172 canonical MVP opcodes** — every opcode of
the MVP instruction set, including the float memory loads/stores, the
conversions and the reinterpretations. The generated, per-opcode status
table is [`docs/OPCODE_MATRIX.md`](docs/OPCODE_MATRIX.md): it is
produced from the engine sources themselves (`tools/opcode_matrix.py`)
and runs as part of `tests/test_phase12.c`, so it cannot drift from the
code.

| Group | Coverage |
|---|---|
| Control, calls, branches, select | structured control, direct/indirect calls, `br_table` |
| Constants, locals, globals | four value types, mutable globals |
| i32 / i64 integer | comparisons, arithmetic, bitwise, shifts, memory load/store |
| f32 / f64 floating point | arithmetic, comparisons, rounding, min/max, memory load/store |
| Conversions and reinterpretation | truncation, conversion, promote/demote, bit reinterpretation |

Not implemented: threads/atomics, SIMD, GC, multi-value blocks or returns,
bulk-memory operations, reference types, sign-extension operations, tail
calls, multi-memory, and exception handling. These omissions are deliberate
MVP subset limits and are not claims of full modern-WebAssembly coverage.

### Spec compliance

The measured conformance state is in
[`docs/SPEC_STATUS.md`](docs/SPEC_STATUS.md): 66 official spec suites
on a **pinned** corpus, 19,054 assertions, **15,906 passing (83.5 %)**,
with every failure classified. The numeric baseline is checked in CI
(`tools/check_spec_baseline.py`), so a regression fails the build.

### Spec deviations

The engine is an MVP interpreter with fixed capacity and single-instance
linking. The documented deviations are:

| Deviation | Effect |
|---|---|
| Fixed capacity limits | Functions, table size, globals, data segments, control nesting (1024) and branch-scan cost are compile-time limits; a module that exceeds one is rejected. See [`LIMITS.md`](LIMITS.md). |
| No multi-module linking | The engine links one module into one instance; `register` and cross-module tests are not supported. This is the cause of most remaining spec failures. |
| No post-MVP features | Sign extension, bulk memory, non-trapping conversions, multi-value, reference types, SIMD and tail calls are rejected as invalid. |
| Host `libm` for `sqrt`/conversions | The float suites pass completely on the pinned corpus; the rounding edge case that was open earlier is not reproducible there any more. |
| No UTF-8 name validation | Module and export names are compared byte-wise. |

The full list with the reason for each entry is in
[`tests/spec/DEVIATIONS.md`](tests/spec/DEVIATIONS.md).

## Quickstart

Requirements are a C11 compiler, Make, and the standard C library plus `libm`.
No package manager, runtime library, or Python dependency is needed for the
core build.

```sh
make
make test
make no-alloc-check
make fuzz-smoke SEED=7 ITER=1000000
make sanitizers          # asan + ubsan build, unit suite and spec smoke
make fuzz                 # libFuzzer -> AFL++ -> deterministic loop
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

Every module is statically validated before linking; validation failure
exits 1 with `error: validation: NAME` on stderr. `--no-validate` skips
that static check — the module is then linked and executed as-is, so it
may trap at runtime (or fail to link). Use it to reproduce a runtime
failure of a module the validator rejects, and only then: the static
check is the safer default.

The CLI reads at most **1 MiB** (`RUNNER_FILE_MAX`) and provides a
one-page linear-memory arena; a larger file is a usage error, and a
module whose data does not fit the arena fails with a memory error. The
embedded targets use their own, smaller budget (4 KiB) — see
[`LIMITS.md`](LIMITS.md).

## Conformance and fuzz tooling

`make fuzz-smoke SEED=1 ITER=1000000` runs the deterministic in-repo smoke
fuzzer.

`make asan`, `make ubsan` and `make sanitizers` build the runner, every
test and the fuzz harnesses with AddressSanitizer (and/or
UndefinedBehaviorSanitizer, `-fno-sanitize-recover=all`) into their own
build tree, then run the full unit suite and three representative spec
suites (`f32`, `memory_trap`, `traps`) through a sanitized runner. The
sanitizer runtimes must be provided by the selected C toolchain; clang is
preferred when installed, gcc otherwise, and `CC=clang` / `CC=gcc` forces
one.

`make fuzz` picks a fuzzing backend automatically and prints it:

1. **libFuzzer** (`clang -fsanitize=fuzzer`, target `fuzz-libfuzzer`),
2. **AFL++** (`afl-clang-fast`, target `fuzz-afl`),
3. **deterministic seeded loop** (`fuzz-loop`): a mutation driver over the
   valid corpus that prints its seed, so a run can be replayed exactly.

`make fuzz-mutate` runs the Python corpus driver (`fuzz/mutate.py`): it
harvests `demo/*.wasm` plus the official spec corpus, generates mutants
(bit flips, truncation, section-size corruption, random byte overwrite,
header corruption), replays them through the C pipeline and minimizes any
crash it finds. `make no-alloc-check` verifies mechanically that no engine,
test or fuzz source calls a dynamic allocation API.

The fuzzers and the shared pipeline live in `fuzz/`; all of their buffers
are static, so the fuzz drivers allocate nothing either.

`make realworld` is the readiness proof: three C programs (integer sort,
8x8 f32 matrix multiply, CRC-32 with a lookup table in the data section)
are compiled natively and with `clang --target=wasm32`, and the engine's
output must match the native one byte for byte. The emitted modules are
checked to be MVP-only (no sign extension, bulk memory, non-trapping
conversions, multi-value, reference types, SIMD, mutable globals or
imports). `make realworld-verify` runs the committed modules through the
engine only, without a wasm toolchain.

The wasm build needs `clang` with the `wasm32` target and `wasm-ld`.
Where the distribution packages are not installed system-wide, they can
be unpacked into a local prefix without root:

```sh
sh tools/setup_wasm_toolchain.sh          # lld + wabt + binaryen
export PATH="$HOME/tools/root/usr/bin:$HOME/tools/root/usr/lib/llvm-18/bin:$PATH"
```

The programs live in `realworld/`; `realworld/expected.txt` holds the
native outputs, and `tools/gen_realworld_data.py` regenerates the matrix
and CRC-table data (with a CRC-32 self-check against the canonical
vector `crc32("123456789") = 0xCBF43926`).

CI gates: `make spec` (and `make spec-summary`) run the official spec
suite and fail only on a regression against the numeric baseline in
[`tests/spec/DEVIATIONS.md`](tests/spec/DEVIATIONS.md);
`make spec-report` keeps the raw runner behaviour (exit 1 on any
unclassified failure). `make size-check` fails when the engine's
`.text` grew by more than 5 % against `size_baseline.txt`.

### Extended fuzzing runs

The committed campaign reached the execution minimum (5,000,000 libFuzzer
executions plus 2,000,000 mutation-loop iterations under ASan+UBSan, no
crashes). A longer campaign runs unattended with the same harness:

```sh
# 24 h, four parallel libFuzzer jobs
make CC=clang fuzz-build fuzz-corpus
mkdir -p build/fuzz/extended
for i in 1 2 3 4; do
    mkdir -p build/fuzz/extended/job$i
    cp build/fuzz/corpus/* build/fuzz/extended/job$i/ 2>/dev/null || true
    nohup ./build/fuzz/libfuzzer_parse build/fuzz/extended/job$i \
        -max_total_time=86400 -max_len=65536 -timeout=25 \
        -rss_limit_mb=4096 -print_final_stats=1 \
        -artifact_prefix=build/fuzz/extended/job$i/crash- \
        > build/fuzz/extended/job$i/fuzz.log 2>&1 &
done
wait
```

Use `-max_total_time=172800` for 48 h. The AFL++ backend
(`make CC=clang build/fuzz/afl_parse`, then `afl-fuzz -i build/fuzz/corpus
-o build/fuzz/afl-out -- build/fuzz/afl_parse`) and the deterministic
mutation loop (`make CC=clang build/fuzz/loop_driver_san`, then
`./build/fuzz/loop_driver_san 42 100000000 demo/*.wasm`, which prints its
seed so a run can be replayed) are the other two backends. A crash is
minimized by `fuzz/mutate.py` and belongs in `tests/regression/`.

The spec harness is `tools/run_spec_suite.py` and uses only Python 3's standard
library. The user supplies `.wast` conversions; no binaries are bundled. A
typical preparation command is:

`wast2json --spec path/to/test.wast -o path/to/test.json`

Then run `python tools/run_spec_suite.py path/to/test.json --runner build/wasm_runner`.
The curated default names are documented in the script; `--filter=NAME` limits
the run. Unsupported JSON command forms are counted as skips and reported.

Conformance evidence in this checkout is bounded by the absence of bundled
`.json` inputs: the curated run reports 0 executed assertions and 23 skipped
files until the user supplies conversions. The measured numbers are in
[`docs/SPEC_STATUS.md`](docs/SPEC_STATUS.md), the baseline in
[`tests/spec/DEVIATIONS.md`](tests/spec/DEVIATIONS.md). Sanitizer
execution requires a compiler runtime providing ASan and UBSan; the
reference MinGW host does not provide those libraries, while the
committed Ubuntu CI jobs use native GCC and Clang sanitizer runtimes.

```
# [load] 126 bytes from 'demo\hello.wasm'
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
# [dump] size: 42 bytes
# [dump] sections:
# [dump]   id=1 name=Type header_off=8 payload_off=10 payload_len=7
# [dump]   id=3 name=Function header_off=17 payload_off=19 payload_len=2
# [dump]   id=7 name=Export header_off=21 payload_off=23 payload_len=8
# [dump]   id=10 name=Code header_off=31 payload_off=33 payload_len=9
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
wasm_vm_t=80960 bytes
wasm_host_registry_t=3584 bytes
wasm_memory_t=24 bytes
engine_static_footprint=84568 bytes
```

(The structured-control pool shared by all call frames made the VM
~24 KiB *smaller* than before while raising the accepted nesting depth
from 32 to 1024 — see `LIMITS.md`.)

For ESP32 and ESP32-C3, see [targets/esp32/README.md](targets/esp32/README.md)
for the exact ESP-IDF component setup and `idf.py` commands. The component
wrapper links the same zero-allocation core; application code supplies its
static memory arena and host bindings.

## Testing

Standalone suites on the shared harness in `tests/harness.h` (test
registration, counters, assert macros with file:line, non-zero exit on
failure; no test framework, no allocation). Current status:
**2036/2036 passing, 0 failed** (`make test` prints
`TOTAL: 2036 passed, 0 failed`; `HARNESS_VERBOSE=1` prints every
passing assertion as well).

| Suite | Coverage | Checks |
|---|---|---|
| `test_phase1` | LEB128 vectors/overflow/truncation, header + section parsing | 58 |
| `test_phase2` | Stack push/pop/peek, overflow/underflow, LE memory round-trips, OOB + wraparound, page grow | 100 |
| `test_phase3` | Frames, call stack, enter/exit, locals, height restoration | 137 |
| `test_phase4` | i32 ALU, div-by-zero, `MIN/-1`, shifts, comparisons, load/store | 379 |
| `test_phase5` | blocks/loops/branches, factorial, `call`/`return` interop, control-pool bounds | 80 |
| `test_phase6` | Host callbacks, export lookup, import binding, instantiation, i64 ALU, CLI `--dump` output | 164 |
| `test_cli` | End-to-end runner runs of hand-built `()->i64` modules, exact `result:` rendering incl. `INT64_MIN` | 31 |
| `test_phase10` | Table/element validation, indirect dispatch/bounds/unset/signature matrix, host indirect, recursion, overwrite/reset/scanner, CLI dispatch2 run | 150 |
| `test_phase9` | Global get/set/immutability/init forms/imports, data placement/bounds/caps, start run-once/rejects/host-start, register validation, CLI hello run | 200 |
| `test_phase8` | br_table all-arms/default/unsigned-index/loop-target/valued-carry/malformed/scanner, select over 4 types incl. NaN/mismatch/underflow, CLI dispatch run | 118 |
| `test_phase7` | f32/f64 const/arith/sqrt/min-max/abs-neg-copysign/round/nearest/cmp, all 8 trunc boundaries, converts, reinterprets, NaN propagation, memory NaN round-trips, CLI f32 run + dump | 371 |
| `test_phase11` | static module validation corpus | 205 |
| `test_phase12` | conformance/fuzz tooling helpers, opcode-matrix generator | 8 |
| `test_phase13` | control-nesting limit (validator + interpreter), shared control pool, branch-scan budget | 23 |
| `test_phase14` | real clang output (qsort, f32 matmul, CRC-32) and the typed-local regression | 12 |

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
  and one shared control pool (1024 entries) that every call frame owns
  a window of. Overflow/underflow are trap values, never crashes.
- `wasm_vm`: fetch/decode/execute loop; typed operand pops; subtractions-form
  bounds checks (`offset > total - size`, computed in 64 bits) defeat
  wraparound attacks.
- `wasm_host`: 16-slot native callback registry; import→host binding with
  functype verification; export lookup; whole-module instantiation into the
  VM's 32-slot function table (imports first, per MVP index space).
- `main`: ≤1 MiB file buffer, 1-page arena, 10M-step watchdog, branch-scan
  budget.

## Fixed limits

| Resource | Cap | Overflow behavior |
|---|---|---|
| Operand stack | 1024 values | `WASM_TRAP_STACK_OVERFLOW` |
| Call stack | 64 frames | `WASM_TRAP_CALL_STACK_OVERFLOW` |
| Locals per frame | 32 | `WASM_TRAP_INVALID_LOCAL_INDEX` |
| Control nesting | 1024 blocks, shared by the whole call stack (`WASM_MAX_CTRL_NESTING`, `-D` overridable) | `WASM_TRAP_CONTROL_STACK_OVERFLOW` |
| VM functions | 32 | `WASM_TRAP_INVALID_ARGUMENT` |
| Host callbacks | 16 (31-char names) | `WASM_TRAP_INVALID_ARGUMENT` |
| CLI input file | 1 MiB (`RUNNER_FILE_MAX`; embedded builds override it to 4 KiB) | usage error |
| Branch labels | open depth | `WASM_TRAP_INVALID_BLOCK_LABEL` |
| Branch-scan cost | 1024 scanned bytes = 1 watchdog step (`WASM_SCAN_BYTES_PER_STEP`, `-D` overridable) | budget exhausted |
| Indirect table | 128 (`WASM_MAX_TABLE_SIZE`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| Globals (imported + defined) | 32 (`WASM_MAX_GLOBALS`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| Host-provided globals | 16 (`WASM_MAX_HOST_GLOBALS`, `-D` overridable) | `WASM_TRAP_INVALID_ARGUMENT` |
| Data segments | 32 (`WASM_MAX_DATA_SEGMENTS`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| br_table labels | 256 (`WASM_MAX_BR_TABLE_LABELS`, `-D` overridable) | `WASM_TRAP_INVALID_ARGUMENT` |
| Validation value stack | 1024 (`WASM_VALIDATE_STACK_MAX`, `-D` overridable) | `WASM_ERR_SECTION_OVERFLOW` |
| Validation control depth | 1024 (`WASM_MAX_CTRL_NESTING`, shared with execution) | `WASM_ERR_INVALID_BLOCK` |

The exhaustive list, including the embedded overrides, is in
[`LIMITS.md`](LIMITS.md).

Fuzz-smoke defaults are also overrideable at compile time: `FUZZ_MAX_BYTES`
256, `FUZZ_PAGE_COUNT` 1, and `FUZZ_STEP_BUDGET` 10.

## Memory footprint

Static engine cost (measured by `make size-report`):

```
sizeof(vm)=80960  sizeof(registry)=3584  sizeof(mem)=24
engine = 84568 bytes  (limit 131072)  →  < 128 KiB ✓
```

The engine library itself (`.text`, `-Os`, freestanding) measures
49,010 bytes and has no static data; `make size-check` compares it
against the committed `size_baseline.txt` and fails when it grew by
more than 5 %.

Guest linear memory and the module image live in application-owned buffers
the engine only borrows, so an embedded target pays the ~82.6 KiB engine
cost plus whatever arena it provisions.

## Feature status

The engine implements the **complete MVP section set** (type, import,
function, table, memory, global, export, start, element, code, data) and
the full MVP instruction set:

| Area | Status | Proven by |
|------|--------|-----------|
| Host integration (callbacks, imports, exports) | implemented | `test_phase6` + `demo/print.wasm` |
| f32/f64 arithmetic, conversions, reinterprets | implemented | `test_phase7` + `demo/fadd.wasm` |
| Globals, data segments, start function | implemented | `test_phase9` + `demo/hello.wasm` |
| Tables, element segments, `call_indirect` | implemented | `test_phase10` + `demo/dispatch2.wasm` |
| `br_table`, `select` | implemented | `test_phase8` + `demo/dispatch.wasm` |
| Static validation of every executable section | implemented | `test_phase11` |
| Bounded nesting and branch-scan cost | implemented | `test_phase13` |
| Real compiler output (clang `--target=wasm32`) | verified byte for byte against the native build | `realworld/` + `test_phase14` |

Deliberately still open: multi-module linking, post-MVP proposals
(multi-value, bulk memory, reference types, SIMD), UTF-8 name
validation, and physical deployment to an ESP32/STM32 target (GPIO/LED
bindings would register through the existing host table).

# Embedded-Wasm-Engine

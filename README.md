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
- **Structured control flow** — `block`/`loop`/`if`/`else`/`end`,
  `br`/`br_if` with relative labels, `return`, `call`, `local.get/set/tee`.
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

- MVP integer subset: f32/f64 values can be stored, loaded, and passed
  through, but there are **no float arithmetic opcodes**.
- Blocks carry void or i32 results; functions return at most one value.
- Fixed capacities (see Limits); oversized modules trap cleanly instead of
  allocating.
- No float parsing of CLI args (i32 only); no `br_table`, SIMD, threads,
  or GC extensions.

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
├── tests/             # test_phase1.c … test_phase6.c (assert-based, no framework)
├── demo/              # add.wasm (main(a,b)->a+b), print.wasm (host-callback demo)
└── build/             # wasm_runner + 6 test binaries (generated)
```

## Requirements

- A C11 compiler (gcc/clang). POSIX `make` + shell for the Makefile.
- No libraries beyond the C standard library.

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

## Testing

Standalone `assert`-style suites, no test framework. Current status:
**681/681 passing, 0 failed** (`make test` prints `TOTAL: 681 passed, 0 failed`).

| Suite | Coverage | Checks |
|---|---|---|
| `test_phase1` | LEB128 vectors/overflow/truncation, header + section parsing | 58 |
| `test_phase2` | Stack push/pop/peek, overflow/underflow, LE memory round-trips, OOB + wraparound, page grow | 100 |
| `test_phase3` | Frames, call stack, enter/exit, locals, height restoration | 137 |
| `test_phase4` | i32 ALU, div-by-zero, `MIN/-1`, shifts, comparisons, load/store | 137 |
| `test_phase5` | blocks/loops/branches, factorial, `call`/`return` interop | 74 |
| `test_phase6` | Host callbacks, export lookup, import binding, instantiation, i64 ALU, CLI `--dump` output | 160 |
| `test_cli` | End-to-end runner runs of hand-built `()->i64` modules, exact `result:` rendering incl. `INT64_MIN` | 15 |

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

## Memory footprint

Static engine cost (LP64, asserted in `test_phase6`):

```
sizeof(vm)=102424  sizeof(registry)=1408  sizeof(mem)=24
engine = 103984 bytes  (limit 131072)  →  < 128 KiB ✓
```

Guest linear memory and the module image live in application-owned buffers
the engine only borrows, so an embedded target pays the ~101.6 KiB engine
cost plus whatever arena it provisions.

## Roadmap status vs `Plan.md`

Phases 1–4 of the plan are implemented (delivered as six incremental phases
here); host integration works through the generic callback registry, proven
with `print_i32`. Deliberately still open: f32/f64 arithmetic opcodes,
multi-value blocks/functions, and physical deployment to an ESP32/STM32
target (GPIO/LED bindings would register through the existing host table).

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

Still missing as of this baseline: f32/f64 arithmetic opcodes, `br_table`,
`select`, globals, tables, data section, start section, module validation
beyond header/sections/imports, fuzzing, CI. Known deviation: a function
body's trailing `end` byte traps (`WASM_TRAP_CONTROL_STACK_UNDERFLOW` —
no implicit function-level block), so tool-produced modules must be emitted
without it, as `demo/add.wasm` already is. Fixed capacity constants are not
yet `#ifndef`-guarded (invariant I5 gap, pre-existing).

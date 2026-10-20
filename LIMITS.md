# Engine capacity limits (fixed, zero allocation)

Every limit is a compile-time constant (`-D` flag or `#define`). There is
no dynamic growth: a module that exceeds a limit is rejected or traps,
never allocated around.

## Capacity summary

| Resource | Limit | Source |
|----------|------:|--------|
| Module file | 1 MiB (desktop CLI); 4 KiB in embedded builds | `RUNNER_FILE_MAX` |
| Functions | 512 | `WASM_MAX_FUNCTIONS` |
| Table size | 1024 | `WASM_MAX_TABLE_SIZE` |
| Globals | 32 | `MAX_GLOBALS` |
| Data segments | 32 | `MAX_DATA` |
| Memory pages | 2 (128 KiB) via the CLI arena; larger = `MEM_ARENA` | `RUNNER_ARENA_PAGES` |
| Control nesting | 1024 | `WASM_MAX_CTRL_NESTING` |
| Branch-scan cost | 1024 scanned bytes = 1 watchdog step | `WASM_SCAN_BYTES_PER_STEP` |
| Call stack | 64 frames | `WASM_MAX_CALL_STACK_DEPTH` |
| Multi-module linking | single instance; `register` / multi-module links are not supported | — |

Any spec case exceeding these is a `CAPACITY_LIMIT`; the conformance
consequences are listed in
[`tests/spec/DEVIATIONS.md`](tests/spec/DEVIATIONS.md).

## All compile-time constants

| Constant | Default | What it bounds | Static cost | Override |
|----------|--------:|----------------|-------------|----------|
| `WASM_MAX_FUNCTIONS` | 32 | imported + defined functions | 32 descriptors per VM | `-DWASM_MAX_FUNCTIONS=64u` |
| `WASM_MAX_TABLE_SIZE` | 128 | indirect-call table entries | table storage | `-DWASM_MAX_TABLE_SIZE=1024u` |
| `MAX_GLOBALS` | 32 | globals (imported + defined) | global storage | `-DMAX_GLOBALS=64u` |
| `MAX_HOST_GLOBALS` | 16 | host-provided globals | global storage | `-DMAX_HOST_GLOBALS=32u` |
| `MAX_DATA` | 32 | data segments | — | `-DMAX_DATA=64u` |
| `WASM_MAX_LOCALS_PER_FRAME` | 32 | locals per frame | 512 B per frame | `-DWASM_MAX_LOCALS_PER_FRAME=64u` |
| `WASM_VALIDATE_STACK_MAX` | 1024 | validation value stack | 1 KiB | `-DWASM_VALIDATE_STACK_MAX=2048u` |
| `WASM_MAX_CTRL_NESTING` | 1024 | live control frames across the whole call stack | ~24 KiB once | `-DWASM_MAX_CTRL_NESTING=16u` |
| `WASM_SCAN_BYTES_PER_STEP` | 1024 | bytes scanned while resolving a branch before one watchdog step is charged | none | `-DWASM_SCAN_BYTES_PER_STEP=256u` |
| `WASM_MAX_BR_TABLE_LABELS` | 256 | `br_table` labels | none | `-DWASM_MAX_BR_TABLE_LABELS=512u` |
| `RUNNER_FILE_MAX` | 1048576 (1 MiB) | CLI input file | two CLI static buffers | `-DRUNNER_FILE_MAX=131072u` |
| `RUNNER_ARENA_PAGES` | 1 | CLI linear-memory arena | 64 KiB per page | `-DRUNNER_ARENA_PAGES=2u` |
| `RUNNER_STEP_BUDGET` | 10000000 | CLI execution watchdog | none | `-DRUNNER_STEP_BUDGET=1000000u` |
| `SESSION_ARENA_PAGES` | 64 | `--session` linear-memory arena | 64 KiB per page | `-DSESSION_ARENA_PAGES=8u` |
| `FUZZ_MAX_BYTES` | 256 | smoke-fuzzer input size | 256 B | `-DFUZZ_MAX_BYTES=1024u` |
| `FUZZ_PAGE_COUNT` | 1 | smoke-fuzzer memory pages | 64 KiB | `-DFUZZ_PAGE_COUNT=2u` |
| `FUZZ_STEP_BUDGET` | 10 | smoke-fuzzer step budget | none | `-DFUZZ_STEP_BUDGET=100u` |

## Control nesting (`WASM_MAX_CTRL_NESTING`, default 1024)

The number of live structured-control frames (blocks/loops/ifs) is
bounded **globally**: one fixed-capacity pool (`wasm_control_pool_t`,
embedded in `wasm_vm_t`) backs every call frame, so the pool costs
`WASM_MAX_CTRL_NESTING * 24 B` (~24 KiB) once instead of per frame.

* Validation: a function body nested deeper than the limit is rejected
  with `WASM_ERR_INVALID_BLOCK` before anything is executed. The
  validator is iterative (recursion depth 0), so deep nesting costs
  time proportional to the module size, never host stack.
* Execution: a module that slips past validation (e.g. `--no-validate`)
  traps with `WASM_TRAP_CONTROL_STACK_OVERFLOW` instead of growing any
  structure.
* The bound is global, so a *combination* of a deeply nested caller and
  a deeply nested callee can exceed it even though each function is
  within the limit. Such a program traps at run time
  (`WASM_TRAP_CONTROL_STACK_OVERFLOW`) rather than being rejected by the
  validator; this is a capacity limit, not a semantic difference.

Override with `-DWASM_MAX_CTRL_NESTING=<n>` (also changes the pool size
inside `wasm_vm_t`).

## Branch-scan budget (`WASM_SCAN_BYTES_PER_STEP`, default 1024)

Resolving the target of a branch scans the code forward until the
matching `END`, so a single VM step could inspect a whole function body
(one huge block plus a branch to a distant `END`). The VM counts the
scanned bytes (`wasm_vm.scan_bytes`) and the host turns every
`WASM_SCAN_BYTES_PER_STEP` bytes into one extra step of its own budget
(`wasm_vm_scan_charge()`).

* Only the host's watchdog budget changes; program semantics are
  unchanged.
* The CLI applies the charge in both the one-shot and the session
  (`LOAD`/`INVOKE`) drivers; the fuzz pipeline does the same.
* Override with `-DWASM_SCAN_BYTES_PER_STEP=<n>`.

## Reference footprint

On the reference build, `make size-report` measured:

```text
wasm_vm_t=80960 bytes
wasm_host_registry_t=3584 bytes
wasm_memory_t=24 bytes
engine_static_footprint=84568 bytes
```

The engine library itself (`.text`, `-Os`, freestanding) measures
49,010 bytes and has no static data; `make size-check` compares it
against the committed `size_baseline.txt` and fails when it grew by
more than 5 %. `tools/size_report.c` contains a compile-time assertion
that `wasm_vm_t` fits the documented 112 KiB STM32F4 VM budget. The
CLI and the guest memory arena are application-level storage in
addition to the engine object.

## Embedded overrides

The desktop CLI defaults are sized for a host process. Embedded targets
override the budgets explicitly:

| Target | Override | How |
|--------|----------|-----|
| STM32 (Cortex-M4) | `RUNNER_FILE_MAX=4096u`, `STM32F4_VM_BUDGET_BYTES=114688u` | `ARM_CFLAGS` in the Makefile (`make arm-cross`) |
| ESP32 / ESP32-C3 | `RUNNER_FILE_MAX=4096u`, `STM32F4_VM_BUDGET_BYTES=114688u` | `COMPILE_DEFINITIONS` in `targets/esp32/CMakeLists.txt` |
| Any host build | `RUNNER_FILE_MAX` (1 MiB default) | `src/main.c` (`-DRUNNER_FILE_MAX=...`) |

Neither embedded target links `src/main.c` (the CLI); the file budget is
still pinned so that a target which does link it stays inside the
documented footprint. `make embedded-check` builds the Cortex-M demo with
the embedded flags using the host compiler, so the embedded configuration
is verified even without `arm-none-eabi-gcc` installed.

## Zero dynamic allocation

Verified mechanically: `make no-alloc-check`
(`tools/check_no_alloc.sh`) greps every `.c`/`.h` under `src/`,
`include/`, `tests/`, `fuzz/`, `tools/` and `targets/` for allocation
calls (`malloc`, `calloc`, `realloc`, `alloca`, `strdup`, `free`
followed by `(`). Prose mentions in comments do not count.

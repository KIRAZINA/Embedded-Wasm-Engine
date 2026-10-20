# Engine Capacity Limits (fixed, zero allocation)

Documented here per invariant 4. All values are compile-time constants
(`-D` flags or `#define`). No dynamic growth; failure = `CAPACITY_LIMIT`.

| Resource        | Limit   | Source (compile or source) |
|-----------------|---------|----------------------------|
| Module file     | 1 MiB (desktop CLI); 4 KiB in embedded builds | `RUNNER_FILE_MAX` (runner) |
| Functions       | 512     | `WASM_MAX_FUNCTIONS=512`   |
| Table size      | 1024    | `WASM_MAX_TABLE_SIZE=1024` |
| Globals         | 32      | `MAX_GLOBALS = 32` (script)|
| Data segments   | 32      | `MAX_DATA = 32` (script)   |
| Memory pages    | 2 (128 KiB) via CLI arena; larger = `MEM_ARENA` |
| Control nesting | 1024    | `WASM_MAX_CTRL_NESTING=1024` |
| Branch-scan cost | 1024 scanned bytes = 1 watchdog step | `WASM_SCAN_BYTES_PER_STEP=1024` |
| Call stack      | `WASM_MAX_CALL_STACK_DEPTH` (fixed, source) |
| Multi-module linking | Single instance; `register` / multi-module links classify as `KNOWN_DEVIATION` or `ENGINE_BUG` depending on failure |

Any spec case exceeding these is a `CAPACITY_LIMIT`, listed in `DEVIATIONS.md`.

The exhaustive list of compile-time constants (with the embedded
overrides) lives in [docs/LIMITS.md](docs/LIMITS.md).

## Control nesting (`WASM_MAX_CTRL_NESTING`, default 1024)

Phase 4 hardening. The number of live structured-control frames
(blocks/loops/ifs) is bounded **globally**: one fixed-capacity pool
(`wasm_control_pool_t`, embedded in `wasm_vm_t`) backs every call frame,
so the pool costs `WASM_MAX_CTRL_NESTING * 24 B` (~24 KiB) once instead
of per frame.

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

Phase 4 hardening. Resolving the target of a branch scans the code
forward until the matching `END`, so a single VM step could inspect a
whole function body (one huge block plus a branch to a distant `END`).
The VM counts the scanned bytes (`wasm_vm.scan_bytes`) and the host
turns every `WASM_SCAN_BYTES_PER_STEP` bytes into one extra step of its
own budget (`wasm_vm_scan_charge()`).

* Only the host's watchdog budget changes; program semantics are
  unchanged.
* The CLI applies the charge in both the one-shot and the session
  (`LOAD`/`INVOKE`) drivers; the fuzz pipeline does the same.
* Override with `-DWASM_SCAN_BYTES_PER_STEP=<n>`.

## Zero dynamic allocation

Verified mechanically: `make no-alloc-check`
(`tools/check_no_alloc.sh`) greps every `.c`/`.h` under `src/`,
`include/`, `tests/`, `fuzz/`, `tools/` and `targets/` for allocation
calls (`malloc`, `calloc`, `realloc`, `alloca`, `strdup`, `free`
followed by `(`). Prose mentions in comments do not count.

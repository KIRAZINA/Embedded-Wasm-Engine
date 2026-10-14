# Engine Capacity Limits (fixed, zero allocation)

Documented here per invariant 4. All values are compile-time constants
(`-D` flags or `#define`). No dynamic growth; failure = `CAPACITY_LIMIT`.

| Resource        | Limit   | Source (compile or source) |
|-----------------|---------|----------------------------|
| Module file     | 64 KiB  | `FILE_MAX = 65536` (runner)|
| Functions       | 512     | `WASM_MAX_FUNCTIONS=512`   |
| Table size      | 1024    | `WASM_MAX_TABLE_SIZE=1024` |
| Globals         | 32      | `MAX_GLOBALS = 32` (script)|
| Data segments   | 32      | `MAX_DATA = 32` (script)   |
| Memory pages    | 2 (128 KiB) via CLI arena; larger = `MEM_ARENA` |
| Control stack   | `MAX_CONTROL_DEPTH` (fixed, source `wasm_vm.c`) |
| Call stack      | `WASM_MAX_CALL_STACK_DEPTH` (fixed, source) |
| Multi-module linking | Single instance; `register` / multi-module links classify as `KNOWN_DEVIATION` or `ENGINE_BUG` depending on failure |

Any spec case exceeding these is a `CAPACITY_LIMIT`, listed in `DEVIATIONS.md`.

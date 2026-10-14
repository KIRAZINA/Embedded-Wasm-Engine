# Known Deviations — Official Spec Suite (Phase 3)

Every entry is explicitly classified and justified. Silently ignored = violation.

## Engine-subset / post-MVP exclusions (expected failures, not bugs)
- Bulk-memory, multi-memory, exception-handling, GC, reference-types, SIMD,
  threads, tail-calls, multi-value, memory64: excluded by MVP opcode set.
  Any assertion involving them is `KNOWN_DEVIATION:post-mvp`.
- `utf8-*` name checks: engine validates labels/types but not full UTF-8
  normalization; failures classified `KNOWN_DEVIATION:utf8-names-unchecked`.

## Capacity limits (listed, never silent)
- Module > 64 KiB => `CAPACITY_LIMIT:file-size`.
- Functions > 512 => `CAPACITY_LIMIT:func-count`.
- Table > 1024 => `CAPACITY_LIMIT:table-size`.
- Data segments > 32 => `CAPACITY_LIMIT:data-segments`.
- Memory grow past 2 pages (128 KiB) => `MEM_ARENA` / `CAPACITY_LIMIT`.

## Multi-module / linking
- Engine is single-instance. `register` + cross-module links that require
  separate instances are `KNOWN_DEVIATION:multi-module-link` or
  `ENGINE_BUG:link-failed` if unexpectedly accepted/rejected.
- Only host function `env.print_i32` registered; other import dependencies
  produce `KNOWN_DEVIATION:spectest-host`.

## Float / NaN (fixed in phase3 — documented for traceability)
- Before fix: arithmetic NaN propagated host payload (non-canonical).
  Fixed by `vm_f32_arith_res` / `vm_f64_arith_res`; canonical quiet NaN
  (`0x7FC00000` / `0x7FF8000000000000`) emitted after every arithmetic op.
- Before fix: `sqrt` for f32 used `sqrt` (double); fixed to `sqrtf`.
- Before fix: f32 arithmetic could use double intermediate due to C promotion;
  all float arithmetic variables declared float, operations stay float.
- Signed zero: `min/max/copysign/conversions` handled by bit-pattern helpers;
  `trunc` of negative values near zero preserves sign per IEEE / spec.

## Trap message strings (fixed)
- `indirect call type mismatch` -> `indirect call signature mismatch` (TRAP_MAP).
- `undefined element` / `uninitialized element` / exact `unreachable`
  mapped per `include/wasm_types.h` → `TRAP_MAP`.

## Unfixable / ambiguous (for curator decision — do not hack around)
- [None identified yet; to be filled by triage loop.]

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

## Numeric baseline (CI regression gate)

The numbers below are the reference result of the official spec suite on
the **pinned** corpus: `tools/fetch_spec_tests.sh` checks out upstream
revision `340650f` (the corpus is otherwise a moving target, and the
suite set also depends on the `wast2json` version). 66 suites, 19,054
assertions.

`tools/check_spec_baseline.py` compares a run against these numbers and
fails when the number of passing assertions drops or a failure class
grows. The `corpus_*` lines are a fingerprint: when it does not match
(the converter changed), the pass *rate* is compared with a tolerance
instead, so a corpus change cannot cause a false failure.

```text
corpus_suites = 66
corpus_assertions = 19054
total_pass = 15906
total_fail = 3148
CLASS ENGINE_BUG:mismatch = 128
CLASS ENGINE_BUG:no-trap = 15
CLASS KNOWN_DEVIATION:dead-instance = 2187
CLASS KNOWN_DEVIATION:externref-table = 6
CLASS KNOWN_DEVIATION:multi-table = 1
CLASS KNOWN_DEVIATION:multi-value = 2
CLASS KNOWN_DEVIATION:opcodes(26,d1,d2) = 1
CLASS KNOWN_DEVIATION:opcodes(c0,c1) = 1
CLASS KNOWN_DEVIATION:opcodes(c2,c3,c4) = 1
CLASS KNOWN_DEVIATION:opcodes(d0) = 2
CLASS KNOWN_DEVIATION:opcodes(d0,d2) = 1
CLASS KNOWN_DEVIATION:opcodes(d2) = 1
CLASS KNOWN_DEVIATION:opcodes(fc) = 4
CLASS KNOWN_DEVIATION:spectest-host = 482
CLASS KNOWN_DEVIATION:utf8-names-unchecked = 176
CLASS VALIDATOR_BUG:accepts-assert_invalid = 36
CLASS VALIDATOR_BUG:accepts-assert_malformed = 19
CLASS VALIDATOR_BUG:rejects-valid = 85
```

The remaining failures are the documented gaps above: multi-module
linking (2,187 `dead-instance` assertions — the dominant class),
missing host functions (`spectest-host`), UTF-8 name validation,
post-MVP opcodes and tables. Two classes are **not** covered by a
documented deviation and still need triage: `ENGINE_BUG:mismatch` (128
assertions, mostly in `float_exprs`) and `ENGINE_BUG:no-trap` (15).

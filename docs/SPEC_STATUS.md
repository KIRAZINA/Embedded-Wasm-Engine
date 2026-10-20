# Spec compliance status

The engine implements the **WebAssembly MVP** (core 1.0). This page
distils the measured conformance state; the raw evidence lives in
`PHASE3_REPORT.md` (phase 3), `PHASE4_REPORT.md` (phase 4) and
`PHASE5_REPORT.md` (phase 5).

## How the numbers are produced

```sh
sh tools/fetch_spec_tests.sh          # official corpus -> tests/spec/json
make spec                             # run it
make spec-summary                     # run it and check the baseline
```

The official corpus is converted with `wast2json` from the upstream
`WebAssembly/spec` repository (65 suites, ~20,200 assertions). The
harness (`tools/run_spec_suite.py`) classifies every failure, so a
failing assertion is never silently dropped.

| Measurement | Value |
|-------------|-------|
| Suites | 65 |
| Assertions | 20,236 |
| **Passing** | **16,432** (81.2 %) |
| Failing | 3,804 (documented below) |

The numeric baseline lives in `tests/spec/DEVIATIONS.md` and is checked
by `tools/check_spec_baseline.py`; CI fails when the pass count drops
or a failure class grows.

## Failure classes

| Class | Count | Meaning |
|-------|-------|---------|
| `KNOWN_DEVIATION:dead-instance` | 2,729 | The module could not be loaded (usually a multi-module test), so the assertions that follow it cannot run. |
| `ENGINE_BUG:mismatch` | 575 | The module runs, but the result differs from the spec expectation. |
| `VALIDATOR_BUG:rejects-valid` | 130 | A valid module is rejected (validator too strict or a missing feature). |
| `VALIDATOR_BUG:accepts-assert_invalid` | 97 | An invalid module is accepted (validator too lax). |

## Per-area status

| Area | Status | Notes |
|------|--------|-------|
| f32 arithmetic | **complete** | `f32` suite 100 % (2,514 assertions) |
| f64 arithmetic | complete except one libm edge | `f64` 2,513/2,514; the single failure is the host `sqrt` rounding of `0x7fefffffffffffff` (a software sqrt would be needed) |
| i32 / i64 expressions | complete | `int_exprs` 108/108 |
| Memory and traps | **complete** | `memory_trap` 182/182, `store` 68/68, `traps` 36/36 |
| Control flow (block/loop/br/br_table/select) | implemented, multi-module tests fail | the engine implements the semantics; the failing cases are cross-module tests |
| Calls, functions, locals | implemented, multi-module tests fail | same |
| Tables and elements | implemented | the element-section encoding was corrected in phase 4; `elem` went from 42 to 57 passing assertions |
| Globals, data segments, start | implemented | |
| Multi-module linking / `register` | **not implemented** | the engine is single-instance by design; this causes most `dead-instance` failures |
| Post-MVP features | not implemented (by design) | sign extension, bulk memory, non-trapping conversions, multi-value, reference types, SIMD, tail calls |
| Externref / multiple tables | not implemented | |
| UTF-8 name validation | not implemented | module and export names are compared byte-wise |

## Deliberate deviations

The complete list with the reason for each is in
[`tests/spec/DEVIATIONS.md`](../tests/spec/DEVIATIONS.md). The short
version:

* **Capacity limits** are fixed at compile time (see
  [`docs/LIMITS.md`](LIMITS.md)): functions, table size, globals, data
  segments, control nesting, branch-scan cost. A module that exceeds
  one is rejected instead of being executed.
* **Multi-module linking** is not supported; the engine links one
  module into one instance.
* **Post-MVP features** are rejected as invalid (the engine is an MVP
  interpreter).
* **Host libm** is used for `sqrt`, `min`/`max` and the float
  conversions; one documented rounding deviation remains for `f64`.

## What is *not* claimed

The engine is **not** a complete WebAssembly implementation: it is an
MVP interpreter with a fixed capacity, single-instance linking and no
post-MVP proposals. The conformance numbers above are a measurement of
that scope, not of the specification as a whole.

## Opcode coverage

The instruction-by-instruction status is generated from the engine
itself: [`docs/OPCODE_MATRIX.md`](OPCODE_MATRIX.md) lists every MVP
opcode with its implemented behaviour (172 canonical opcodes, all
`OK`). The generator is `tools/opcode_matrix.py` and runs as part of
`tests/test_phase12.c`, so the table cannot drift from the code.

# Spec compliance status

The engine implements the **WebAssembly MVP** (core 1.0). This page
distils the measured conformance state: the numbers, the failure
classes, the per-area status and the deliberate deviations. The numeric
baseline that CI checks against lives in
[`tests/spec/DEVIATIONS.md`](../tests/spec/DEVIATIONS.md).

## How the numbers are produced

```sh
sh tools/fetch_spec_tests.sh          # official corpus -> tests/spec/json
make spec                             # run it
make spec-summary                     # run it and check the baseline
```

The official corpus is converted with `wast2json` from the upstream
`WebAssembly/spec` repository. The upstream revision is **pinned**
(`tools/fetch_spec_tests.sh` checks out `340650f`), because the corpus
is otherwise a moving target and every number below would drift with
it. 66 suites, 19,054 assertions.

The harness (`tools/run_spec_suite.py`) classifies every failure, so a
failing assertion is never silently dropped.

| Measurement | Value |
|-------------|-------|
| Suites | 66 |
| Assertions | 19,054 |
| **Passing** | **15,906** (83.5 %) |
| Failing | 3,148 (16.5 %), every one classified |

The numeric baseline lives in `tests/spec/DEVIATIONS.md` and is checked
by `tools/check_spec_baseline.py`; CI fails when the pass count drops
or a failure class grows. The baseline carries a corpus fingerprint, so
a different `wast2json` version cannot produce a false failure.

## Failure classes

| Class | Count | Share | Meaning |
|-------|------:|------:|---------|
| `KNOWN_DEVIATION:dead-instance` | 2,187 | 69.4 % | The module could not be loaded (multi-module test), so the assertions that follow it cannot run. |
| `KNOWN_DEVIATION:spectest-host` | 482 | 15.3 % | The module needs a host function the engine does not provide. |
| `KNOWN_DEVIATION:utf8-names-unchecked` | 176 | 5.6 % | The engine compares module/export names byte-wise instead of validating UTF-8. |
| `ENGINE_BUG:mismatch` | 128 | 4.1 % | The module runs, but the result differs from the spec expectation. **Not covered by a documented deviation — needs triage.** |
| `VALIDATOR_BUG:rejects-valid` | 85 | 2.7 % | A valid module is rejected. |
| `VALIDATOR_BUG:accepts-assert_invalid` | 36 | 1.1 % | An invalid module is accepted. |
| `VALIDATOR_BUG:accepts-assert_malformed` | 19 | 0.6 % | A malformed module is accepted. |
| `ENGINE_BUG:no-trap` | 15 | 0.5 % | The module should have trapped but did not. **Not covered by a documented deviation.** |
| `KNOWN_DEVIATION:opcodes(...)` | 14 | 0.4 % | Post-MVP opcodes (sign extension, bulk memory, reference types, multi-value) — out of scope for an MVP engine. |
| `KNOWN_DEVIATION:externref-table`, `multi-table`, `multi-value` | 9 | 0.3 % | Post-MVP table and block features. |

**Invalid/malformed acceptance rate:** 55 of 19,054 assertions
(`accepts-assert_invalid` 36 + `accepts-assert_malformed` 19) =
**0.29 %** — the engine accepts a module that the spec marks invalid or
malformed in roughly one out of 350 cases.

**Untriaged engine bugs:** 143 assertions (`ENGINE_BUG:mismatch` 128 +
`ENGINE_BUG:no-trap` 15), i.e. 0.75 % of the suite. They concentrate in
`float_exprs` (72), `func_ptrs` (16), `switch` (7), `memory_size` (7),
`memory_grow` (6), `nop` (5), `load` (4), `func` (4) and `labels` (3).

## Per-area status

| Area | Status | Notes |
|------|--------|-------|
| f32 arithmetic | **complete** | `f32` 2,514/2,514, `f32_bitwise` 364/364, `f32_cmp` 2,407/2,407, `float_misc` 471/471, `float_literals` 179/179 |
| f64 arithmetic | **complete** | `f64` 2,514/2,514, `f64_bitwise` 364/364, `f64_cmp` 2,407/2,407 |
| Float expressions | **incomplete** | `float_exprs` 855/927: 72 `ENGINE_BUG:mismatch` — the largest untriaged group |
| i32 / i64 expressions | complete | `int_exprs` 108/108, `int_literals` 51/51, `const` 778/778, `endianness` 69/69; `i32`/`i64` themselves fail only on multi-module loads and one sign-extension opcode each |
| Memory and traps | **complete** | `memory_trap` 182/182, `store` 68/68, `address` 260/260, `float_memory` 90/90, `memory_redundancy` 8/8, `traps` 36/36; `load`, `memory_size`, `memory_grow` and `data` carry a few untriaged mismatches |
| Control flow (block/loop/br/br_table/select) | implemented, multi-module tests fail | the failures are `dead-instance` (multi-module) plus 3 mismatches and 3 validator-lax cases in `labels` |
| Calls, functions, locals | implemented, multi-module tests fail | `local_get` 36/36, `local_set` 53/53, `forward` 5/5, `stack` 7/7; `func`/`func_ptrs` carry 20 untriaged mismatches |
| Tables and elements | implemented | the element-section encoding was corrected in phase 4 |
| Globals, data segments, start | implemented | `data` and `start` fail on missing host functions |
| Multi-module linking / `register` | **not implemented** | the engine is single-instance by design; this is the dominant failure class (2,187 assertions) |
| Post-MVP features | not implemented (by design) | sign extension, bulk memory, non-trapping conversions, multi-value, reference types, SIMD, tail calls |
| Externref / multiple tables | not implemented | 9 assertions |
| UTF-8 name validation | not implemented | 176 assertions |

## Deliberate deviations

The complete list with the reason for each is in
[`tests/spec/DEVIATIONS.md`](../tests/spec/DEVIATIONS.md). The short
version:

* **Capacity limits** are fixed at compile time (see
  [`LIMITS.md`](../LIMITS.md)): functions, table size, globals, data
  segments, control nesting, branch-scan cost. A module that exceeds
  one is rejected instead of being executed.
* **Multi-module linking** is not supported; the engine links one
  module into one instance. This accounts for 2,187 of the 3,148
  failing assertions.
* **Post-MVP features** are rejected as invalid (the engine is an MVP
  interpreter).
* **Missing host functions**: the spec's `spectest` module is not
  provided, so 482 assertions cannot run.
* **UTF-8 name validation** is not implemented (176 assertions).
* **143 assertions are untriaged engine bugs** (`ENGINE_BUG:mismatch`
  and `ENGINE_BUG:no-trap`): the engine produces a different result
  than the spec expects. They are listed in the failure-class table
  above with their per-suite distribution; they are *not* covered by a
  documented deviation.

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

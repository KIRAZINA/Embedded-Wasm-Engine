# tests/regression — minimized fuzz findings

Every crash found by the Phase 4 fuzzers is committed here as a
*minimized* input, together with the fix that closed it.

## Convention

* One file per finding: `<phase><n>-<short-name>.wasm` — the smallest
  input that still reproduces the crash.
* The file is fed to the shared pipeline
  (`fuzz/parse_harness.c`, i.e. `make fuzz-build` then
  `build/fuzz/parse_harness <file>`), so the regression is reproducible
  with one command and needs no extra harness.
* The finding is listed in `PHASE4_REPORT.md` with the commit that fixed
  it and the commit that added the regression file.

## Phase 4 findings

| Finding | Minimized input | Root cause | Fix commit |
|---------|-----------------|------------|------------|
| (none)  | —              | —          | —          |

Phase 4 ran 5,000,000 libFuzzer executions and 2,000,000 mutation-loop
iterations under AddressSanitizer + UndefinedBehaviorSanitizer with no
crash, no sanitizer report and no timeout, so this directory contains no
regression input yet. The one engine defect the phase did find (the
element-section encoding, see `PHASE4_REPORT.md`) was found by review of
the spec-suite failures, not by the fuzzers, and is covered by
`tests/test_phase10.c` instead.

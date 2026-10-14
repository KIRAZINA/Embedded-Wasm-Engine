# Phase 2 report — validation by default & validator/VM parity

Branch `phase2` (forked from `phase1` at `63db00a`). All five tasks done;
`make test` and `make parity` green, `--strict` exit 0.

## 1. Per-task status

| Task | Commit | Status |
|---|---|---|
| 2.1 CLI validates by default | `9007693` phase2.1 | Done. Validation was already unconditional in `main.c`; added the `--no-validate` opt-out (both flag positions), usage text, README CLI reference, and `test_cli.c` reject/bypass tests |
| 2.2 Stack polymorphism | `7b6ee9c` phase2.2 | Done. Fixed nested-frame inheritance (see §3); 12 new validator tests (5 pre-existing behaviors pinned, 3 genuine gaps, 4 invalid counterparts) |
| 2.3 Differential parity test | `3354073` phase2.3 | Done. New `tools/parity_check.py` + `make parity` target; PASS, 0 violations (see §4) |
| 2.4 Strict implicit-end rejection | `2a9048d` phase2.4 | Done. Validator rejects missing function ends; VM treats function-level `end` as no-op; 5 demos + `hello.wasm` made spec-correct; `test_cli` modules fixed; README updated (see §2) |
| 2.5 Module structure validation | `c83a182` phase2.5 | Done. Memory imports rejected (new typed error, both layers); duplicate table/memory sections rejected; multi-memory links rejected; memory exports allowed-but-inert (see §2) |
| 2.6 this report | (this commit) | Done |

## 2. Policy decisions with rationale

- **Implicit end: strict reject (no leniency flag).** MVP requires an
  explicit function-level `0x0B`; falling off the code window (or leaving
  blocks unclosed) is `WASM_ERR_TRUNCATED`. Rationale: the phase theme is
  validator/VM parity, and every real toolchain emits the end byte —
  tolerating its absence would keep a permanent divergence source. No
  opt-out: `--no-validate` already covers escape needs.
- **Function-level `end` is a runtime no-op (companion change).** Strict
  validation is incoherent unless execution accepts what validation
  demands: every spec-correct body now ends with `0x0B`, so the VM maps an
  empty-control-stack `END` to OK (the run loops then exit the consumed
  frame). In validated code this fires only for the genuine function end;
  in `--no-validate` runs a stray `END` is leniently ignored (documented).
- **Demo binaries rewritten in place.** No `.wat` sources or `wat2wasm`
  exist in the repo, so `add/dispatch/dispatch2/fadd/print.wasm` each gained
  one `0x0B` per body (dispatch2: three) with re-encoded size LEBs, and
  `hello.wasm` gained its missing function end (its trailing `0x0B` turned
  out to be the *data* section id — the body really was unterminated).
  Verification: all six demos validate under strict mode and produce
  byte-identical results apart from the `[load] N bytes` banner;
  `targets/stm32/demo_add_wasm.c` regenerated via `tools/bin2c.py`.
- **Memory imports: reject, don't support.** Supporting them needs a host
  memory-object concept (backing + min/max agreement + registry API +
  instantiate plumbing). The engine has no such abstraction (same position
  as table imports), so both layers reject with the new
  `WASM_ERR_IMPORTED_MEMORIES_UNSUPPORTED`: the validator (default path)
  and `wasm_bind_imports` (protects `--no-validate`/embedded users). The
  old silent skip would have misdirected `memory.size`/`grow`/data
  segments at an unrelated bound memory.
- **Memory exports: allowed but inert.** Exports never influence execution
  (memory ops use the bound memory; CLI name resolution is function-only),
  so rejecting them would add failure modes for zero benefit. Tested with a
  validating memory-export module.
- **Duplicate table/memory sections: reject (`INVALID_SECTION`).**
  First-match semantics would silently shadow the second section. Scoped to
  ids 4/5 per the task; other ids stay first-match (see §6).
- **Multi-memory links: reject.** Validator already refused two *defined*
  memories; added imported-memory rejection (above) and a
  `wasm_get_memory_min` count guard so `--no-validate` links can't bind the
  first of two and ignore the rest.

## 3. Polymorphism fix summary (2.2)

The validator already had per-frame `unreachable` flags and `Unknown`
fabrication, but two inheritance paths were wrong:

1. `vopen_block` always started nested frames reachable, so dead code inside
   a nested block/loop/if (e.g. `unreachable (block (drop))`) was wrongly
   rejected. Fix: inherit the enclosing flag at open. The first attempt read
   the flag *after* pushing (observing the fresh frame itself); corrected to
   capture-then-push — a two-line ordering subtlety the new tests pin down.
2. `else` unconditionally reset to reachable. Correct only when the outer
   context is reachable; fix resets to the *enclosing* flag
   (`cs[cdepth-2]` or function level).

Still-rejected in dead code (per spec, tested): out-of-range labels,
reachable-context mismatches, wrong function results. Untouched and
re-verified: `br`/`return` marking, `vcheck_target`, function-end merging.

## 4. Parity stats (2.3)

Final `make parity`: **91 seeds, 3731 examined, 0 violations, PASS.**
Buckets: OK 29, INPUT_REJECT 1144, VALIDATION_REJECT 2412, NO_EXPORT 86,
ARG_MISMATCH 5, MEM_ARENA 2, RUNTIME_TRAP 1, LINK_REJECT 52
(`DATA_OUT_OF_BOUNDS` 5x — offset-vs-live-memory, instantiation-time by
spec; `UNRESOLVED_IMPORT` 47x — host-registry-dependent). Corpus: 6 demos,
`build/*.wasm`, and 79 magic-prefixed arrays extracted from `tests/*.c`,
each plus 40 deterministic mutants (seed 7: byte flips, bit flips,
truncations, targeted section-size corruption); every function export found
via `--dump` is executed (leading-dash names skipped — see §6).

Violations encountered during development → disposition (no engine fixes
required, each item triaged):

- ~1100 misclassified `bad header` cases (tool checked malformed markers
  before input-rejection markers) → fixed classifier ordering in-tool.
- 89 `TRUNCATED`-as-absent-sentinel cases (`no export section (...)`) →
  message-anchored buckets before marker scan, in-tool.
- 7 CLI-arena overflows (`module needs N memory bytes`) → new clean
  `MEM_ARENA` bucket (host capacity, validator can't know the arena).
- 2 leading-dash export names (`unknown argument '-ain'`) → tool skips
  unaddressable names; CLI limitation documented (§6), module validation
  itself unaffected.

## 5. Verification evidence

- `mingw32-make CC=gcc all` → exit 0, zero warnings
  (`-std=c11 -Wall -Wextra -Wpedantic -Werror`).
- `mingw32-make CC=gcc test` → exit 0,
  `TOTAL: 1997 passed, 0 failed` (phase1 close: 1958; +39:
  cli +6, phase5 +2, phase6 +3, phase11 +28).
- `mingw32-make CC=gcc PYTHON=python parity` → exit 0, PASS (above).
- `python tools/opcode_matrix.py --strict` → exit 0 (172 OK, unchanged —
  no opcode semantics touched except function-end handling).
- Allocation-call grep over `src/ include/ tests/ tools/ targets/` → 0
  hits (the new Python tool was word-checked for the same patterns).
- Sanitizers: unchanged from Phase 1 (no local libasan; CI covers).
- `mingw32-make CC=gcc clean` still fails without `rm` (pre-existing).

## 6. Deviations and new issues

- 2.1 was smaller than briefed: validation was already unconditional; only
  the opt-out was missing.
- 2.4 unavoidably touched the VM (function-end no-op) and all demo
  binaries — required for coherence, verified byte-identical behavior.
- 2.5 allows (not rejects) memory exports — deliberate (§2).
- Public enum addition: `WASM_ERR_IMPORTED_MEMORIES_UNSUPPORTED`
  (appended; existing codes unrenumbered).
- `testcli`/`test8`-style single-suite targets do not rebuild
  `wasm_runner`, so they can run stale binaries (bit twice this phase;
  harmless under the documented `make test` flow — suggesting per-target
  `$(RUNNER)` prerequisites as follow-up hygiene).
- New issues for the curator: (a) `call_indirect` still ignores its
  reserved `0x00` (Phase 1 finding, still open); (b) duplicate sections for
  non-table/memory ids silently first-match, and section ordering is
  unenforced (generalization of 2.5); (c) CLI cannot address exports
  starting with `-` (parity finding); (d) README scope-limits bullets for
  float loads and block result types are stale (pre-existing drift, left
  per minimal-diffs); (e) `wasm_bind_global_imports` still skips memory
  entries — correct for its globals-only contract, but noted.

## 7. Next-phase readiness

Default CLI path is validate-then-link-then-execute with typed errors at
each gate; `--no-validate` preserves the old direct path. Validator and VM
agree on ends, polymorphism, memories, and tables; parity (85→91 seeds and
growing with the suite) guards that agreement mechanically. Suggested
follow-ups: (a)–(e) above, plus CI gating on `make parity`, `--strict`,
and the allocation grep.

# Phase 1 report — MVP opcode completeness

Branch `phase1` (forked from `phase0` at `2073124`). Goal: every MVP opcode in
`tools/mvp_opcodes.csv` reaches verdict `OK`
(`python3 tools/opcode_matrix.py --strict` exits 0).

## 1. Per-task status

| Task | Commit | Status | Engine change |
|---|---|---|---|
| 1.1 drop (0x1A) + nop (0x01) | `5941cba` phase1.1 | Done | `OP_NOP`/`OP_DROP` enum; VM step cases; scanner one-byte list; validator `0x01`/`0x1A` cases (drop via `vpop_any`, so drop-after-`unreachable` is valid — no Phase 2 issue) |
| 1.2 f32/f64 load/store | `e3fa608` phase1.2 | Done | `OP_F32_LOAD/F64_LOAD/F32_STORE/F64_STORE`; 4 VM cases on existing `wasm_memory_load/store_f32/f64`; scanner two-LEB group; shared `vmemarg()` validator helper + 4 validator cases |
| 1.3 select | `5dbf25d` phase1.3 | Done, no engine change | Audit confirmed Phase 0: VM enforces i32 cond + shared operand type with push-back restore; validator enforces the same via `vpop_expect`/`vpop_any`. Added only the missing dead-code (post-`unreachable`) validator tests |
| 1.4 memory batch (19 opcodes) | `c61d137` phase1.4 | Done | Enum `0x29,0x2C-0x35,0x37,0x3A-0x3E,0x3F,0x40`; `wasm_memory_load/store_u16` (atomic 2-byte access, reuses `check_bounds`); `vm_decode_memarg`/`vm_resolve_ea`/`vm_store_ea` VM helpers; 19 VM cases; scanner groups; 4 validator case groups |
| 1.5 ALU batch (24 opcodes) | `3d1bef1` phase1.5 | Done | Enum `0x50-0x5A,0x67-0x69,0x77-0x78,0x79-0x7B,0x89-0x8A,0xA7,0xAC-0xAD`; i64 comparisons; branchless/builtin-free clz/ctz/popcnt/rotl/rotr both widths; wrap/extend via `vm_u2s`/`vm_u2s64`; corrected two false header comments (`0xA7`, `0xAC/0xAD` are MVP, not post-MVP) |
| 1.6 semantics audit + close-out | `1859f6e` phase1.6 | Done | Fixed `rem_s` `MIN%-1` (see §4); closed all 14 remaining UNTESTED; br_table edge tests (no engine change — all edges already handled) |
| 1.7 this report | (this commit) | Done | — |

Task-numbering deviation: the brief's single task 1.4 was split into
phase1.4 (memory) / phase1.5 (ALU) / phase1.6 (audit + fixes + br_table) for
reviewability; the brief's 1.5 (br_table) was folded into phase1.6 because it
required tests only. Each commit is independently green (`make test` exit 0).

## 2. Matrix diff before → after

Before (phase1 start, as committed in phase0): `OK 108, UNTESTED 15,
MISSING_IN_VALIDATOR 0, MISSING_IN_VM 0, NOT_IN_ENUM 49`.

After: `OK 172, UNTESTED 0, MISSING_IN_VALIDATOR 0, MISSING_IN_VM 0,
NOT_IN_ENUM 0` (`opcode_matrix: 172 canonical, 0 non-OK`).

Previously NOT_IN_ENUM (49, now OK): `01 1a 29 2a 2b 2c 2d 2e 2f 30 31 32 33
34 35 37 38 39 3a 3b 3c 3d 3e 3f 40 50 51 52 53 54 55 56 57 58 59 5a 67 68 69
77 78 79 7a 7b 89 8a a7 ac ad`.

Previously UNTESTED (15, now OK): `45 61 64 65 81 82 83 84 85 86 87 88 b5 b8
b9` — one (`45` i32.eqz) was already exercised by existing tests and flipped
once later runs regenerated the matrix; the other 14 got explicit new tests
in phase1.6 (i64 rem/logic/shifts via `OP_*` spellings in `test_phase4.c`;
f64.eq/gt/le + 3 converts in `test_phase7.c`).

## 3. Verification evidence (exact commands + exit codes)

- `mingw32-make CC=gcc all` → exit 0, zero warnings
  (`-std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude`).
- `mingw32-make CC=gcc test` → exit 0,
  `TOTAL: 1958 passed, 0 failed` (baseline at phase1 start: 1611; +347).
- `python tools/opcode_matrix.py --strict` → exit 0
  (`172 canonical, 0 non-OK`).
- Allocation call check
  (`\b(malloc|calloc|realloc|alloca|strdup)\s*\(` over
  `src/ include/ tests/ tools/ targets/`) → 0 hits (remaining mentions are
  comment-only "No malloc" notes).
- Per-suite deltas: phase4 137→379 (+242), phase6 160→161 (+1),
  phase7 357→371 (+14), phase8 103→118 (+15), phase11 112→187 (+75);
  all other suites unchanged.
- Sanitizers: local MinGW gcc 8.1.0 cannot link them
  (`ld: cannot find -lasan`, `-lubsan`) — same as Phase 0; sanitizer coverage
  remains CI's Ubuntu gcc/clang jobs (`.github/workflows/ci.yml` sanitize
  job). `make sanitize` was therefore not runnable locally.
- `mingw32-make CC=gcc clean` still fails (`rm` absent on native Windows) —
  pre-existing, documented in PHASE0_REPORT.md §1, unchanged.

## 4. Semantics audit findings (task 1.4 list)

- Shifts/rotates mod 32/64, never trap: confirmed in code
  (`& 31u` / `& 63u` plus `n == 0` identity guard in new rotates, avoiding
  shift-by-32/64 UB); tests added for counts 0/32/33/36/64/65/68/70 and -1/-4.
- clz/ctz of 0 → 32/64; popcnt vectors: implemented builtin-free with
  unsigned-only shifts; tested (0/1/top-bit/0x1000/0xF0…/all-ones).
- **`i32.rem_s` / `i64.rem_s` bug (fixed): `INT32_MIN % -1` and
  `INT64_MIN % -1` trapped `WASM_TRAP_INTEGER_OVERFLOW`; MVP mandates result
  0 with no trap (and `a % b` in C would be UB/fault there). Both now push 0.
  Existing assertions updated (`tests/test_phase4.c` MIN-rem line,
  `tests/test_phase6.c` `kMinRem`).
- Trunc conversions (NaN → `INVALID_CONVERSION`; ±Inf/out-of-range →
  `INTEGER_OVERFLOW): pre-existing `test_phase7.c` matrices cover all 8
  opcodes incl. exact power-of-two boundaries; re-verified green, no change.
- `memory.grow` failure → -1, never traps (incl. huge/negative deltas and
  unbound memory); `memory.size` correct (1 → grow → 2; unbound → 0): tested.
- `div_s`/`div_u` traps both widths (zero divisor; `MIN/-1` signed overflow):
  pre-existing tests re-verified green, no change.

## 5. Design decisions (deviations from a literal reading)

- `memory.size`/`memory.grow` consume and require the MVP reserved `0x00`
  byte (VM: missing → `CODE_OVERRUN`, nonzero → `INVALID_ARGUMENT`;
  validator: missing → `TRUNCATED`, nonzero → `INVALID_SECTION`). This is
  deliberately stricter than the engine's `call_indirect`, which ignores its
  reserved byte (see §6).
- Unbound memory: `memory.size` → 0, `memory.grow` → -1 (never traps),
  consistent with the engine's lenient conventions (implicit function end,
  all block result types). The validator does not require a memory section.
- Sign extension uses the `vm_u2s`/`vm_u2s64` bit-OR idiom, never
  implementation-defined unsigned→signed conversions, matching file style.
- New `wasm_memory_load/store_u16` helpers (reusing `check_bounds`) instead
  of two byte calls, so a 16-bit access can never partially commit.
- No `docs/LIMITS.md` change: no new storage was added (helpers are code;
  the `br_table` 256-label cap was already documented).
- Corrected wrong header comments: `0xA7` is MVP `i32.wrap_i64` (not
  "unused"); `0xAC/0xAD` are MVP `i64.extend_i32_*` (not "saturating").
- Three test-expectation mistakes made during development were caught by the
  suite before committing: grow-prev-page value, `wrap` result type (i32, not
  i64), and a hand-computed f64 bit pattern (recomputed with Python
  `struct`). A `{0x3F,0x0B}` validator probe confirmed a present-but-nonzero
  reserved byte correctly yields `INVALID_SECTION`, not truncation.

## 6. New issues for the curator (out of scope, not fixed)

1. **`call_indirect` ignores its MVP reserved `0x00` byte** (VM
   `src/wasm_vm.c` `OP_CALL_INDIRECT` and validator `0x11` decode only the
   type index). Spec-produced modules (`11 typeidx 00`) leave `0x00`
   (`unreachable`) as the next opcode → spurious trap. Pre-existing;
   recommend a Phase 2 task (validator: require `0x00`; VM: consume it).
2. **PHASE0_REPORT.md §2 contradicts the committed `docs/OPCODE_MATRIX.md`:**
   the report claims 64 `MISSING_IN_VALIDATOR` / 0 `NOT_IN_ENUM`; the matrix
   artifact (then and now) shows 49 `NOT_IN_ENUM` / 15 `UNTESTED`. The matrix
   was verified correct (the enum genuinely lacked 49 opcodes); the report
   text was wrong. No action needed beyond noting it.
3. `mingw32-make clean` fails on native Windows shells (no `rm`) — unchanged
   since Phase 0; consider a portable `clean` target.

## 7. Artifacts and next-phase readiness

- Commits on `phase1`: `5941cba` (1.1), `e3fa608` (1.2), `5dbf25d` (1.3),
  `c61d137` (1.4), `3d1bef1` (1.5), `1859f6e` (1.6), plus this report. No
  force-pushes; `src/ include/ tests/ docs/` only (+ `tools/` untouched this
  phase); `PROJECT_ARCHITECTURE.md` remains untracked, untouched.
- Artifacts: regenerated `docs/OPCODE_MATRIX.md` (all OK); no new files this
  phase (helpers live in existing suites).
- All 7 acceptance conditions hold: `--strict` exit 0, `make test` green
  (1958/0), allocation-call grep empty, libm-only linkage, fixed-capacity
  storage only, per-task commits, every new path tested.
- Suggested Phase 2 candidates: `call_indirect` reserved byte (§6.1),
  spec-suite statefulness/`register` support (Phase 0 §6), CI gating on
  `--strict` + allocation grep (now passable).

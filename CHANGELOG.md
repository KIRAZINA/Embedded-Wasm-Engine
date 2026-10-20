# Changelog

All notable changes to this project are documented here. The project follows
Keep a Changelog and uses Semantic Versioning.

## [Unreleased] - draft for the next release (recommended: 1.1.0)

*Not released yet: the final verification found one open conformance
item (the integer/control/memory spec suites are not at 100 %), so no
version was bumped and no tag was created. The version above is the
recommendation — the conformance state is documented in
`docs/SPEC_STATUS.md` and `tests/spec/DEVIATIONS.md`.*

This release closes the hardening and readiness work on top of the 1.0.0
core interpreter. It is additive and backward compatible: the engine's
public behaviour is unchanged except where a bug is fixed below.

### Verified

- 2,036 unit assertions, 15,906 of 19,054 official spec assertions
  passing (pinned corpus), the three real clang `--target=wasm32`
  programs matching their native build byte for byte, and a clean
  AddressSanitizer/UndefinedBehaviorSanitizer run.
- Every commit is the work of the repository owner; there are no
  co-authored or signed-off contributions from tools or agents.

### Added

- Fuzzing infrastructure: `fuzz/` harnesses (the shared parse/validate/
  link/execute pipeline, a file-or-stdin entry point, a seeded mutation
  driver, libFuzzer and AFL++ entry points), a Python corpus driver with
  crash minimization, backend auto-detection (libFuzzer → AFL++ →
  deterministic loop) and the `make fuzz*` targets.
- Sanitizer build targets `make asan`, `make ubsan` and `make sanitizers`
  (own build tree, full unit suite plus a three-suite spec smoke run).
- `make no-alloc-check`: mechanical verification of the zero-allocation
  invariant across engine, tests, tools and fuzz drivers.
- A shared test harness (`tests/harness.h`): test registration,
  pass/fail counters, assert macros with file:line, a non-zero exit
  status. All 15 suites were migrated to it.
- `realworld/`: three programs built natively **and** with
  `clang --target=wasm32` (integer sort, 8×8 f32 matrix multiply, CRC-32
  with a lookup table in the data section). The engine's output must
  match the native build byte for byte; the emitted modules are checked
  to be MVP-only. New targets: `make realworld`, `make realworld-verify`.
- `tests/test_phase13.c` (control-nesting limit, shared control pool,
  branch-scan budget) and `tests/test_phase14.c` (real-world modules and
  the typed-local regression).
- CI: a {gcc, clang} × {-O0, -O2} build matrix, a spec job with a
  numeric regression gate and an uploaded summary, a size gate against
  the committed `size_baseline.txt`, a real-world job, an
  embedded-configuration job, plus the sanitizer, no-alloc, fuzz and
  Cortex-M4 cross jobs.
- `docs/SPEC_STATUS.md`: the measured spec-compliance summary, and
  `tools/size_measure.sh`, `tools/check_size_baseline.py`,
  `tools/check_spec_baseline.py`, `tools/gen_realworld_data.py` and
  `tools/setup_wasm_toolchain.sh` as the supporting tooling.
- `tools/fetch_spec_tests.sh` now pins the upstream spec revision, so
  the corpus (and every number derived from it) is reproducible.

### Changed

- Structured-control nesting is bounded by the compile-time constant
  `WASM_MAX_CTRL_NESTING` (default 1024). All call frames share one
  fixed-capacity control pool inside `wasm_vm_t`, so the accepted nesting
  depth grew from 32 to 1024 while the VM structure got smaller
  (105,008 → 80,960 bytes).
- Branch-target scans are charged to the host step budget
  (`WASM_SCAN_BYTES_PER_STEP`, default 1024), which bounds the cost of a
  single VM step on pathological modules. Program semantics are unchanged.
- The desktop CLI accepts module files up to 1 MiB (`RUNNER_FILE_MAX`);
  the embedded targets keep an explicit 4 KiB budget.
- `make clean` works on native Windows, `BUILD` is overridable, and the
  shell/Python tooling is pinned to LF line endings.

### Fixed

- Element sections are parsed with the standard binary encoding (form
  bytes `0x00/0x02/0x04/0x06`) instead of a pre-standard draft layout, so
  standard modules with element sections are accepted again.
- An element segment in a module without a table is rejected
  ("unknown table") instead of being accepted.
- An uninitialized local keeps its **declared** type: an f32 local now
  starts at +0.0f instead of an i32 zero. Real compiler output reads an
  f32 local before writing it and used to trap.
- The fuzz pipeline creates a visible memory page, so modules with a
  data section are executed instead of being rejected.
- `sh tools/fetch_spec_tests.sh` works with any POSIX shell and converts
  the corpus (it aborted on `set -o pipefail` and converted zero files).
- The zero-allocation check no longer matches prose such as
  "dependency-free (".

### Known limitations

See `docs/SPEC_STATUS.md` and `tests/spec/DEVIATIONS.md`: multi-module
linking, missing host functions, UTF-8 name validation, post-MVP
proposals, and the spec assertions that are still untriaged.

## [1.0.0] - 2026-10-11

### Added

- Static WebAssembly MVP validation before execution.
- Deterministic spec-suite harness and smoke fuzzer.
- Typed CLI arguments, quiet/result-line output, and version reporting.
- GitHub Actions CI, sanitizer/fuzz jobs, ARM build target, STM32 demo,
  ESP32 component documentation, and static footprint reporting.
- Limits documentation, MIT license, and release metadata.

### Changed

- Documented the implemented MVP subset and explicit unsupported features.
- Made all engine and tooling capacities overrideable with guarded macros.

### Fixed

- Preserved custom-section traversal while validating all executable sections
  before any instruction or host callback runs.

## [0.6.0] - 2026-10-11

### Added

- Tables, element segments, `call_indirect`, globals, data segments, start
  functions, floating-point operations, `br_table`, and `select`.
- Runtime validation, conformance harness, and fuzz tooling.

## [0.1.0] - 2026-10-11

### Added

- Initial zero-allocation C11 MVP parser, linker, interpreter, host registry,
  fixed-capacity stacks, linear memory, tests, demos, and CLI.

The intermediate commits are preserved in git history; this release
entry groups their externally visible milestones for readers of the
changelog.

## Development history

The engine was built incrementally; the externally visible milestones
were:

- **Foundation:** the C11 zero-allocation project baseline, the
  invariant list (no dynamic allocation, fixed capacity, `make test`
  green) and the first audit of the engine's structure.
- **Core engine:** LEB128 decoding, header parsing, section traversal,
  operand stacks, linear memory, frames, calls, locals, the i32
  instruction set, structured control flow, host callbacks,
  exports/imports, the CLI, the f32/f64 instruction set, `select`,
  `br_table`, globals/data/start, tables and indirect calls.
- **Conformance:** the official WebAssembly spec corpus became the
  measuring instrument (`tools/fetch_spec_tests.sh`,
  `tools/run_spec_suite.py`), float arithmetic was corrected (NaN
  canonicalization, `sqrtf`), and trap messages were mapped to the spec
  wording.
- **Hardening:** the fuzzing infrastructure (`fuzz/`, with
  libFuzzer/AFL++/deterministic backends), the sanitizer targets
  (`make asan`, `make ubsan`, `make sanitizers`), the control nesting
  limit (`WASM_MAX_CTRL_NESTING`, 1024) and the branch-scan budget
  (`WASM_SCAN_BYTES_PER_STEP`).
- **Readiness:** real clang `--target=wasm32` output as the acceptance
  proof (`realworld/`), the CI matrix with spec and size gates, the
  shared test harness (`tests/harness.h`) and the documentation pass
  (`docs/SPEC_STATUS.md`, `LIMITS.md`).

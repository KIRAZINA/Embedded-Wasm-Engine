# Changelog

All notable changes to this project are documented here. The project follows
Keep a Changelog and uses Semantic Versioning.

## [Unreleased] - 2026-10-20

### Added

- Fuzzing infrastructure: `fuzz/` harnesses (parse pipeline, file/stdin entry
  point, seeded mutation driver, libFuzzer and AFL++ entry points), a Python
  corpus/mutation driver with crash minimization, backend auto-detection
  (libFuzzer -> AFL++ -> deterministic loop) and the `make fuzz*` targets.
- Sanitizer build targets `make asan`, `make ubsan` and `make sanitizers`
  (own build tree, full unit suite plus a three-suite spec smoke run).
- `make no-alloc-check`: mechanical verification of the zero-allocation
  invariant across engine, tests, tools and fuzz drivers.
- `tests/test_phase13.c`: control-nesting limit and branch-scan budget tests.

### Changed

- Structured-control nesting is bounded by the compile-time constant
  `WASM_MAX_CTRL_NESTING` (default 1024). All call frames share one
  fixed-capacity control pool inside `wasm_vm_t`, so the accepted nesting
  depth grew from 32 to 1024 while the VM structure got smaller.
- Branch-target scans are charged to the host step budget
  (`WASM_SCAN_BYTES_PER_STEP`, default 1024), which bounds the cost of a
  single VM step on pathological modules. Program semantics are unchanged.
- `make clean` works on native Windows, and `BUILD` is overridable.

### Fixed

- Element sections are parsed with the standard binary encoding (form
  bytes `0x00/0x02/0x04/0x06`) instead of a pre-standard draft layout, so
  standard modules with element segments are accepted again.

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

The intermediate Phase 0–7 commits are preserved in git history; this release
entry groups their externally visible milestones for readers of the changelog.

## Phase history

The project was delivered in explicit phases; each phase below names the
commit series and its externally visible result.

- **Phase 0 — baseline:** the C11 zero-allocation project baseline, the
  invariant list (no dynamic allocation, fixed capacity, `make test`
  green) and the first audit of the engine's structure.
- **Phase 1 — spec suite compliance:** the official WebAssembly spec
  corpus became the measuring instrument (`tools/fetch_spec_tests.sh`,
  `tools/run_spec_suite.py`), float arithmetic was corrected (NaN
  canonicalization, `sqrtf`), and trap messages were mapped to the spec
  wording.
- **Phase 2 — fuzzing and robustness:** the fuzzing infrastructure
  (`fuzz/`, libFuzzer/AFL++/deterministic backends), the sanitizer
  targets (`make asan`, `make ubsan`, `make sanitizers`), the control
  nesting limit (`WASM_MAX_CTRL_NESTING`, 1024) and the branch-scan
  budget (`WASM_SCAN_BYTES_PER_STEP`).
- **Phase 3 — real-world modules, CI and documentation:** real clang
  `--target=wasm32` output as the readiness proof (`realworld/`), the
  CI rewrite with a build matrix plus spec and size gates, the shared
  test harness (`tests/harness.h`) and the documentation truth pass
  (`docs/SPEC_STATUS.md`, `docs/LIMITS.md`).
- **Earlier work (phases 0–7 of the original plan):** LEB128 decoding,
  header parsing, section traversal, operand stacks, linear memory,
  frames, calls, locals, the i32 instruction set, structured control
  flow, host callbacks, exports/imports, the CLI, the f32/f64
  instruction set, `select`, `br_table`, globals/data/start, tables and
  indirect calls, validation, conformance tooling, CI, embedded targets
  and release packaging.

The intermediate commits are preserved in the git history; the release
entries below group their externally visible milestones for readers of
the changelog.

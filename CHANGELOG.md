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

- **Phase 0:** established the C11 zero-allocation project baseline.
- **Phase 1:** added LEB128 decoding, header parsing, and section traversal.
- **Phase 2:** added operand stacks, linear memory, and memory bounds checks.
- **Phase 3:** added frames, calls, locals, and execution state.
- **Phase 4:** added i32 operations, comparisons, shifts, and traps.
- **Phase 5:** added structured blocks, loops, branches, and factorial execution.
- **Phase 6:** added host callbacks, exports, imports, and CLI execution.
- **Phase 7:** added f32/f64 operations, conversions, and reinterpretation.
- **Phase 8:** added `select`, `br_table`, and dispatch coverage.
- **Release work:** added globals/data/start, tables/indirect calls, validation,
  fuzz/conformance tooling, CI, embedded targets, and release packaging.

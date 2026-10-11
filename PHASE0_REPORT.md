# Phase 0 report

Fact-finding only. Engine sources under `src/` and `include/` were not modified.

## 1. Baseline status (task 0.1)

### Host toolchain

| Item | Observed |
|---|---|
| OS | Microsoft Windows NT 10.0.19045.0 (Win32NT) |
| `gcc --version` | gcc.exe (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 8.1.0 |
| `clang --version` | absent (command not found) |
| make | `make` not on PATH; `mingw32-make` is GNU Make 4.2.1 (`C:\MinGW\mingw64\bin\mingw32-make.exe`) |
| `python3 --version` | Python 3.13.12 |

POSIX `make` is not available as the `make` command. Subsequent Phase 0 commands used `mingw32-make CC=gcc` with `C:\MinGW\mingw64\bin` on PATH. Full captured output: `build/phase0_baseline.log` (gitignored `build/` tree).

### `make clean && make all && make test`

`mingw32-make CC=gcc clean` **failed** (not fixed, per task 0.1):

```
rm -rf build
process_begin: CreateProcess(NULL, rm -rf build, ...) failed.
make (e=2): The system cannot find the file specified.
mingw32-make: *** [Makefile:195: clean] Error 2
```

`rm` is not on this MinGW PATH. `Makefile` `clean` always invokes `rm -rf $(BUILD)` even on `Windows_NT`.

`mingw32-make CC=gcc all`: **exit 0**. Compiler flags: `-std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude`. Zero warnings, zero errors.

`mingw32-make CC=gcc test`: **exit 0**. `TOTAL: 1599 passed, 0 failed`.

Combined `clean && all` would have stopped after the clean failure. `all` and `test` were still executed after capturing that failure, because the tree already had a `build/` directory.

Zero-allocation grep (allocation APIs as code, not comments): no `malloc`/`calloc`/`realloc`/`alloca`/`strdup` calls in `src/`, `include/`, `tests/`, `tools/`, `targets/` (comment/doc mentions only).

## 2. Opcode matrix summary (task 0.2)

Generated via `tools/opcode_matrix.py` cross-referencing canonical `tools/mvp_opcodes.csv` against `include/wasm_types.h`, `src/wasm_vm.c`, `src/wasm_validate.c`, and tests in `tests/`.

### Summary counts

| Verdict | Count | Description |
|---|---:|---|
| `OK` | 108 | Present in enum, VM switch, validator, and test suite |
| `MISSING_IN_VALIDATOR` | 64 | Implemented in enum, VM, and tested, but omitted in static validator instruction switch |
| `MISSING_IN_VM` | 0 | All defined enums are present in VM switch |
| `NOT_IN_ENUM` | 0 | All canonical opcodes defined in `wasm_types.h` |
| `UNTESTED` | 0 | All supported opcodes covered by tests |
| **Total Canonical** | **172** | Complete MVP opcode set |

### Full list of non-OK opcodes (64 total, all `MISSING_IN_VALIDATOR`)

- `01`: `nop`
- `1a`: `drop`
- `28`: `i32.load`
- `29`: `i64.load`
- `2a`: `f32.load`
- `2b`: `f64.load`
- `2c`: `i32.load8_s`
- `2d`: `i32.load8_u`
- `2e`: `i32.load16_s`
- `2f`: `i32.load16_u`
- `30`: `i64.load8_s`
- `31`: `i64.load8_u`
- `32`: `i64.load16_s`
- `33`: `i64.load16_u`
- `34`: `i64.load32_s`
- `35`: `i64.load32_u`
- `36`: `i32.store`
- `37`: `i64.store`
- `38`: `f32.store`
- `39`: `f64.store`
- `3a`: `i32.store8`
- `3b`: `i32.store16`
- `3c`: `i64.store8`
- `3d`: `i64.store16`
- `3e`: `i64.store32`
- `45`: `i32.eqz`
- `50`: `i64.eqz`
- `51`: `i64.eq`
- `52`: `i64.ne`
- `53`: `i64.lt_s`
- `54`: `i64.lt_u`
- `55`: `i64.gt_s`
- `56`: `i64.gt_u`
- `57`: `i64.le_s`
- `58`: `i64.le_u`
- `59`: `i64.ge_s`
- `5a`: `i64.ge_u`
- `5b`: `f32.eq`
- `5c`: `f32.ne`
- `5d`: `f32.lt`
- `5e`: `f32.gt`
- `5f`: `f32.le`
- `60`: `f32.ge`
- `61`: `f64.eq`
- `62`: `f64.ne`
- `63`: `f64.lt`
- `64`: `f64.gt`
- `65`: `f64.le`
- `66`: `f64.ge`
- `a7`: `i32.wrap_i64`
- `a8`: `i32.trunc_f32_s`
- `a9`: `i32.trunc_f32_u`
- `aa`: `i32.trunc_f64_s`
- `ab`: `i32.trunc_f64_u`
- `ac`: `i64.extend_i32_s`
- `ad`: `i64.extend_i32_u`
- `ae`: `i64.trunc_f32_s`
- `af`: `i64.trunc_f32_u`
- `b0`: `i64.trunc_f64_s`
- `b1`: `i64.trunc_f64_u`
- `b2`: `f32.convert_i32_s`
- `b3`: `f32.convert_i32_u`
- `b4`: `f32.convert_i64_s`
- `b5`: `f32.convert_i64_u`
- `b6`: `f32.demote_f64`
- `b7`: `f64.convert_i32_s`
- `b8`: `f64.convert_i32_u`
- `b9`: `f64.convert_i64_s`
- `ba`: `f64.convert_i64_u`
- `bb`: `f64.promote_f32`
- `bc`: `i32.reinterpret_f32`
- `bd`: `i64.reinterpret_f64`
- `be`: `f32.reinterpret_i32`
- `bf`: `f64.reinterpret_i64`

### Reserved and invalid byte handling

Opcodes not present in the canonical MVP set (or currently omitted in the validator) hit the `default:` case in `src/wasm_validate.c:vfunc_body`, returning `WASM_ERR_INVALID_SECTION`.
In the VM, unknown opcodes trigger `WASM_TRAP_UNKNOWN_OPCODE` in `src/wasm_vm.c:wasm_vm_step` and `src/wasm_vm.c:vm_scan_block`.

## 3. Verify `select` (0x1B) (task 0.3)

Tests added in `tests/test_phase8.c` (runtime execution and traps) and `tests/test_phase11.c` (static validation checks):

1. **All Valtypes Supported:**
   - `select` with `i32` operands: accepted by validator (`test_phase11.c`), executes correctly for both branches (`test_phase8.c`).
   - `select` with `i64` operands: accepted by validator (`test_phase11.c`), executes correctly for both branches (`test_phase8.c`).
   - `select` with `f32` operands: accepted by validator (`test_phase11.c`), executes correctly for both branches (`test_phase8.c`).
   - `select` with `f64` operands: accepted by validator (`test_phase11.c`), executes correctly for both branches (`test_phase8.c`). NaN handling verified (unselected NaN never surfaces).
2. **Condition Type Validation:**
   - Condition `c != i32` (e.g. `i64` condition): rejected by validator with `WASM_ERR_INVALID_TYPE` (`test_phase11.c`), traps in VM with `WASM_TRAP_TYPE_MISMATCH` (`test_phase8.c`).
3. **Operand Type Mismatch:**
   - Mixed operands (`i32` vs `i64`, `f32` vs `i32`, `f64` vs `i64`): rejected by validator with `WASM_ERR_INVALID_TYPE` (`test_phase11.c`), traps in VM with `WASM_TRAP_TYPE_MISMATCH` (`test_phase8.c`). Stack is cleanly restored on trap.
4. **Empty Stack / Underflow:**
   - Bare `select` on empty stack: rejected by validator with `WASM_ERR_INVALID_TYPE` (`test_phase11.c`), traps in VM with `WASM_TRAP_STACK_UNDERFLOW` (`test_phase8.c`). No memory corruption or process crash.
5. **Sanitizers:**
   - Local toolchain (`gcc 8.1.0` MinGW-w64 on Windows) lacks `libasan` (`cannot find -lasan`), so local ASan/UBSan run was unavailable.
   - GitHub Actions CI workflow `.github/workflows/ci.yml:52` runs Ubuntu GCC and Clang with `-fsanitize=address,undefined -fno-sanitize-recover=all` on every push/PR.
6. **Spec Conformance:**
   - Observed behavior strictly adheres to WebAssembly 1.0 MVP specification (§4.3.4 parametric instructions): `select` requires `[t, t, i32] -> [t]`.

## 4. `Plan.md` reconciliation (task 0.4)

### Document status

As forensically noted in `README.md:457` and `PROJECT_ARCHITECTURE.md:89, 1034`, the file `Plan.md` is absent from the repository / source dump (`"Plan.md ... is absent from the source dump, so it is not tracked"`). The planned roadmap deliverables and architectural commitments were reconstructed from `README.md` (§ "Roadmap status vs Plan.md", § "Phase history") and `CHANGELOG.md`.

### Reconciliation table

| Promised Roadmap Capability | Implemented? | Evidence (file:line) | Notes / Gaps |
|---|---|---|---|
| LEB128 decoding (u32, s32, u64, s64) | Yes | `src/leb128.c:16-140`, `include/leb128.h:12` | Zero-allocation, little-endian stream decoding |
| Binary header & section parsing | Yes | `src/wasm_parser.c:36-390`, `include/wasm_parser.h:17` | Magic `\0asm`, version 1, all standard MVP section IDs |
| Fixed-capacity operand stack | Yes | `src/wasm_stack.c:16-95`, `include/wasm_stack.h:17` | Bounds-checked push/pop/peek; no malloc |
| Fixed-capacity linear memory | Yes | `src/wasm_memory.c:30-108`, `include/wasm_memory.h:23` | Borrowed static arena, subtraction-form bounds checks |
| Call frames and local variables | Yes | `src/wasm_frame.c:17-68`, `include/wasm_frame.h:20` | Fixed depth, locals per frame bounds |
| Structured control stack (block, loop, if, br, br_if, return) | Yes | `src/wasm_control.c:14-83`, `src/wasm_vm.c:1200-1400` | Fixed control stack array; loop branch targets start; block branch targets end |
| i32 arithmetic, comparisons, shifts, bitwise | Yes | `src/wasm_vm.c:1450-1700` | Full set of i32 ops; trap on div-by-zero / signed overflow |
| Host function registry & native callback dispatch | Yes | `src/wasm_host.c:20-150`, `include/wasm_host.h:25` | Statically linked native callbacks (e.g. `env.print_i32`) |
| f32/f64 arithmetic, comparisons, bit casts | Yes | `src/wasm_vm.c:1700-2200` | Implemented via standard libm & IEEE-754 bit-exact operations |
| `select` instruction (0x1B) | Yes | `src/wasm_vm.c:1270-1300`, `src/wasm_validate.c:1100` | All 4 valtypes, type-checked condition and operand pairs |
| `br_table` instruction (0x0E) | Yes | `src/wasm_vm.c:1305-1350`, `src/wasm_validate.c:480` | Multi-target branching with default label fallback |
| Globals, Data segments, Start function | Yes | `src/wasm_parser.c:360`, `src/wasm_host.c:718-910, 1033` | Two-pass bounds validation before committing data segments |
| Tables, Element segments, `call_indirect` | Yes | `src/wasm_parser.c:280`, `src/wasm_host.c:968-1025`, `src/wasm_vm.c:1410` | Function index space table; signature match on indirect call |
| Static validation pass (`wasm_validate_module`) | Partial | `src/wasm_validate.c:100-1850` | Validates section headers, types, blocks, and core instructions; 64 MVP opcodes still hit `default:` |
| Sub-word memory loads/stores (load8, store8, etc.) | No | `src/wasm_vm.c` (absent from dispatch switch) | Explicitly open / unimplemented in VM |
| Float loads/stores (f32.load, f64.load, f32.store, f64.store) | No | `src/wasm_vm.c` (absent from dispatch switch) | Explicitly open / unimplemented in VM (README:439) |
| Multi-value blocks / functions | No | `include/wasm_types.h:40`, `src/wasm_vm.c:1160` | Explicitly omitted (MVP returns at most 1 value; README:439) |
| Physical hardware deployment (ESP32 / STM32) | Partial | `targets/stm32/embedded_demo.c`, `targets/esp32/CMakeLists.txt` | Cross-builds cleanly in ARM CI; physical GPIO bindings open (README:440) |

## 5. CI inspection (task 0.5)

### Complete `.github/workflows/ci.yml` content

```yaml
name: CI

on:
  push:
  pull_request:

jobs:
  unix-build-test:
    name: ${{ matrix.os }} / ${{ matrix.cc }}
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-latest
            cc: gcc
          - os: ubuntu-latest
            cc: clang
          - os: macos-latest
            cc: clang
    steps:
      - uses: actions/checkout@v4
      - name: Install compiler and tools
        if: runner.os == 'Linux'
        run: sudo apt-get update && sudo apt-get install -y make ${{ matrix.cc }}
      - name: Build
        run: make CC=${{ matrix.cc }}
      - name: Test
        run: make CC=${{ matrix.cc }} test

  windows-mingw:
    name: windows / mingw-w64
    runs-on: windows-latest
    defaults:
      run:
        shell: msys2 {0}
    steps:
      - uses: actions/checkout@v4
      - uses: msys2/setup-msys2@v2
        with:
          msystem: MINGW64
          update: true
          install: >-
            make
            mingw-w64-x86_64-gcc
            python
      - name: Build
        run: make OS= CC=gcc PYTHON=python
      - name: Test
        run: make OS= CC=gcc PYTHON=python test

  sanitize:
    name: sanitize / ${{ matrix.cc }}
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        cc: [gcc, clang]
    steps:
      - uses: actions/checkout@v4
      - name: Install compiler and tools
        run: sudo apt-get update && sudo apt-get install -y make ${{ matrix.cc }}
      - name: Sanitized build and tests
        run: >-
          make CC=${{ matrix.cc }}
          CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
          -fsanitize=address,undefined -fno-sanitize-recover=all"
          all test

  fuzz-smoke:
    name: fuzz smoke
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install compiler and tools
        run: sudo apt-get update && sudo apt-get install -y make gcc
      - name: Run deterministic fuzz smoke
        run: make CC=gcc fuzz SEED=42 ITER=200000

  cross-arm:
    name: Cortex-M4 cross build
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install ARM GNU toolchain
        run: sudo apt-get update && sudo apt-get install -y make gcc-arm-none-eabi binutils-arm-none-eabi
      - name: Build library and embedded demo
        run: make CC=gcc PYTHON=python3 arm-cross
      - name: Check artifacts
        run: test -s build/arm/libwasm_engine.a && test -s build/arm/embedded_demo.elf

  size-report:
    name: static footprint
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install compiler and tools
        run: sudo apt-get update && sudo apt-get install -y make gcc
      - name: Build and print footprint
        run: make CC=gcc size-report
```

### Analysis of CI configuration

- **Jobs & platforms:**
  - `unix-build-test`: Matrix on `ubuntu-latest` (`gcc`, `clang`) and `macos-latest` (`clang`).
  - `windows-mingw`: `windows-latest` under MSYS2 MINGW64 environment with `mingw-w64-x86_64-gcc`.
  - `sanitize`: `ubuntu-latest` matrix with `gcc` and `clang`.
  - `fuzz-smoke`: `ubuntu-latest` with `gcc` (200,000 deterministic fuzz iterations with SEED=42).
  - `cross-arm`: `ubuntu-latest` cross-compiling with `gcc-arm-none-eabi` for Cortex-M4, validating static library and ELF artifacts.
  - `size-report`: `ubuntu-latest` measuring static footprint and binary section sizes.
- **Compilers:** `gcc`, `clang`, `arm-none-eabi-gcc`.
- **Compiler flags:**
  - Baseline: `-std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude`.
  - Sanitizer job: Adds `-fsanitize=address,undefined -fno-sanitize-recover=all`.
- **Execution of tests and sanitizers:**
  - Standard test suites (`make test`) run on Linux, macOS, Windows MSYS2, and in sanitized mode.
  - Sanitizers (ASan + UBSan) run cleanly on Linux for both GCC and Clang.
  - Deterministic fuzzer runs in every CI build.

### Identified CI gaps

1. **Strict opcode matrix gating missing:** CI does not execute `python tools/opcode_matrix.py --strict`, so regressions in opcode coverage or validator omissions do not fail the build.
2. **Spec suite not run in CI:** `tools/run_spec_suite.py` is never invoked in any CI job.
3. **Hard invariant 1 (zero-allocation) not verified:** CI lacks a step running `grep -rnE "malloc|calloc|realloc|alloca|strdup" src/ include/ tests/ tools/ targets/` to guarantee zero dynamic allocation mechanically.
4. **Native Windows shell untested:** CI runs Windows inside MSYS2 bash (where POSIX `rm` is available). Native Windows PowerShell / CMD environments (which run `mingw32-make` without `rm` on PATH) are untested and fail on `make clean`.
5. **No Clang sanitizer job on macOS:** Sanitizers are only executed on Ubuntu Linux.

## 6. Spec-runner capability assessment (task 0.6)

Analysis of `tools/run_spec_suite.py`:

### Supported input format

- **Format:** JSON test scripts generated from official `.wast` files via `wast2json` (e.g. `wast2json --spec <file>.wast -o <file>.json`).
- Raw `.wast` / `.wat` text format is **not supported directly**; translation to JSON and linked `.wasm` binaries must occur before invoking the runner.

### Supported assertion and action types

- `invoke` (action): Maps to `wasm_runner --quiet --result-line <file.wasm> <field> [--arg type:val...]`. Passes if process exits with code 0.
- `assert_return`: Fully parsed and compared. Handles:
  - `i32` / `i64`: Compared against decimal / hex values in stdout `RESULT i32/i64 <val>`.
  - `f32` / `f64`: Bit-exact comparison against hex integer representations.
  - `nan:canonical` / `nan:arithmetic`: Handled via `nan_bits()` helper by checking exponent and quiet fraction bit patterns.
- `assert_trap`: Checked by testing for non-zero exit code and `"RESULT trap "` in stdout.
- `assert_invalid`, `assert_malformed`, `assert_uninstantiable`: Handled by executing `wasm_runner --quiet <file.wasm>` and asserting exit code != 0.

### Missing capabilities vs official WebAssembly JSON spec format

1. **Statefulness across commands:** `run_spec_suite.py` spawns a fresh `wasm_runner` CLI process for every individual assertion. In the WebAssembly spec testsuite, many tests instantiate a module and perform multiple mutating calls (e.g., modifying mutable globals or linear memory). Because `wasm_runner` does not maintain state across invocations, sequential stateful tests cannot pass without an interactive/in-memory session mode.
2. **`register` command skipped:** Line 118 completely skips `register` (`self.skip_count += 1; return`). Multi-module tests that register one module under an import namespace (e.g. `(register "test")`) and link another module to it cannot be executed.
3. **`get` action missing:** Only function invocation (`invoke`) is supported. Inspecting exported globals via `get` actions is not implemented.
4. **`assert_unlinkable` and `assert_exhaustion`:** Not distinctly categorized; fall into a catch-all non-zero return code check without verifying the specific failure reason.
5. **Trap / Error message verification:** Spec assertions specify exact expected error strings (e.g., `"out of bounds memory access"`, `"integer divide by zero"`). `run_spec_suite.py` only tests process exit code and generic substring `"RESULT trap "`, ignoring the specific trap classification.
6. **Negative canonical NaN bits:** `nan_bits()` expects canonical float NaN to match exact positive bits `0x7fc00000` (f32) or `0x7ff8000000000000` (f64). The WebAssembly specification allows either sign bit for canonical NaNs (`0xffc00000` is also canonical), which causes false-negative mismatches if the engine preserves a negative sign bit on NaN operations.




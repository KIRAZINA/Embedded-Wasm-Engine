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

# Baseline Makefile — C11 strict, zero-dependency (libm only for floats).
# `make all`  : builds wasm_runner + all test binaries (phase1..10, cli).
# `make test` : builds everything, runs all suites sequentially, prints
#               per-suite results and a final TOTAL line
#               ("TOTAL: X passed, Y failed"); non-zero exit on failure.
# `make clean`: removes the build tree.
CC       ?= gcc
PYTHON   ?= python3
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
LDLIBS   ?= -lm
SRC       = src/leb128.c src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c \
            src/wasm_frame.c src/wasm_control.c src/wasm_vm.c src/wasm_host.c \
            src/wasm_validate.c
BUILD    ?= build
ifeq ($(OS),Windows_NT)
MKDIR_P   = if not exist $(BUILD) mkdir $(BUILD)
ARM_MKDIR = if not exist "$(ARM_BUILD)" mkdir "$(ARM_BUILD)"
else
MKDIR_P   = mkdir -p $(BUILD)
ARM_MKDIR = mkdir -p $(ARM_BUILD)
endif
RUNNER    = $(BUILD)/wasm_runner
ifeq ($(OS),Windows_NT)
EXE       = .exe
PARITY_RUNNER = $(RUNNER).exe
SPEC_RUNNER_BIN = $(BUILD)/wasm_runner_spec.exe
else
EXE       =
PARITY_RUNNER = $(RUNNER)
SPEC_RUNNER_BIN = $(BUILD)/wasm_runner_spec
endif
# Spec-runner build: same sources, raised fixed caps for the official
# suites (many small funcs per module; 1000-entry tables). Desktop/CI
# only; the shipping build keeps embedded-sized defaults. Caps mirror
# LIMITS.md -D overrides; the harness capacity flags must match these.
SPEC_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude \
              -DWASM_MAX_FUNCTIONS=512 -DWASM_MAX_TABLE_SIZE=1024
T1        = $(BUILD)/test_phase1
T2        = $(BUILD)/test_phase2
T3        = $(BUILD)/test_phase3
T4        = $(BUILD)/test_phase4
T5        = $(BUILD)/test_phase5
T6        = $(BUILD)/test_phase6
T7        = $(BUILD)/test_phase7
T8        = $(BUILD)/test_phase8
T9        = $(BUILD)/test_phase9
T10       = $(BUILD)/test_phase10
T11       = $(BUILD)/test_phase11
T12       = $(BUILD)/test_phase12
T13       = $(BUILD)/test_phase13
TCLI      = $(BUILD)/test_cli
FUZZ      = $(BUILD)/fuzz_smoke
SEED      ?= 1
ITER      ?= 1000000
ARM_CC    ?= arm-none-eabi-gcc
ARM_AR    ?= arm-none-eabi-ar
ARM_BUILD = $(BUILD)/arm
ARM_OBJECTS = $(patsubst src/%.c,$(ARM_BUILD)/%.o,$(SRC))
ARM_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude \
             -mcpu=cortex-m4 -mthumb -Os -ffreestanding -ffunction-sections -fdata-sections

all: $(RUNNER) $(T1) $(T2) $(T3) $(T4) $(T5) $(T6) $(T7) $(T8) $(T9) $(T10) $(T11) $(T12) $(T13) $(TCLI)

$(RUNNER): $(SRC) src/main.c include/version.h
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) src/main.c -o $(RUNNER) $(LDLIBS)

$(T1): src/leb128.c src/wasm_parser.c tests/test_phase1.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) src/leb128.c src/wasm_parser.c tests/test_phase1.c -o $(T1) $(LDLIBS)

$(T2): src/leb128.c src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c tests/test_phase2.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) src/leb128.c src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c tests/test_phase2.c -o $(T2) $(LDLIBS)

$(T3): $(SRC) tests/test_phase3.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase3.c -o $(T3) $(LDLIBS)

$(T4): $(SRC) tests/test_phase4.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase4.c -o $(T4) $(LDLIBS)

$(T5): $(SRC) tests/test_phase5.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase5.c -o $(T5) $(LDLIBS)

$(T6): $(SRC) tests/test_phase6.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase6.c -o $(T6) $(LDLIBS)

$(TCLI): tests/test_cli.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) tests/test_cli.c -o $(TCLI) $(LDLIBS)

$(T7): $(SRC) tests/test_phase7.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase7.c -o $(T7) $(LDLIBS)

$(T8): $(SRC) tests/test_phase8.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase8.c -o $(T8) $(LDLIBS)

$(T9): $(SRC) tests/test_phase9.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase9.c -o $(T9) $(LDLIBS)

$(T10): $(SRC) tests/test_phase10.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase10.c -o $(T10) $(LDLIBS)

$(T11): src/leb128.c src/wasm_parser.c src/wasm_validate.c tests/test_phase11.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) src/leb128.c src/wasm_parser.c src/wasm_validate.c tests/test_phase11.c -o $(T11) $(LDLIBS)

$(T12): tests/test_phase12.c include/version.h
	$(MKDIR_P)
	$(CC) $(CFLAGS) tests/test_phase12.c -o $(T12) $(LDLIBS)

$(T13): $(SRC) tests/test_phase13.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/test_phase13.c -o $(T13) $(LDLIBS)

$(FUZZ): $(SRC) tests/fuzz_smoke.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) $(SRC) tests/fuzz_smoke.c -o $(FUZZ) $(LDLIBS)

test: all
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -Command "$$pass=0; $$fail=0; $$tests=@('$(BUILD)/test_phase1.exe','$(BUILD)/test_phase2.exe','$(BUILD)/test_phase3.exe','$(BUILD)/test_phase4.exe','$(BUILD)/test_phase5.exe','$(BUILD)/test_phase6.exe','$(BUILD)/test_phase7.exe','$(BUILD)/test_phase8.exe','$(BUILD)/test_phase9.exe','$(BUILD)/test_phase10.exe','$(BUILD)/test_phase11.exe','$(BUILD)/test_phase12.exe','$(BUILD)/test_phase13.exe','$(BUILD)/test_cli.exe'); foreach ($$t in $$tests) { $$o=& $$t; $$o | Select-Object -Last 3; if ($$LASTEXITCODE -ne 0) { exit $$LASTEXITCODE }; $$m=$$o | Select-String 'RESULT: ([0-9]+) passed, ([0-9]+) failed'; if ($$m) { $$pass += [int]$$m.Matches[0].Groups[1].Value; $$fail += [int]$$m.Matches[0].Groups[2].Value } }; Write-Output ('TOTAL: {0} passed, {1} failed' -f $$pass,$$fail); if ($$fail -ne 0) { exit 1 }"
else
	@pass=0; fail=0; \
	for t in $(T1) $(T2) $(T3) $(T4) $(T5) $(T6) $(T7) $(T8) $(T9) $(T10) $(T11) $(T12) $(T13) $(TCLI); do \
	  echo "== $$t =="; \
	  out=`./$$t`; ec=$$?; \
	  echo "$$out" | tail -n 3; \
	  p=`echo "$$out" | sed -n 's/^RESULT: \([0-9][0-9]*\) passed, \([0-9][0-9]*\) failed$$/\1/p'`; \
	  f=`echo "$$out" | sed -n 's/^RESULT: \([0-9][0-9]*\) passed, \([0-9][0-9]*\) failed$$/\2/p'`; \
	  pass=$$((pass + $${p:-0})); fail=$$((fail + $${f:-0})); \
	  if [ $$ec -ne 0 ]; then echo "FAIL: $$t exited with status $$ec"; exit 1; fi; \
	done; \
	echo "TOTAL: $$pass passed, $$fail failed"; \
	test $$fail -eq 0
endif

test1: $(T1)
	./$(T1)

test2: $(T2)
	./$(T2)

test3: $(T3)
	./$(T3)

test4: $(T4)
	./$(T4)

test5: $(T5)
	./$(T5)

test6: $(T6)
	./$(T6)

test7: $(T7)
	./$(T7)

test8: $(T8)
	./$(T8)

test9: $(T9)
	./$(T9)

test10: $(T10)
	./$(T10)

test11: $(T11)
	./$(T11)

test12: $(T12)
	./$(T12)

test13: $(T13)
	./$(T13)

testcli: $(TCLI)
	./$(TCLI)

fuzz-smoke: $(FUZZ)
	./$(FUZZ) $(SEED) $(ITER)

parity: $(RUNNER)
	$(PYTHON) tools/parity_check.py --runner $(PARITY_RUNNER)

$(SPEC_RUNNER_BIN): $(SRC) src/main.c include/version.h
	$(MKDIR_P)
	$(CC) $(SPEC_CFLAGS) $(SRC) src/main.c -o $(SPEC_RUNNER_BIN) $(LDLIBS)

spec: $(SPEC_RUNNER_BIN)
	$(PYTHON) tools/run_spec_suite.py --runner $(SPEC_RUNNER_BIN) tests/spec/json/*.json

size-report: tools/size_report.c
	$(MKDIR_P)
	$(CC) $(CFLAGS) tools/size_report.c -o $(BUILD)/size_report $(LDLIBS)
	./$(BUILD)/size_report

targets/stm32/demo_add_wasm.c: demo/add.wasm tools/bin2c.py
	$(PYTHON) tools/bin2c.py demo/add.wasm targets/stm32/demo_add_wasm.c embedded_demo_wasm

$(ARM_BUILD)/%.o: src/%.c
	$(ARM_MKDIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

arm-cross: targets/stm32/demo_add_wasm.c $(ARM_OBJECTS)
	$(ARM_AR) rcs $(ARM_BUILD)/libwasm_engine.a $(ARM_OBJECTS)
	$(ARM_CC) $(ARM_CFLAGS) targets/stm32/embedded_demo.c targets/stm32/demo_add_wasm.c \
		$(ARM_BUILD)/libwasm_engine.a -specs=nosys.specs -Wl,--gc-sections,-Map=$(ARM_BUILD)/embedded_demo.map \
		-o $(ARM_BUILD)/embedded_demo.elf -lm

# ---- Phase 4: fuzzing ----------------------------------------------------
# Harnesses live in fuzz/. Every buffer in the fuzz drivers is a static
# array: the fuzz pipeline never allocates (invariant 1).
FUZZ_BUILD    = $(BUILD)/fuzz
FUZZ_CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Ifuzz
FUZZ_SRC      = $(SRC) fuzz/fuzz_pipeline.c
FUZZ_CORPUS   = $(FUZZ_BUILD)/corpus
FUZZ_RUNS     ?= 1000000
FUZZ_MAX_LEN  ?= 65536
MUT_ROUNDS    ?= 20
FUZZ_TOOL     ?= $(shell sh tools/detect_fuzz_tool.sh 2>/dev/null || echo loop)
ifeq ($(OS),Windows_NT)
  FUZZ_MKDIR = if not exist $(FUZZ_BUILD) mkdir $(FUZZ_BUILD)
  FUZZ_CORPUS_MKDIR = if not exist $(FUZZ_CORPUS) mkdir $(FUZZ_CORPUS)
else
  FUZZ_MKDIR = mkdir -p $(FUZZ_BUILD)
  FUZZ_CORPUS_MKDIR = mkdir -p $(FUZZ_CORPUS)
endif

$(FUZZ_BUILD)/parse_harness: $(FUZZ_SRC) fuzz/fuzz_pipeline.c fuzz/parse_harness.c
	$(FUZZ_MKDIR)
	$(CC) $(FUZZ_CFLAGS) $(FUZZ_SRC) fuzz/parse_harness.c -o $@ $(LDLIBS)

$(FUZZ_BUILD)/loop_driver: $(FUZZ_SRC) fuzz/fuzz_pipeline.c fuzz/loop_driver.c
	$(FUZZ_MKDIR)
	$(CC) $(FUZZ_CFLAGS) $(FUZZ_SRC) fuzz/loop_driver.c -o $@ $(LDLIBS)

# Sanitized variant of the loop driver (used for the fuzzing runs).
$(FUZZ_BUILD)/loop_driver_san: $(FUZZ_SRC) fuzz/fuzz_pipeline.c fuzz/loop_driver.c
	$(FUZZ_MKDIR)
	$(SAN_CC) $(FUZZ_CFLAGS) $(SAN_FLAGS) $(FUZZ_SRC) fuzz/loop_driver.c -o $@ $(LDLIBS)

$(FUZZ_BUILD)/libfuzzer_parse: $(FUZZ_SRC) fuzz/fuzz_pipeline.c fuzz/libfuzzer_entry.c
	$(FUZZ_MKDIR)
	clang $(FUZZ_CFLAGS) $(SAN_FLAGS) -fsanitize=fuzzer $(FUZZ_SRC) \
	      fuzz/libfuzzer_entry.c -o $@ $(LDLIBS)

$(FUZZ_BUILD)/afl_parse: $(FUZZ_SRC) fuzz/fuzz_pipeline.c fuzz/afl_entry.c
	$(FUZZ_MKDIR)
	afl-clang-fast $(FUZZ_CFLAGS) $(FUZZ_SRC) fuzz/afl_entry.c -o $@ $(LDLIBS)

fuzz-build: $(FUZZ_BUILD)/parse_harness $(FUZZ_BUILD)/loop_driver

# Seed corpus: the demo modules plus a spread sample of the official spec
# corpus (every 14th module, so the sample covers every suite instead of
# only the alphabetically first ones).
fuzz-corpus: fuzz-build
	$(FUZZ_CORPUS_MKDIR)
	cp demo/*.wasm $(FUZZ_CORPUS)/ 2>/dev/null || true
	@i=0; for f in $$(ls tests/spec/json/*.wasm 2>/dev/null); do \
	    i=$$((i+1)); \
	    if [ $$((i % 14)) -eq 0 ]; then cp "$$f" $(FUZZ_CORPUS)/ 2>/dev/null || true; fi; \
	done

# libFuzzer backend: the shared pipeline, seeded with the valid corpus.
fuzz-libfuzzer: fuzz-corpus $(FUZZ_BUILD)/libfuzzer_parse
	$(FUZZ_BUILD)/libfuzzer_parse $(FUZZ_CORPUS) -runs=$(FUZZ_RUNS) \
	    -max_len=$(FUZZ_MAX_LEN) -timeout=25 -rss_limit_mb=4096 \
	    -print_final_stats=1 -artifact_prefix=$(FUZZ_BUILD)/crash-

# AFL++ backend (persistent mode).
fuzz-afl: fuzz-corpus $(FUZZ_BUILD)/afl_parse
	AFL_SKIP_CPUFREQ=1 afl-fuzz -i $(FUZZ_CORPUS) -o $(FUZZ_BUILD)/afl-out \
	    -m none -t 25000 -- $(FUZZ_BUILD)/afl_parse

# Deterministic loop backend (always available, seed printed by the driver).
fuzz-loop: fuzz-build $(FUZZ_BUILD)/loop_driver_san
	$(FUZZ_BUILD)/loop_driver_san $(SEED) $(ITER) demo/*.wasm

# Backend detection: libFuzzer -> AFL++ -> deterministic loop.
fuzz:
	@echo "[fuzz] backend: $(FUZZ_TOOL)"
	@case "$(FUZZ_TOOL)" in \
	    libfuzzer) $(MAKE) fuzz-libfuzzer FUZZ_RUNS=$(FUZZ_RUNS) ;; \
	    afl) $(MAKE) fuzz-afl ;; \
	    *) $(MAKE) fuzz-loop SEED=$(SEED) ITER=$(ITER) ;; \
	esac

# Mutation driver: generate a mutant corpus from the valid modules and
# (optionally) replay every mutant through the C pipeline.
fuzz-mutate: fuzz-build
	$(PYTHON) fuzz/mutate.py --out $(FUZZ_CORPUS) --rounds $(MUT_ROUNDS) \
	    --runner $(FUZZ_BUILD)/parse_harness

# Hard invariant 1: no dynamic allocation anywhere in the tree.
no-alloc-check:
	sh tools/check_no_alloc.sh

.PHONY: fuzz-build fuzz-corpus fuzz-libfuzzer fuzz-afl fuzz-loop fuzz fuzz-mutate no-alloc-check

# ---- Phase 4: sanitizer builds -------------------------------------------
# Sanitizer toolchain: prefer clang when it is installed (it also provides
# libFuzzer for the fuzz targets), otherwise fall back to gcc. Override
# explicitly with `make asan CC=clang` / `make asan CC=gcc`.
ifeq ($(OS),Windows_NT)
SAN_CC ?= $(CC)
else
SAN_CC ?= $(shell command -v clang 2>/dev/null || echo gcc)
endif
SAN_CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
ASAN_FLAGS   = -fsanitize=address -fno-sanitize-recover=all
UBSAN_FLAGS  = -fsanitize=undefined -fno-sanitize-recover=all
SAN_FLAGS    = -fsanitize=address,undefined -fno-sanitize-recover=all
# Three representative spec suites for the sanitizer smoke run:
# float arithmetic, memory bounds, trap semantics.
SPEC_SMOKE   = tests/spec/json/f32.json tests/spec/json/memory_trap.json \
               tests/spec/json/traps.json
SPEC_SMOKE_CAPFLAGS = -DWASM_MAX_FUNCTIONS=512 -DWASM_MAX_TABLE_SIZE=1024

asan:
	$(MAKE) san-run NAME=asan SAN_CC=$(SAN_CC) SAN_FLAGS="$(ASAN_FLAGS)"

ubsan:
	$(MAKE) san-run NAME=ubsan SAN_CC=$(SAN_CC) SAN_FLAGS="$(UBSAN_FLAGS)"

sanitizers:
	$(MAKE) san-run NAME=sanitizers SAN_CC=$(SAN_CC) SAN_FLAGS="$(SAN_FLAGS)"

# Historical alias kept for existing CI invocations.
sanitize: sanitizers

# One sanitizer configuration end to end: build every source and test
# binary with the sanitizer flags into $(BUILD)/$(NAME), run the full
# test suite, then run the spec smoke suites through a sanitized runner.
san-run:
	@echo "[san] $(NAME): $(SAN_CC) $(SAN_FLAGS)"
	$(MAKE) BUILD=$(BUILD)/$(NAME) CC=$(SAN_CC) \
	        CFLAGS="$(SAN_CFLAGS) $(SAN_FLAGS)" all
	$(MAKE) BUILD=$(BUILD)/$(NAME) CC=$(SAN_CC) \
	        CFLAGS="$(SAN_CFLAGS) $(SAN_FLAGS)" test
	$(MAKE) BUILD=$(BUILD)/$(NAME) CC=$(SAN_CC) \
	        SPEC_CFLAGS="$(SAN_CFLAGS) $(SAN_FLAGS) $(SPEC_SMOKE_CAPFLAGS)" \
	        $(BUILD)/$(NAME)/wasm_runner_spec$(EXE)
	$(PYTHON) tools/run_spec_suite.py --runner $(BUILD)/$(NAME)/wasm_runner_spec$(EXE) $(SPEC_SMOKE)

runner: $(RUNNER)
	./$(RUNNER)

ifeq ($(OS),Windows_NT)
clean:
	rmdir /s /q $(BUILD) 2>nul || ver > nul
else
clean:
	rm -rf $(BUILD)
endif

.PHONY: all test test1 test2 test3 test4 test5 test6 test7 test8 test9 test10 test11 test12 test13 testcli runner fuzz parity spec size-report arm-cross asan ubsan sanitizers san-run sanitize clean

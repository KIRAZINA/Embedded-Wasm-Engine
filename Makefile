# Baseline Makefile — C11 strict, zero-dependency (libm only for floats).
# `make all`  : builds wasm_runner + all test binaries (phase1..10, cli).
# `make test` : builds everything, runs all suites sequentially, prints
#               per-suite results and a final TOTAL line
#               ("TOTAL: X passed, Y failed"); non-zero exit on failure.
# `make clean`: removes the build tree.
CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
LDLIBS   ?= -lm
SRC       = src/leb128.c src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c \
            src/wasm_frame.c src/wasm_control.c src/wasm_vm.c src/wasm_host.c \
            src/wasm_validate.c
BUILD     = build
ifeq ($(OS),Windows_NT)
MKDIR_P   = if not exist $(BUILD) mkdir $(BUILD)
else
MKDIR_P   = mkdir -p $(BUILD)
endif
RUNNER    = $(BUILD)/wasm_runner
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
TCLI      = $(BUILD)/test_cli

all: $(RUNNER) $(T1) $(T2) $(T3) $(T4) $(T5) $(T6) $(T7) $(T8) $(T9) $(T10) $(T11) $(TCLI)

$(RUNNER): $(SRC) src/main.c
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

test: all
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -Command "$$pass=0; $$fail=0; $$tests=@('build/test_phase1','build/test_phase2','build/test_phase3','build/test_phase4','build/test_phase5','build/test_phase6','build/test_phase7','build/test_phase8','build/test_phase9','build/test_phase10','build/test_phase11','build/test_cli'); foreach ($$t in $$tests) { $$o=& $$t; $$o | Select-Object -Last 3; if ($$LASTEXITCODE -ne 0) { exit $$LASTEXITCODE }; $$m=$$o | Select-String 'RESULT: ([0-9]+) passed, ([0-9]+) failed'; if ($$m) { $$pass += [int]$$m.Matches[0].Groups[1].Value; $$fail += [int]$$m.Matches[0].Groups[2].Value } }; Write-Output ('TOTAL: {0} passed, {1} failed' -f $$pass,$$fail); if ($$fail -ne 0) { exit 1 }"
else
	@pass=0; fail=0; \
	for t in $(T1) $(T2) $(T3) $(T4) $(T5) $(T6) $(T7) $(T8) $(T9) $(T10) $(T11) $(TCLI); do \
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

testcli: $(TCLI)
	./$(TCLI)

runner: $(RUNNER)
	./$(RUNNER)

clean:
	rm -rf $(BUILD)

.PHONY: all test test1 test2 test3 test4 test5 test6 test7 test8 test9 test10 test11 testcli runner clean

/*
 * test_phase10.c — Phase 4 verification: tables, elements, call_indirect.
 * Standalone, C11, assert-style. Modules hand-assembled with exact sizes.
 *
 * Covers: indirect dispatch (idx 0/last), table OOB (size/99/-1),
 * truncated/empty-stack/short-args immediates, same-arity wrong-type +
 * count/return mismatches, dangling typeidx, host imports (typed OK,
 * untyped fail-closed, trap propagation), table recursion (factorial),
 * element offsets/overwrite/OOB/funcidx/tableidx/trailing/no-table,
 * table malformed matrix (two/elemtype/max<min/over-cap/empty/count-0/
 * trailing/flags-2) + min-0/128/max==min edges, re-instantiate reset,
 * scanner interplay, CLI dispatch2 run.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $(SRC)
 *        tests/test_phase10.c -o build/test_phase10 $(LDLIBS)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_host.h"
#include "wasm_memory.h"
#include "wasm_parser.h"
#include "wasm_vm.h"
#include "harness.h"


static wasm_vm_t g_vm;
static wasm_host_registry_t g_reg;

static wasm_err_t do_instantiate(const uint8_t *mod, size_t len)
{
    wasm_module_t m;
    wasm_err_t ec = wasm_parse_header(mod, len, &m);
    if (ec != WASM_OK) {
        CHECK(0, "fixture parses");
        return ec;
    }
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm init");
    return wasm_instantiate(&g_vm, &m, &g_reg, NULL);
}

/* Run a snippet in a fresh frame on the CURRENT vm (keeps the table).
 * Drains leftover operands first (trapped indirects leave args behind by
 * construction: index pops, then the check traps), then drives the full
 * enter/run/exit protocol so nested callee frames unwind instead of
 * pending mid-execution. */
static void drain_stack(void)
{
    while (!wasm_stack_is_empty(&g_vm.operands)) {
        wasm_val_t drop;
        if (wasm_stack_pop(&g_vm.operands, &drop) != WASM_TRAP_OK) {
            break;
        }
    }
}

static wasm_trap_t run_snip(const uint8_t *code, size_t len)
{
    wasm_trap_t tr;
    wasm_frame_t *top = NULL;
    uint32_t base;
    drain_stack();
    tr = wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_attach_code(&g_vm, code, len);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    /* Full driver: unwind completed callees via exit_function so nested
     * calls (e.g. table recursion) actually return into their callers.
     * Stop at this snippet's own frame (depth captured at entry — older
     * pending frames from earlier snippets must not confuse the loop),
     * preserving its result values exactly like the old single run. */
    base = wasm_call_stack_depth(&g_vm.calls);
    for (;;) {
        tr = wasm_vm_run(&g_vm);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (wasm_call_stack_depth(&g_vm.calls) <= base) {
            return WASM_TRAP_OK;
        }
        tr = wasm_call_stack_top(&g_vm.calls, &top);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_vm_exit_function(&g_vm, top->return_count);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
    }
}

static void expect_i32(int32_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v);
    CHECK(tr == WASM_TRAP_OK && v.value.i32 == want, name);
}

static void expect_i64(int64_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I64, &v);
    CHECK(tr == WASM_TRAP_OK && v.value.i64 == want, name);
}

static void expect_f32_bits(uint32_t want, const char *name)
{
    wasm_val_t v;
    uint32_t got;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_F32, &v);
    memcpy(&got, &v.value.f32, sizeof(got));
    CHECK(tr == WASM_TRAP_OK && got == want, name);
}

/* M_AB: add/sub over (i32,i32)->i32 (t0), main unused here; table [0,1]. */
static const uint8_t kAB[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0E, 0x02, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
    0x60, 0x03, 0x7F, 0x7F, 0x7F, 0x01, 0x7F,
    0x03, 0x04, 0x03, 0x00, 0x00, 0x01,
    0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
    0x0A, 0x19, 0x03,
    0x06, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A,
    0x06, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6B,
    0x09, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x00, 0x11, 0x00,
    0x09, 0x08, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x02, 0x00, 0x01
};

static void test_dispatch(void)
{
    printf("[T] call_indirect dispatch + bounds\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kAB, sizeof(kAB)) == WASM_OK, "AB instantiates");
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x00,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run idx 0");
        expect_i32(14, "idx 0 adds");
    }
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x01,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run idx 1 (last)");
        expect_i32(6, "idx 1 subtracts");
    }
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x02,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_TABLE_OUT_OF_BOUNDS,
              "idx == size traps");
    }
    {
        static const uint8_t k[] = { 0x41, 0x63, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_TABLE_OUT_OF_BOUNDS,
              "idx 99 traps");
    }
    {
        static const uint8_t k[] = { 0x41, 0x7F, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_TABLE_OUT_OF_BOUNDS,
              "idx -1 (unsigned huge) traps");
    }
    {
        static const uint8_t k[] = { 0x11 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_CODE_OVERRUN,
              "truncated typeidx overruns");
    }
    {
        static const uint8_t k[] = { 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_STACK_UNDERFLOW,
              "empty stack underflows");
    }
    {
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INSUFFICIENT_ARGS,
              "sig ok but short args reports arity");
    }
}

/* M_SIG: A=(i32)->i32, B=(i64)->i64, C=()->i32, D=()->(), E=(f32)->f32,
 * F=(i64)->i64 identity-ish bodies; G=(i32,i64)->i32 entry only. */
static const uint8_t kSig[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x22, 0x07,
    0x60, 0x01, 0x7F, 0x01, 0x7F,
    0x60, 0x01, 0x7E, 0x01, 0x7E,
    0x60, 0x00, 0x01, 0x7F,
    0x60, 0x00, 0x00,
    0x60, 0x01, 0x7D, 0x01, 0x7D,
    0x60, 0x01, 0x7F, 0x01, 0x7F,
    0x60, 0x02, 0x7F, 0x7E, 0x01, 0x7F,
    0x03, 0x07, 0x06, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00,
    0x04, 0x04, 0x01, 0x70, 0x00, 0x06,
    0x0A, 0x17, 0x06,
    0x03, 0x00, 0x20, 0x00,
    0x03, 0x00, 0x20, 0x00,
    0x03, 0x00, 0x41, 0x00,
    0x01, 0x00,
    0x03, 0x00, 0x20, 0x00,
    0x03, 0x00, 0x20, 0x00,
    0x09, 0x0C, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x06,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x00
};

static void test_sig_matrix(void)
{
    printf("[T] indirect signature checks, element by element\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kSig, sizeof(kSig)) == WASM_OK, "SIG instantiates");
    {
        /* f0 via A with a real arg: full call works. */
        static const uint8_t k[] = { 0x41, 0x07, 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run f0 via A");
        expect_i32(7, "identity call returns arg");
    }
    {
        /* f0 via B: same counts, param type differs. */
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x01 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "i32 vs i64 param mismatch");
    }
    {
        /* f1 via A: param i64 vs i32. */
        static const uint8_t k[] = { 0x41, 0x01, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "i64 vs i32 param mismatch");
    }
    {
        /* f2 (()->i32) via D (()->()): return count differs. */
        static const uint8_t k[] = { 0x41, 0x02, 0x11, 0x03 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "return count mismatch");
    }
    {
        /* f3 (()->()) via C (()->i32): return count differs. */
        static const uint8_t k[] = { 0x41, 0x03, 0x11, 0x02 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "missing return mismatch");
    }
    {
        /* f4 via A: f32 vs i32. */
        static const uint8_t k[] = { 0x41, 0x04, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "f32 vs i32 mismatch");
    }
    {
        /* f0 via E: second param position is return; i32 vs f32. */
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x04 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "return i32 vs f32 mismatch");
    }
    {
        /* f0 via G (i32,i64)->i32: arity differs. */
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x06 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "arity mismatch");
    }
    {
        /* dangling typeidx fails closed. */
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x09 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "dangling typeidx mismatches");
    }
    {
        /* f2 via C with zero args: genuine void-arg call. */
        static const uint8_t k[] = { 0x41, 0x02, 0x11, 0x02 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run f2 via C");
        expect_i32(0, "nullary call returns const");
    }
    {
        /* f1 (i64)->i64 full call. */
        static const uint8_t k[] = { 0x42, 0x09, 0x41, 0x01, 0x11, 0x01 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run f1 via B");
        expect_i64(9, "i64 identity call");
    }
    {
        /* f4 (f32)->f32 full call. */
        static const uint8_t k[] = { 0x43, 0x00, 0x00, 0xC0, 0x3F,
                                     0x41, 0x04, 0x11, 0x04 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run f4 via E");
        expect_f32_bits(0x3FC00000u, "f32 identity call");
    }
}

/* M_OFF: table min 2, single element [f0] at offset 1. */
static const uint8_t kOff[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
    0x03, 0x02, 0x01, 0x00,
    0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
    0x0A, 0x08, 0x01, 0x06, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A,
    0x09, 0x07, 0x01, 0x00, 0x41, 0x01, 0x0B, 0x01, 0x00
};

static void test_offsets(void)
{
    printf("[T] element offsets + unset slots\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kOff, sizeof(kOff)) == WASM_OK, "OFF instantiates");
    {
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_UNDEFINED_ELEMENT,
              "unset slot 0 traps");
    }
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x01,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run slot 1");
        expect_i32(14, "offset-1 element dispatches");
    }
}

/* M_LOOP: factorial self-recursion through table slot 0. */
static const uint8_t kLoop[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,
    0x03, 0x02, 0x01, 0x00,
    0x04, 0x04, 0x01, 0x70, 0x00, 0x01,
    0x0A, 0x1A, 0x01, 0x18, 0x00,
    0x20, 0x00, 0x41, 0x01, 0x4C, 0x04, 0x7F, 0x41, 0x01, 0x05,
    0x20, 0x00, 0x20, 0x00, 0x41, 0x01, 0x6B, 0x41, 0x00, 0x11, 0x00,
    0x6C, 0x0B,
    0x09, 0x07, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00
};

static void test_recursion(void)
{
    printf("[T] recursion via the table (factorial)\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kLoop, sizeof(kLoop)) == WASM_OK, "LOOP links");
    {
        static const uint8_t k[] = { 0x41, 0x05, 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run fact(5)");
        expect_i32(120, "5! == 120");
    }
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run fact(10)");
        expect_i32(3628800, "10! == 3628800");
    }
    {
        static const uint8_t k[] = { 0x41, 0x00, 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run fact(0)");
        expect_i32(1, "0! == 1");
    }
    {
        static const uint8_t k[] = { 0x41, 0x01, 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run fact(1)");
        expect_i32(1, "1! == 1");
    }
}

static void test_table_malformed(void)
{
    static const uint8_t kTwo[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x07, 0x02, 0x70, 0x00, 0x01, 0x70, 0x00, 0x01
    };
    static const uint8_t kElemType[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x7F, 0x00, 0x01
    };
    static const uint8_t kMaxMin[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x05, 0x01, 0x70, 0x01, 0x02, 0x01
    };
    static const uint8_t kOverCap[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x05, 0x01, 0x70, 0x00, 0x81, 0x01
    };
    static const uint8_t kMin128[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x05, 0x01, 0x70, 0x00, 0x80, 0x01
    };
    static const uint8_t kEmpty[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x00
    };
    static const uint8_t kCount0[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x01, 0x00
    };
    static const uint8_t kTrailing[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x05, 0x01, 0x70, 0x00, 0x01, 0x00
    };
    static const uint8_t kFlags2[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x02, 0x01
    };
    static const uint8_t kMaxEq[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x05, 0x01, 0x70, 0x01, 0x02, 0x02
    };
    static const uint8_t kMin0[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x00
    };
    printf("[T] table section malformed matrix\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kTwo, sizeof(kTwo)) ==
          WASM_ERR_INVALID_SECTION, "two tables rejected");
    CHECK(do_instantiate(kElemType, sizeof(kElemType)) ==
          WASM_ERR_INVALID_SECTION, "non-funcref rejected");
    CHECK(do_instantiate(kMaxMin, sizeof(kMaxMin)) ==
          WASM_ERR_INVALID_SECTION, "max < min rejected");
    CHECK(do_instantiate(kOverCap, sizeof(kOverCap)) ==
          WASM_ERR_SECTION_OVERFLOW, "min 129 exceeds cap");
    CHECK(do_instantiate(kMin128, sizeof(kMin128)) == WASM_OK,
          "min 128 accepted");
    {
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_UNDEFINED_ELEMENT,
              "cap-size table slot empty");
    }
    {
        static const uint8_t k[] = { 0x41, 0xFF, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_UNDEFINED_ELEMENT,
              "idx 127 in bounds but slot empty");
    }
    CHECK(do_instantiate(kEmpty, sizeof(kEmpty)) ==
          WASM_ERR_LEB_TRUNCATED, "empty table section truncated");
    CHECK(do_instantiate(kCount0, sizeof(kCount0)) ==
          WASM_ERR_INVALID_SECTION, "count 0 rejected");
    CHECK(do_instantiate(kTrailing, sizeof(kTrailing)) ==
          WASM_ERR_INVALID_SECTION, "trailing byte rejected");
    CHECK(do_instantiate(kFlags2, sizeof(kFlags2)) ==
          WASM_ERR_INVALID_SECTION, "flags 2 rejected");
    CHECK(do_instantiate(kMaxEq, sizeof(kMaxEq)) == WASM_OK,
          "max == min accepted");
    CHECK(do_instantiate(kMin0, sizeof(kMin0)) == WASM_OK,
          "min 0 accepted");
    {
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_TABLE_OUT_OF_BOUNDS,
              "empty (size-0) table traps everything");
    }
}

/* Element fixtures bind no memory (vm->mem NULL from init). */
static void test_element_malformed(void)
{
    /* table min 2 + element [0] at offset 2. */
    static const uint8_t kOob[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
        0x09, 0x07, 0x01, 0x00, 0x41, 0x02, 0x0B, 0x01, 0x00
    };
    /* table min 2 + element @-1 (const -1). */
    static const uint8_t kNeg[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
        0x09, 0x07, 0x01, 0x00, 0x41, 0x7F, 0x0B, 0x01, 0x00
    };
    /* table min 2, funcidx 7, no funcs at all. */
    static const uint8_t kDangling[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
        0x09, 0x07, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x07
    };
    /* tableidx 1. */
    static const uint8_t kTableIdx[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
        0x09, 0x06, 0x01, 0x01, 0x41, 0x00, 0x0B, 0x00
    };
    /* no table section, one live entry. */
    static const uint8_t kNoTable[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0A, 0x03, 0x01, 0x01, 0x00,
        0x09, 0x07, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00
    };
    /* no table section, empty segment at 0. */
    static const uint8_t kNoTableEmpty[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x09, 0x06, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x00
    };
    printf("[T] element segments malformed matrix\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kOob, sizeof(kOob)) == WASM_ERR_ELEMENT_OUT_OF_BOUNDS,
          "offset past end fails");
    CHECK(do_instantiate(kNeg, sizeof(kNeg)) == WASM_ERR_ELEMENT_OUT_OF_BOUNDS,
          "offset -1 cannot wrap");
    CHECK(do_instantiate(kDangling, sizeof(kDangling)) ==
          WASM_ERR_INVALID_SECTION, "dangling funcidx rejected");
    CHECK(do_instantiate(kTableIdx, sizeof(kTableIdx)) ==
          WASM_ERR_INVALID_SECTION, "tableidx 1 rejected");
    CHECK(do_instantiate(kNoTable, sizeof(kNoTable)) ==
          WASM_ERR_INVALID_SECTION, "entries need a table");
    /* Spec: an element segment always needs a table ("unknown table"),
     * even when the segment is empty. */
    CHECK(do_instantiate(kNoTableEmpty, sizeof(kNoTableEmpty)) ==
          WASM_ERR_INVALID_SECTION, "empty segment needs a table too");
}

static void test_element_trailing(void)
{
    /* kOff plus one trailing zero byte in the element section. */
    static const uint8_t k[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
        0x03, 0x02, 0x01, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
        0x0A, 0x08, 0x01, 0x06, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A,
        0x09, 0x08, 0x01, 0x00, 0x41, 0x01, 0x0B, 0x01, 0x00, 0x00
    };
    printf("[T] element trailing bytes\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(k, sizeof(k)) == WASM_ERR_INVALID_SECTION,
          "trailing byte rejected");
}

static void test_import_table(void)
{
    static const uint8_t k[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x02, 0x0B, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x01, 0x74,
        0x01, 0x70, 0x00, 0x01
    };
    printf("[T] imported tables rejected\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(k, sizeof(k)) ==
          WASM_ERR_IMPORTED_TABLES_UNSUPPORTED, "table import rejected");
}

static int32_t g_add2_called = 0;

static wasm_trap_t test_add2(struct wasm_vm *vm, const wasm_val_t *args,
                             uint32_t nargs, wasm_val_t *rets, uint32_t nrets)
{
    (void)vm;
    if (args == NULL || rets == NULL || nargs < 2u || nrets < 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (args[0].type != WASM_TYPE_I32 || args[1].type != WASM_TYPE_I32) {
        return WASM_TRAP_TYPE_MISMATCH;
    }
    g_add2_called++;
    rets[0].type = WASM_TYPE_I32;
    rets[0].value.i32 = args[0].value.i32 + args[1].value.i32;
    return WASM_TRAP_OK;
}

static wasm_trap_t test_boom(struct wasm_vm *vm, const wasm_val_t *args,
                             uint32_t nargs, wasm_val_t *rets, uint32_t nrets)
{
    (void)vm;
    (void)args;
    (void)nargs;
    (void)rets;
    (void)nrets;
    return WASM_TRAP_DIV_BY_ZERO;
}

/* Import env.add2 (i32,i32)->i32 + table [0]. */
static const uint8_t kHost[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
    0x02, 0x0C, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x04, 0x61, 0x64, 0x64,
    0x32, 0x00, 0x00,
    0x04, 0x04, 0x01, 0x70, 0x00, 0x01,
    0x09, 0x07, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00
};

static void test_host_indirect(void)
{
    static const wasm_valtype_t kP2[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
    static const wasm_valtype_t kR1[] = { WASM_TYPE_I32 };
    static const wasm_valtype_t kBad[] = { WASM_TYPE_I64, WASM_TYPE_I64 };
    printf("[T] indirect calls to host imports\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "add2", kP2, 2u, kR1,
                                        1u, test_add2) == WASM_TRAP_OK,
          "typed registration ok");
    g_add2_called = 0;
    CHECK(do_instantiate(kHost, sizeof(kHost)) == WASM_OK, "host links");
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x00,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run indirect host");
        expect_i32(14, "host add2 via table");
    }
    CHECK(g_add2_called == 1, "callback fired once");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry reset");
    CHECK(wasm_host_register_func(&g_reg, "env", "add2", 2u, 1u,
                                  test_add2) == WASM_TRAP_OK,
          "untyped registration ok");
    CHECK(do_instantiate(kHost, sizeof(kHost)) == WASM_OK, "links untyped");
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x00,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "untyped host fails closed");
    }
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry reset 2");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "add2", kBad, 2u, kR1,
                                        1u, test_add2) == WASM_TRAP_OK,
          "wrongly-typed registration ok");
    CHECK(do_instantiate(kHost, sizeof(kHost)) == WASM_OK,
          "counts-only link accepts");
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x00,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INDIRECT_TYPE_MISMATCH,
              "element-wise check beats counts");
    }
}

static void test_host_trap_propagates(void)
{
    static const wasm_valtype_t kP[] = { WASM_TYPE_I32 };
    static const uint8_t kH[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x01, 0x7F, 0x00,
        0x02, 0x0C, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x04, 0x62, 0x6F, 0x6F,
        0x6D, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x01,
        0x09, 0x07, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00
    };
    printf("[T] host trap inside indirect propagates\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "boom", kP, 1u, NULL,
                                        0u, test_boom) == WASM_TRAP_OK,
          "register boom");
    CHECK(do_instantiate(kH, sizeof(kH)) == WASM_OK, "boom links");
    {
        static const uint8_t k[] = { 0x41, 0x01, 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_DIV_BY_ZERO,
              "host trap surfaces");
    }
}

static void test_typed_api_validation(void)
{
    static const wasm_valtype_t kP[] = { WASM_TYPE_I32 };
    static const wasm_valtype_t kBad[] = { (wasm_valtype_t)0x70 };
    printf("[T] typed registration validation\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "f", kBad, 1u, NULL,
                                        0u, test_add2) ==
          WASM_TRAP_INVALID_ARGUMENT, "bad param type rejected");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "f", kP, 1u, kBad, 1u,
                                        test_add2) == WASM_TRAP_INVALID_ARGUMENT,
          "bad return type rejected");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "f", NULL, 1u, NULL,
                                        0u, test_add2) == WASM_TRAP_NULL,
          "NULL types with params rejected");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "f", kP, 33u, NULL,
                                        0u, test_add2) ==
          WASM_TRAP_INVALID_ARGUMENT, "33rd param rejected");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "f", kP, 1u, NULL,
                                        0u, NULL) == WASM_TRAP_NULL,
          "NULL callback rejected");
}

/* M_OW: like AB but two overlapping segments; later wins (sub at 0). */
static const uint8_t kOw[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0E, 0x02, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
    0x60, 0x03, 0x7F, 0x7F, 0x7F, 0x01, 0x7F,
    0x03, 0x04, 0x03, 0x00, 0x00, 0x01,
    0x04, 0x04, 0x01, 0x70, 0x00, 0x02,
    0x0A, 0x19, 0x03,
    0x06, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A,
    0x06, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6B,
    0x09, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x00, 0x11, 0x00,
    0x09, 0x0D, 0x02,
    0x00, 0x41, 0x00, 0x0B, 0x01, 0x00,
    0x00, 0x41, 0x00, 0x0B, 0x01, 0x01
};

static void test_overwrite_order(void)
{
    printf("[T] later element segments overwrite earlier\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kOw, sizeof(kOw)) == WASM_OK, "OW instantiates");
    {
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x04, 0x41, 0x00,
                                     0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run overwritten slot");
        expect_i32(6, "second segment won");
    }
}

static void test_reinstantiate_reset(void)
{
    /* Table-less ()->() module: proves the previous table is gone. */
    static const uint8_t k[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0A, 0x03, 0x01, 0x01, 0x00
    };
    static const uint8_t kCall[] = { 0x41, 0x00, 0x11, 0x00 };
    printf("[T] re-instantiation resets the table\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kAB, sizeof(kAB)) == WASM_OK, "AB links");
    CHECK(do_instantiate(k, sizeof(k)) == WASM_OK, "plain links over it");
    CHECK(run_snip(kCall, sizeof(kCall)) == WASM_TRAP_TABLE_OUT_OF_BOUNDS,
          "old table gone (size 0)");
}

static void test_scanner_indirect(void)
{
    /* const 0; if { const 0; call_indirect 11 } ; end; const 9.
     * The 0x0B typeidx byte must be skipped as an immediate, not an END. */
    static const uint8_t k[] = { 0x41, 0x00, 0x04, 0x40, 0x41, 0x00,
                                 0x11, 0x0B, 0x0B, 0x41, 0x09 };
    printf("[T] scanner skips indirect immediates\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kAB, sizeof(kAB)) == WASM_OK, "AB links");
    CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run with skipped call");
    expect_i32(9, "landed on the true end");
}

static wasm_trap_t test_nine(struct wasm_vm *vm, const wasm_val_t *args,
                             uint32_t nargs, wasm_val_t *rets, uint32_t nrets)
{
    (void)vm;
    (void)args;
    (void)nargs;
    if (rets == NULL || nrets < 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    rets[0].type = WASM_TYPE_I32;
    rets[0].value.i32 = 9;
    return WASM_TRAP_OK;
}

static void test_typed_host_nullary(void)
{
    /* env.nine ()->i32 via typed registration, called with empty stack. */
    static const wasm_valtype_t kR[] = { WASM_TYPE_I32 };
    static const uint8_t kH[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        0x02, 0x0C, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x04, 0x6E, 0x69, 0x6E,
        0x65, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x01,
        0x09, 0x07, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00
    };
    printf("[T] nullary host indirect call\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_func_typed(&g_reg, "env", "nine", NULL, 0u, kR,
                                        1u, test_nine) == WASM_TRAP_OK,
          "register nullary");
    CHECK(do_instantiate(kH, sizeof(kH)) == WASM_OK, "nine links");
    {
        static const uint8_t k[] = { 0x41, 0x00, 0x11, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run nullary indirect");
        expect_i32(9, "nullary host returns 9");
    }
}

static void test_cli_dispatch2(void)
{
#ifdef _WIN32
    static const char *const fmt =
        "build\\wasm_runner.exe demo\\dispatch2.wasm main %d %d %d > build\\cli_d2_%d.txt";
#else
    static const char *const fmt =
        "./build/wasm_runner demo/dispatch2.wasm main %d %d %d > build/cli_d2_%d.txt";
#endif
    static const struct {
        int sel, a, b;
        const char *want;
    } rows[] = {
        { 0, 10, 4, "result: 14 (i32)" },
        { 1, 10, 4, "result: 6 (i32)" },
    };
    static char out[1024];
    char cmd[256];
    char path[64];
    size_t i, n;
    FILE *fp = NULL;
    printf("[T] CLI dispatch2 demo via call_indirect\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        sprintf(cmd, fmt, rows[i].sel, rows[i].a, rows[i].b, rows[i].sel);
        sprintf(path, "build/cli_d2_%d.txt", rows[i].sel);
        CHECK(system(cmd) == 0, "runner exits 0");
        fp = fopen(path, "rb");
        CHECK(fp != NULL, "output captured");
        if (fp == NULL) {
            return;
        }
        n = fread(out, 1u, sizeof(out) - 1u, fp);
        fclose(fp);
        out[n] = '\0';
        CHECK(strstr(out, rows[i].want) != NULL, "dispatch result correct");
    }
}

int main(void)
{
    static const harness_test_t tests[] = {
        { "dispatch", test_dispatch },
        { "sig_matrix", test_sig_matrix },
        { "offsets", test_offsets },
        { "recursion", test_recursion },
        { "table_malformed", test_table_malformed },
        { "element_malformed", test_element_malformed },
        { "element_trailing", test_element_trailing },
        { "import_table", test_import_table },
        { "host_indirect", test_host_indirect },
        { "host_trap_propagates", test_host_trap_propagates },
        { "typed_api_validation", test_typed_api_validation },
        { "overwrite_order", test_overwrite_order },
        { "reinstantiate_reset", test_reinstantiate_reset },
        { "scanner_indirect", test_scanner_indirect },
        { "typed_host_nullary", test_typed_host_nullary },
        { "cli_dispatch2", test_cli_dispatch2 },
    };
    printf("=== Phase 10 tests: tables, elements, call_indirect ===\n");
    return harness_run("phase10", tests,
                       sizeof(tests) / sizeof(tests[0]));
}

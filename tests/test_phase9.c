/*
 * test_phase9.c — globals, data segments and the start function.
 * Standalone, C11, assert-style. Modules are hand-assembled (runtime-built
 * only for the 32/33-count cap cases); multi-byte immediates use exact
 * hand-verified bytes.
 *
 * Covers: global round-trips (all 4 types); immutability + type-mismatch +
 * index traps; all 4 init const forms; non-const/empty/cut/bad-mut/
 * bad-type/mismatched init rejected; init from imported immutables
 * (mutable/defined refs rejected); global count cap; imported globals
 * (resolved/unresolved/type/mutability); data placement via loads,
 * multi-segment/empty/end-exact/end+1/wrap/memidx/cap/no-memory/global
 * offset/count-0 cases; start runs-once/wrong-sig/traps/oob-hidx/
 * host-start/with-locals; register_global validation; CLI hello demo.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $(SRC)
 *        tests/test_phase9.c -o build/test_phase9 $(LDLIBS)
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

/* The runner under test. The Makefile passes the path of the binary that
 * belongs to this build tree, so a sanitized tree exercises the sanitized
 * runner instead of the plain one. */
#ifndef WASM_TEST_RUNNER
#define WASM_TEST_RUNNER "./build/wasm_runner"
#endif


static wasm_vm_t g_vm;
static wasm_host_registry_t g_reg;
static wasm_memory_t g_mem;
static uint8_t g_arena[65536];

/* Parse + (optionally) bind pages of memory + instantiate. g_reg must be
 * prepared by the caller. Returns the instantiate code. */
static wasm_err_t do_instantiate(const uint8_t *mod, size_t len, int pages)
{
    wasm_module_t m;
    wasm_trap_t tr;
    wasm_err_t ec = wasm_parse_header(mod, len, &m);
    if (ec != WASM_OK) {
        CHECK(0, "fixture parses");
        return ec;
    }
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm init");
    if (pages >= 0) {
        tr = wasm_memory_init(&g_mem, g_arena, sizeof(g_arena),
                              (uint32_t)pages, (uint32_t)pages);
        if (tr != WASM_TRAP_OK) {
            CHECK(0, "memory init");
            return WASM_ERR_DATA_OUT_OF_BOUNDS;
        }
        CHECK(wasm_vm_bind_memory(&g_vm, &g_mem) == WASM_TRAP_OK,
              "memory bind");
    }
    return wasm_instantiate(&g_vm, &m, &g_reg, NULL);
}

/* Run a snippet in a fresh frame on the CURRENT vm (keeps globals). */
static wasm_trap_t run_snip(const uint8_t *code, size_t len)
{
    wasm_trap_t tr = wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_attach_code(&g_vm, code, len);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    return wasm_vm_run(&g_vm);
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

static uint32_t f32bits(float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static uint64_t f64bits(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static void expect_f32_bits(uint32_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_F32, &v);
    CHECK(tr == WASM_TRAP_OK && f32bits(v.value.f32) == want, name);
}

static void expect_f64_bits(uint64_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_F64, &v);
    CHECK(tr == WASM_TRAP_OK && f64bits(v.value.f64) == want, name);
}

static wasm_val_t mk_i32(int32_t v)
{
    wasm_val_t x;
    x.type = WASM_TYPE_I32;
    x.value.i32 = v;
    return x;
}

/* G1: 4 defined globals (i32mut=7, i64immut=-3, f32mut=1.5, f64immut=2.5)
 * + empty main. */
static const uint8_t kG1[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x06, 0x1F, 0x04,
    0x7F, 0x01, 0x41, 0x07, 0x0B,
    0x7E, 0x00, 0x42, 0x7D, 0x0B,
    0x7D, 0x01, 0x43, 0x00, 0x00, 0xC0, 0x3F, 0x0B,
    0x7C, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, 0x0B,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x03, 0x01, 0x01, 0x00
};

/* ---------- a) global round-trips, all four types ---------- */

static void test_global_get(void)
{
    printf("[T] global.get round-trips, all 4 types\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kG1, sizeof(kG1), -1) == WASM_OK, "G1 instantiates");
    {
        static const uint8_t k[] = { 0x23, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run get 0");
        expect_i32(7, "i32 global == 7");
    }
    {
        static const uint8_t k[] = { 0x23, 0x01 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run get 1");
        expect_i64(-3, "i64 global == -3");
    }
    {
        static const uint8_t k[] = { 0x23, 0x02 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run get 2");
        expect_f32_bits(0x3FC00000u, "f32 global == 1.5");
    }
    {
        static const uint8_t k[] = { 0x23, 0x03 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run get 3");
        expect_f64_bits(0x4004000000000000ULL, "f64 global == 2.5");
    }
}

static void test_global_set(void)
{
    printf("[T] global.set mutable round-trip\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kG1, sizeof(kG1), -1) == WASM_OK, "G1 instantiates");
    {
        static const uint8_t k[] = { 0x41, 0x2A, 0x24, 0x00, 0x23, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run set/get 0");
        expect_i32(42, "mutable i32 updated to 42");
    }
    {
        static const uint8_t k[] = { 0x43, 0x00, 0x00, 0x20, 0x40,
                                     0x24, 0x02, 0x23, 0x02 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run set/get 2");
        expect_f32_bits(0x40200000u, "mutable f32 updated to 2.5");
    }
}

static void test_global_traps(void)
{
    printf("[T] immutability, type mismatch, index traps\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kG1, sizeof(kG1), -1) == WASM_OK, "G1 instantiates");
    {
        static const uint8_t k[] = { 0x41, 0x01, 0x24, 0x01 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_GLOBAL_IMMUTABLE,
              "set immutable i64 traps");
    }
    {
        static const uint8_t k[] = { 0x43, 0x00, 0x00, 0x80, 0x3F,
                                     0x24, 0x03 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_GLOBAL_IMMUTABLE,
              "set immutable f64 traps");
    }
    {
        static const uint8_t k[] = { 0x42, 0x01, 0x24, 0x00 };
        wasm_val_t v;
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_TYPE_MISMATCH,
              "set i32 global with i64 traps");
        CHECK(wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I64, &v) ==
              WASM_TRAP_OK && v.value.i64 == 1, "operand preserved on top");
    }
    {
        static const uint8_t k[] = { 0x23, 0x04 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INVALID_ARGUMENT,
              "get idx == nglobals traps");
    }
    {
        static const uint8_t k[] = { 0x23, 0x63 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INVALID_ARGUMENT,
              "get idx 99 traps");
    }
    {
        static const uint8_t k[] = { 0x41, 0x00, 0x24, 0x04 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INVALID_ARGUMENT,
              "set idx == nglobals traps");
    }
    {
        /* 0xFFFFFFFF needs a 5-byte LEB but still just an index. */
        static const uint8_t k[] = { 0x41, 0x00, 0x24, 0xFF, 0xFF, 0xFF,
                                     0xFF, 0x0F };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_INVALID_ARGUMENT,
              "set idx 0xFFFFFFFF traps");
    }
}

/* ---------- b) init-expr validation ---------- */

static void test_init_forms(void)
{
    /* One global each, header + global section only. */
    static const uint8_t kAdd[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x09, 0x01, 0x7F, 0x01, 0x41, 0x01, 0x41, 0x02, 0x6A, 0x0B
    };
    static const uint8_t kTwo[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x08, 0x01, 0x7F, 0x01, 0x41, 0x01, 0x41, 0x02, 0x0B
    };
    static const uint8_t kEmpty[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x04, 0x01, 0x7F, 0x01, 0x0B
    };
    static const uint8_t kCut[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x05, 0x01, 0x7F, 0x01, 0x41, 0x05
    };
    static const uint8_t kMut[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x06, 0x01, 0x7F, 0x02, 0x41, 0x00, 0x0B
    };
    static const uint8_t kType[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x06, 0x01, 0x70, 0x00, 0x41, 0x00, 0x0B
    };
    static const uint8_t kMismatch[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x06, 0x01, 0x7F, 0x00, 0x42, 0x00, 0x0B
    };
    static const uint8_t kDefRef[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x06, 0x06, 0x01, 0x7F, 0x00, 0x23, 0x00, 0x0B
    };
    printf("[T] non-const init exprs rejected\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kAdd, sizeof(kAdd), -1) ==
          WASM_ERR_INVALID_SECTION, "operator init rejected");
    CHECK(do_instantiate(kTwo, sizeof(kTwo), -1) == WASM_ERR_INVALID_SECTION,
          "two-const init rejected");
    CHECK(do_instantiate(kEmpty, sizeof(kEmpty), -1) == WASM_ERR_INVALID_SECTION,
          "empty init rejected");
    CHECK(do_instantiate(kCut, sizeof(kCut), -1) == WASM_ERR_INVALID_SECTION,
          "unterminated init rejected");
    CHECK(do_instantiate(kMut, sizeof(kMut), -1) == WASM_ERR_INVALID_SECTION,
          "mutability 0x02 rejected");
    CHECK(do_instantiate(kType, sizeof(kType), -1) == WASM_ERR_INVALID_SECTION,
          "funcref global rejected");
    CHECK(do_instantiate(kMismatch, sizeof(kMismatch), -1) ==
          WASM_ERR_INVALID_SECTION, "init/type mismatch rejected");
    CHECK(do_instantiate(kDefRef, sizeof(kDefRef), -1) ==
          WASM_ERR_INVALID_SECTION, "defined-global ref rejected");
}

static void test_global_cap(void)
{
    /* count x `i32 immut = 0` (5 bytes each); section size backpatched
     * as a 2-byte ULEB (payloads 161/166 need it). */
    static uint8_t mod[192];
    size_t n, sizepos;
    uint32_t i, count;
    uint32_t paylen;
    printf("[T] global count cap\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    for (count = 33u; count >= 32u; count--) {
        n = 0u;
        mod[n++] = 0x00; mod[n++] = 0x61; mod[n++] = 0x73; mod[n++] = 0x6D;
        mod[n++] = 0x01; mod[n++] = 0x00; mod[n++] = 0x00; mod[n++] = 0x00;
        mod[n++] = 0x06;
        sizepos = n;
        mod[n++] = 0x00; mod[n++] = 0x00;
        mod[n++] = (uint8_t)count;
        for (i = 0u; i < count; i++) {
            mod[n++] = 0x7F; mod[n++] = 0x00; mod[n++] = 0x41;
            mod[n++] = 0x00; mod[n++] = 0x0B;
        }
        paylen = 1u + count * 5u;
        mod[sizepos] = (uint8_t)((paylen & 0x7Fu) | 0x80u);
        mod[sizepos + 1u] = (uint8_t)(paylen >> 7u);
        if (count == 33u) {
            CHECK(do_instantiate(mod, n, -1) == WASM_ERR_SECTION_OVERFLOW,
                  "33 globals exceed cap");
        } else {
            CHECK(do_instantiate(mod, n, -1) == WASM_OK, "32 globals fit");
            {
                static const uint8_t k[] = { 0x23, 0x1F };
                CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK,
                      "run get 31");
                expect_i32(0, "last global reads 0");
            }
        }
        if (count == 32u) {
            break; /* avoid uint32 wrap */
        }
    }
}

/* G3: import env.g (i32 immut) + main. */
static const uint8_t kG3[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x02, 0x0A, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x01, 0x67, 0x03, 0x7F, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x03, 0x01, 0x01, 0x00
};

/* G4: G3 + one defined global init from the import (global.get 0). */
static const uint8_t kG4[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x02, 0x0A, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x01, 0x67, 0x03, 0x7F, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x06, 0x06, 0x01, 0x7F, 0x00, 0x23, 0x00, 0x0B,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x03, 0x01, 0x01, 0x00
};

/* G5: like G4 but the defined init reads a MUTABLE import. */
static const uint8_t kG5[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x02, 0x0A, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x01, 0x6D, 0x03, 0x7F, 0x01,
    0x03, 0x02, 0x01, 0x00,
    0x06, 0x06, 0x01, 0x7F, 0x00, 0x23, 0x00, 0x0B,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x03, 0x01, 0x01, 0x00
};

static void test_import_global(void)
{
    wasm_val_t gv = mk_i32(42);
    printf("[T] imported globals resolved/unresolved/checked\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I32, 0,
                                    &gv) == WASM_TRAP_OK, "register env.g");
    CHECK(do_instantiate(kG3, sizeof(kG3), -1) == WASM_OK,
          "import-global module links");
    {
        static const uint8_t k[] = { 0x23, 0x00 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run get import");
        expect_i32(42, "imported value visible");
    }
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry reset");
    CHECK(do_instantiate(kG3, sizeof(kG3), -1) ==
          WASM_ERR_UNRESOLVED_IMPORT, "unregistered global rejected");
    {
        wasm_val_t v = mk_i32(9);
        CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I64,
                                        0, &v) == WASM_TRAP_INVALID_ARGUMENT,
              "register with mismatched value type rejected");
        v.type = WASM_TYPE_I32;
        CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I32,
                                        0, &v) == WASM_TRAP_OK,
              "register i32 env.g");
        (void)v;
    }
    {
        /* Import declares i32 but the module below wants i64: craft by
         * reusing kG3 against an i64 registration. */
        wasm_val_t w;
        w.type = WASM_TYPE_I64;
        w.value.i64 = 1;
        CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK,
              "registry reset 2");
        CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I64,
                                        0, &w) == WASM_TRAP_OK,
              "register i64 env.g");
        CHECK(do_instantiate(kG3, sizeof(kG3), -1) ==
              WASM_ERR_TYPE_MISMATCH_IMPORT, "valtype mismatch rejected");
    }
    {
        wasm_val_t w = mk_i32(7);
        CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK,
              "registry reset 3");
        CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I32,
                                        1, &w) == WASM_TRAP_OK,
              "register mutable env.g");
        CHECK(do_instantiate(kG3, sizeof(kG3), -1) ==
              WASM_ERR_TYPE_MISMATCH_IMPORT, "mutability mismatch rejected");
    }
}

/* G3m: import env.g (i32 MUTABLE) + main; no defined globals. */
static const uint8_t kG3m[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x02, 0x0A, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x01, 0x67, 0x03, 0x7F, 0x01,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x03, 0x01, 0x01, 0x00
};

static void test_import_mutable_set(void)
{
    /* kG3m against a mutable env.g = 5: set through the engine. */
    static const uint8_t kSet[] = { 0x41, 0x09, 0x24, 0x00, 0x23, 0x00 };
    static const uint8_t kGet[] = { 0x23, 0x00 };
    wasm_val_t w = mk_i32(5);
    printf("[T] mutable import set through the engine\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I32, 1,
                                    &w) == WASM_TRAP_OK, "register mutable");
    CHECK(do_instantiate(kG3m, sizeof(kG3m), -1) == WASM_OK, "links");
    CHECK(run_snip(kGet, sizeof(kGet)) == WASM_TRAP_OK, "run get");
    expect_i32(5, "initial mutable value");
    CHECK(run_snip(kSet, sizeof(kSet)) == WASM_TRAP_OK, "run set/get");
    expect_i32(9, "mutable import updated");
}

static void test_init_from_import(void)
{
    wasm_val_t w = mk_i32(42);
    printf("[T] defined-global init from imported immutable\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I32, 0,
                                    &w) == WASM_TRAP_OK, "register env.g");
    CHECK(do_instantiate(kG4, sizeof(kG4), -1) == WASM_OK,
          "init-from-import links");
    {
        static const uint8_t k[] = { 0x23, 0x01 };
        CHECK(run_snip(k, sizeof(k)) == WASM_TRAP_OK, "run get defined");
        expect_i32(42, "defined global copied the import");
    }
    w = mk_i32(1);
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry reset");
    CHECK(wasm_host_register_global(&g_reg, "env", "m", WASM_TYPE_I32, 1,
                                    &w) == WASM_TRAP_OK, "register mutable");
    CHECK(do_instantiate(kG5, sizeof(kG5), -1) == WASM_ERR_INVALID_SECTION,
          "init from mutable import rejected");
}

/* D1: memory(1) + 2 data segments + main. seg1 @0 = [65], seg2 @8 = [66,67]. */
static const uint8_t kD1[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x03, 0x01, 0x01, 0x00,
    0x0B, 0x17, 0x02,
    0x00, 0x41, 0x00, 0x0B, 0x04, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x41, 0x08, 0x0B, 0x08, 0x42, 0x00, 0x00, 0x00, 0x43, 0x00, 0x00,
    0x00
};

static void test_data_basic(void)
{
    static const uint8_t kL0[] = { 0x41, 0x00, 0x28, 0x00, 0x00 };
    static const uint8_t kL8[] = { 0x41, 0x08, 0x28, 0x00, 0x00 };
    static const uint8_t kL12[] = { 0x41, 0x0C, 0x28, 0x00, 0x00 };
    printf("[T] data segments placed, verified via loads\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kD1, sizeof(kD1), 1) == WASM_OK, "D1 instantiates");
    CHECK(run_snip(kL0, sizeof(kL0)) == WASM_TRAP_OK, "run load @0");
    expect_i32(65, "seg1 bytes at 0");
    CHECK(run_snip(kL8, sizeof(kL8)) == WASM_TRAP_OK, "run load @8");
    expect_i32(66, "seg2 first cell at 8");
    CHECK(run_snip(kL12, sizeof(kL12)) == WASM_TRAP_OK, "run load @12");
    expect_i32(67, "seg2 second cell at 12");
}

static void test_data_bounds(void)
{
    /* offset 65536 (end of 1 page), len 0 -> ok. */
    static const uint8_t kEndOk[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0B, 0x08, 0x01, 0x00, 0x41, 0x80, 0x80, 0x04, 0x0B, 0x00
    };
    /* Same offset, len 1 -> fail. */
    static const uint8_t kEndFail[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0B, 0x09, 0x01, 0x00, 0x41, 0x80, 0x80, 0x04, 0x0B, 0x01, 0xFF
    };
    /* offset 0, len 65537 -> fail. */
    static const uint8_t kBig[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0B, 0x08, 0x01, 0x00, 0x41, 0x00, 0x0B, 0x81, 0x80, 0x04
    };
    /* offset 0xFFFFFFFF (const -1), len 1 -> fail (no wraparound). */
    static const uint8_t kWrap[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0B, 0x07, 0x01, 0x00, 0x41, 0x7F, 0x0B, 0x01, 0xAA
    };
    /* memidx 1 -> invalid. */
    static const uint8_t kMemIdx[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0B, 0x06, 0x01, 0x01, 0x41, 0x00, 0x0B, 0x00
    };
    /* Empty segment at 16 -> ok. */
    static const uint8_t kEmpty[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0B, 0x06, 0x01, 0x00, 0x41, 0x10, 0x0B, 0x00
    };
    printf("[T] data bounds incl. end-exact and wraparound\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kEndOk, sizeof(kEndOk), 1) == WASM_OK,
          "offset == end, len 0 ok");
    CHECK(do_instantiate(kEndFail, sizeof(kEndFail), 1) ==
          WASM_ERR_DATA_OUT_OF_BOUNDS, "offset == end, len 1 fails");
    CHECK(do_instantiate(kBig, sizeof(kBig), 1) ==
          WASM_ERR_DATA_OUT_OF_BOUNDS, "len past end fails");
    CHECK(do_instantiate(kWrap, sizeof(kWrap), 1) ==
          WASM_ERR_DATA_OUT_OF_BOUNDS, "0xFFFFFFFF cannot wrap");
    CHECK(do_instantiate(kMemIdx, sizeof(kMemIdx), 1) ==
          WASM_ERR_INVALID_SECTION, "memidx 1 rejected");
    CHECK(do_instantiate(kEmpty, sizeof(kEmpty), 1) == WASM_OK,
          "empty segment ok");
    CHECK(do_instantiate(kD1, sizeof(kD1), -1) ==
          WASM_ERR_DATA_OUT_OF_BOUNDS, "segments need bound memory");
}

static void test_data_caps_misc(void)
{
    /* 33 empty segments (5 bytes each); size backpatched as 2-byte ULEB. */
    static uint8_t mod[192];
    size_t n = 0u, sizepos;
    uint32_t i;
    uint32_t paylen = 1u + 33u * 5u;
    /* count-0 data section, no memory bound -> ok. */
    static const uint8_t kZero[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x0B, 0x01, 0x00
    };
    printf("[T] data segment cap + misc\n");
    mod[n++] = 0x00; mod[n++] = 0x61; mod[n++] = 0x73; mod[n++] = 0x6D;
    mod[n++] = 0x01; mod[n++] = 0x00; mod[n++] = 0x00; mod[n++] = 0x00;
    mod[n++] = 0x0B;
    sizepos = n;
    mod[n++] = 0x00; mod[n++] = 0x00;
    mod[n++] = 0x21; /* count 33 */
    for (i = 0u; i < 33u; i++) {
        mod[n++] = 0x00;                          /* memidx */
        mod[n++] = 0x41; mod[n++] = 0x00; mod[n++] = 0x0B; /* offset 0 */
        mod[n++] = 0x00;                          /* len 0 */
    }
    mod[sizepos] = (uint8_t)((paylen & 0x7Fu) | 0x80u);
    mod[sizepos + 1u] = (uint8_t)(paylen >> 7u);
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(mod, n, 1) == WASM_ERR_SECTION_OVERFLOW,
          "33 segments exceed cap");
    CHECK(do_instantiate(kZero, sizeof(kZero), -1) == WASM_OK,
          "count-0 data needs no memory");
}

/* D2: import env.base (i32 immut 16); data offset = global.get 0. */
static const uint8_t kD2[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x02, 0x0D, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x04, 0x62, 0x61, 0x73,
    0x65, 0x03, 0x7F, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x03, 0x01, 0x01, 0x00,
    0x0B, 0x0A, 0x01, 0x00, 0x23, 0x00, 0x0B, 0x04, 0xDE, 0xAD, 0xBE,
    0xEF
};

static void test_data_global_offset(void)
{
    static const uint8_t kLoad[] = { 0x41, 0x10, 0x28, 0x00, 0x00 };
    wasm_val_t w = mk_i32(16);
    printf("[T] data offset from imported global\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_global(&g_reg, "env", "base", WASM_TYPE_I32,
                                    0, &w) == WASM_TRAP_OK, "register base");
    CHECK(do_instantiate(kD2, sizeof(kD2), 1) == WASM_OK,
          "global-offset data links");
    CHECK(run_snip(kLoad, sizeof(kLoad)) == WASM_TRAP_OK, "run load @16");
    expect_i32((int32_t)0xEFBEADDEu, "bytes landed at import-given offset");
}

/* S1: global mut i32 = 0; main ()->(); setter sets 99; start = setter. */
static const uint8_t kS1[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x03, 0x02, 0x00, 0x00,
    0x06, 0x06, 0x01, 0x7F, 0x01, 0x41, 0x00, 0x0B,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x0A, 0x02, 0x01, 0x00, 0x06, 0x00, 0x41, 0xE3, 0x00, 0x24,
    0x00,
    0x08, 0x01, 0x01
};

static void test_start_runs_once(void)
{
    static const uint8_t kGet[] = { 0x23, 0x00 };
    printf("[T] start runs once at instantiation\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kS1, sizeof(kS1), -1) == WASM_OK, "S1 instantiates");
    CHECK(run_snip(kGet, sizeof(kGet)) == WASM_TRAP_OK, "run get");
    expect_i32(99, "start already ran during instantiation");
    {
        /* main is func 0 with an empty body; run it via enter/attach. */
        wasm_trap_t tr = wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u);
        CHECK(tr == WASM_TRAP_OK, "enter main");
        CHECK(wasm_vm_attach_code(&g_vm, g_vm.funcs[0].code,
                                  g_vm.funcs[0].code_len) == WASM_TRAP_OK,
              "attach main");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run main");
        CHECK(wasm_vm_exit_function(&g_vm, 0u) == WASM_TRAP_OK,
              "exit main");
    }
    CHECK(run_snip(kGet, sizeof(kGet)) == WASM_TRAP_OK, "run get again");
    expect_i32(99, "start did not re-run");
}

static void test_start_rejects(void)
{
    /* start -> (i32)->() func. */
    static const uint8_t kSig1[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x08, 0x02, 0x60, 0x01, 0x7F, 0x00, 0x60, 0x00, 0x00,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
        0x0A, 0x05, 0x02, 0x01, 0x00, 0x01, 0x00,
        0x08, 0x01, 0x00
    };
    /* start -> ()->i32 func. */
    static const uint8_t kSig2[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x00, 0x01, 0x7F,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
        0x0A, 0x05, 0x02, 0x01, 0x00, 0x01, 0x00,
        0x08, 0x01, 0x01
    };
    /* start -> unreachable body. */
    static const uint8_t kTrap[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
        0x0A, 0x04, 0x01, 0x02, 0x00, 0x00,
        0x08, 0x01, 0x00
    };
    /* start -> 1/0 div-by-zero body. */
    static const uint8_t kDiv0[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
        0x0A, 0x08, 0x01, 0x06, 0x00, 0x41, 0x01, 0x41, 0x00, 0x6D,
        0x08, 0x01, 0x00
    };
    /* start idx 7 with 1 function. */
    static const uint8_t kOob[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
        0x0A, 0x03, 0x01, 0x01, 0x00,
        0x08, 0x01, 0x07
    };
    printf("[T] start signature/trap/index rejects\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kSig1, sizeof(kSig1), -1) ==
          WASM_ERR_TYPE_MISMATCH_IMPORT, "(i32)->() start rejected");
    CHECK(do_instantiate(kSig2, sizeof(kSig2), -1) ==
          WASM_ERR_TYPE_MISMATCH_IMPORT, "()->i32 start rejected");
    CHECK(do_instantiate(kTrap, sizeof(kTrap), -1) ==
          WASM_ERR_START_FAILED, "unreachable in start aborts");
    CHECK(do_instantiate(kDiv0, sizeof(kDiv0), -1) ==
          WASM_ERR_START_FAILED, "div-by-zero in start aborts");
    CHECK(do_instantiate(kOob, sizeof(kOob), -1) ==
          WASM_ERR_INVALID_SECTION, "start idx out of range rejected");
}

static int g_started = 0;

static wasm_trap_t test_start_cb(struct wasm_vm *vm, const wasm_val_t *args,
                                 uint32_t nargs, wasm_val_t *rets,
                                 uint32_t nrets)
{
    (void)vm;
    (void)args;
    (void)nargs;
    (void)rets;
    (void)nrets;
    g_started++;
    return WASM_TRAP_OK;
}

/* Imported env.init []->[] used as start. */
static const uint8_t kHostStart[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x08, 0x02, 0x60, 0x01, 0x7F, 0x00, 0x60, 0x00, 0x00,
    0x02, 0x0C, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x04, 0x69, 0x6E, 0x69,
    0x74, 0x00, 0x01,
    0x08, 0x01, 0x00
};

/* Imported env.bad (i32)->() used as start -> wrong signature. */
static const uint8_t kHostBad[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x01, 0x7F, 0x00,
    0x02, 0x0B, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x03, 0x62, 0x61, 0x64,
    0x00, 0x00,
    0x08, 0x01, 0x00
};

static wasm_trap_t test_bad_cb(struct wasm_vm *vm, const wasm_val_t *args,
                               uint32_t nargs, wasm_val_t *rets,
                               uint32_t nrets)
{
    (void)vm;
    (void)args;
    (void)nargs;
    (void)rets;
    (void)nrets;
    return WASM_TRAP_OK;
}

static void test_host_start(void)
{
    printf("[T] imported host function as start\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    g_started = 0;
    CHECK(wasm_host_register_func(&g_reg, "env", "init", 0u, 0u,
                                  test_start_cb) == WASM_TRAP_OK,
          "register env.init");
    CHECK(do_instantiate(kHostStart, sizeof(kHostStart), -1) == WASM_OK,
          "host start instantiates");
    CHECK(g_started == 1, "host start invoked exactly once");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry reset");
    CHECK(wasm_host_register_func(&g_reg, "env", "bad", 1u, 0u,
                                  test_bad_cb) == WASM_TRAP_OK,
          "register env.bad");
    CHECK(do_instantiate(kHostBad, sizeof(kHostBad), -1) ==
          WASM_ERR_TYPE_MISMATCH_IMPORT, "host start wrong sig rejected");
}

/* S2: start with a declared local; global ends at 6; data applied first:
 * data[32] = 10, start loads it. */
static const uint8_t kS2[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x06, 0x06, 0x01, 0x7F, 0x01, 0x41, 0x00, 0x0B,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x16, 0x01, 0x14, 0x01, 0x01, 0x7F, 0x41, 0x05, 0x21, 0x00,
    0x20, 0x00, 0x41, 0x01, 0x6A, 0x41, 0x20, 0x28, 0x00, 0x00, 0x6A,
    0x24, 0x00,
    0x0B, 0x0A, 0x01, 0x00, 0x41, 0x20, 0x0B, 0x04, 0x0A, 0x00, 0x00,
    0x00,
    0x08, 0x01, 0x00
};

static void test_start_locals_data_order(void)
{
    static const uint8_t kGet[] = { 0x23, 0x00 };
    printf("[T] start with locals sees data applied first\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(do_instantiate(kS2, sizeof(kS2), 1) == WASM_OK, "S2 instantiates");
    CHECK(run_snip(kGet, sizeof(kGet)) == WASM_TRAP_OK, "run get");
    expect_i32(16, "start: 5+1 locals-math + data[32]=10");
}

static void test_register_global_api(void)
{
    char name[40];
    uint32_t i;
    wasm_val_t v = mk_i32(1);
    printf("[T] register_global validation\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_global(NULL, "env", "g", WASM_TYPE_I32, 0,
                                    &v) == WASM_TRAP_NULL, "NULL registry");
    CHECK(wasm_host_register_global(&g_reg, "env", "g", (wasm_valtype_t)0x70,
                                    0, &v) == WASM_TRAP_INVALID_ARGUMENT,
          "bad valtype rejected");
    CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I32, 2,
                                    &v) == WASM_TRAP_INVALID_ARGUMENT,
          "mut 2 rejected");
    v.type = WASM_TYPE_I64;
    CHECK(wasm_host_register_global(&g_reg, "env", "g", WASM_TYPE_I32, 0,
                                    &v) == WASM_TRAP_INVALID_ARGUMENT,
          "value/type mismatch rejected");
    v = mk_i32(1);
    for (i = 0u; i < 16u; i++) {
        sprintf(name, "g%u", (unsigned)i);
        CHECK(wasm_host_register_global(&g_reg, "env", name, WASM_TYPE_I32,
                                        0, &v) == WASM_TRAP_OK, "fill table");
    }
    CHECK(wasm_host_register_global(&g_reg, "env", "full", WASM_TYPE_I32,
                                    0, &v) == WASM_TRAP_INVALID_ARGUMENT,
          "full table rejected");
}

static void test_cli_hello(void)
{
#ifdef _WIN32
    static const char *const cmd =
        WASM_TEST_RUNNER " demo\\hello.wasm main > build\\cli_hello.txt";
#else
    static const char *const cmd =
        WASM_TEST_RUNNER " demo/hello.wasm main > build/cli_hello.txt";
#endif
    static char out[1024];
    FILE *fp = NULL;
    size_t n = 0u;
    printf("[T] CLI hello demo (data + print loop)\n");
    CHECK(system(cmd) == 0, "hello exits 0");
    fp = fopen("build/cli_hello.txt", "rb");
    CHECK(fp != NULL, "output captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    CHECK(strstr(out, "72") != NULL, "prints 72 (H)");
    CHECK(strstr(out, "101") != NULL, "prints 101 (e)");
    CHECK(strstr(out, "108") != NULL, "prints 108 (l)");
    CHECK(strstr(out, "111") != NULL, "prints 111 (o)");
    CHECK(strstr(out, "ok (no result)") != NULL, "void main completes");
}

int main(void)
{
    static const harness_test_t tests[] = {
        { "global_get", test_global_get },
        { "global_set", test_global_set },
        { "global_traps", test_global_traps },
        { "init_forms", test_init_forms },
        { "global_cap", test_global_cap },
        { "import_global", test_import_global },
        { "import_mutable_set", test_import_mutable_set },
        { "init_from_import", test_init_from_import },
        { "data_basic", test_data_basic },
        { "data_bounds", test_data_bounds },
        { "data_caps_misc", test_data_caps_misc },
        { "data_global_offset", test_data_global_offset },
        { "start_runs_once", test_start_runs_once },
        { "start_rejects", test_start_rejects },
        { "host_start", test_host_start },
        { "start_locals_data_order", test_start_locals_data_order },
        { "register_global_api", test_register_global_api },
        { "cli_hello", test_cli_hello },
    };
    printf("=== Phase 9 tests: globals, data, start ===\n");
    return harness_run("phase9", tests,
                       sizeof(tests) / sizeof(tests[0]));
}

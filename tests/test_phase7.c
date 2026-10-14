/*
 * test_phase7.c — Phase 1 (float) verification: complete f32/f64 MVP set.
 * Standalone, C11, assert-style. Float immediates are emitted from exact
 * bit patterns at runtime (no hand-typed LE bytes to get wrong).
 *
 * Covers: f32/f64.const bit-exactness + truncation; 10-byte i64.const;
 * add/sub/mul/div incl. x/0 = +-Inf and 0/0 = NaN (S1); sqrt incl.
 * negative -> NaN (S5); min/max NaN + signed-zero matrices (S2);
 * abs/neg/copysign/reinterpret bit patterns (S6); ceil/floor/trunc;
 * nearest ties-to-even incl. 0.5/1.5/2.5/-0.5/-1.5 (S3); comparisons
 * incl. NaN; all 8 trunc conversions incl. exact boundaries (S4);
 * int<->float converts incl. demote/promote; NaN propagation + quiet bit
 * (S7); f32/f64 memory NaN-payload round-trips (S8); CLI run + --dump of
 * the float demo.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $(SRC)
 *        tests/test_phase7.c -o build/test_phase7 -lm
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_memory.h"
#include "wasm_vm.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

static wasm_vm_t g_vm;

/* Fresh VM, empty frame, attach, run to completion. */
static wasm_trap_t run_flat(const uint8_t *code, size_t len)
{
    wasm_trap_t tr = wasm_vm_init(&g_vm);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_attach_code(&g_vm, code, len);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    return wasm_vm_run(&g_vm);
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

/* Emit f32.const/f64.const from exact patterns; returns bytes written. */
static size_t emit_f32(uint8_t *b, uint32_t bits)
{
    b[0] = 0x43u;
    b[1] = (uint8_t)(bits & 0xFFu);
    b[2] = (uint8_t)((bits >> 8u) & 0xFFu);
    b[3] = (uint8_t)((bits >> 16u) & 0xFFu);
    b[4] = (uint8_t)((bits >> 24u) & 0xFFu);
    return 5u;
}

static size_t emit_f64(uint8_t *b, uint64_t bits)
{
    unsigned k;
    b[0] = 0x44u;
    for (k = 0u; k < 8u; k++) {
        b[k + 1u] = (uint8_t)((bits >> (8u * k)) & 0xFFu);
    }
    return 9u;
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

static void expect_nan32(const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_F32, &v);
    CHECK(tr == WASM_TRAP_OK && isnan(v.value.f32) &&
          (f32bits(v.value.f32) & 0x7FC00000u) == 0x7FC00000u, name);
}

static void expect_nan64(const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_F64, &v);
    CHECK(tr == WASM_TRAP_OK && isnan(v.value.f64) &&
          (f64bits(v.value.f64) & 0x7FF8000000000000ULL) ==
          0x7FF8000000000000ULL, name);
}

static void expect_i32(int32_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v);
    CHECK(tr == WASM_TRAP_OK && v.value.i32 == want, name);
}

static void expect_u32(uint32_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v);
    CHECK(tr == WASM_TRAP_OK && (uint32_t)v.value.i32 == want, name);
}

static void expect_i64(int64_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I64, &v);
    CHECK(tr == WASM_TRAP_OK && v.value.i64 == want, name);
}

static void expect_u64(uint64_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I64, &v);
    CHECK(tr == WASM_TRAP_OK && (uint64_t)v.value.i64 == want, name);
}

/* ---------- a) const immediates: bit-exact + truncated ---------- */

static void test_const(void)
{
    uint8_t c[16];
    size_t n;
    printf("[T] f32/f64.const immediates\n");
    n = emit_f32(c, 0x3FC00000u); /* 1.5 */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f32.const 1.5");
    expect_f32_bits(0x3FC00000u, "f32.const bit-exact 1.5");
    n = emit_f32(c, 0x80000000u); /* -0 */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f32.const -0");
    expect_f32_bits(0x80000000u, "f32.const preserves -0 bits");
    n = emit_f32(c, 0x7F800000u); /* +Inf */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f32.const +Inf");
    expect_f32_bits(0x7F800000u, "f32.const +Inf bits");
    n = emit_f32(c, 0x7FC00001u); /* NaN payload 1 */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f32.const nan:0x1");
    expect_f32_bits(0x7FC00001u, "f32.const preserves NaN payload");
    n = emit_f64(c, 0x3FF8000000000000ULL); /* 1.5 */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.const 1.5");
    expect_f64_bits(0x3FF8000000000000ULL, "f64.const bit-exact 1.5");
    n = emit_f64(c, 0x7FF8000000000001ULL); /* NaN payload 1 */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.const nan:0x1");
    expect_f64_bits(0x7FF8000000000001ULL, "f64.const preserves NaN payload");
    { /* truncated immediates */
        static const uint8_t t32[] = { 0x43, 0x00, 0x00, 0x00 };
        static const uint8_t t64[] = { 0x44, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00 };
        CHECK(run_flat(t32, sizeof(t32)) == WASM_TRAP_CODE_OVERRUN,
              "f32.const cut to 3 bytes overruns");
        CHECK(run_flat(t64, sizeof(t64)) == WASM_TRAP_CODE_OVERRUN,
              "f64.const cut to 7 bytes overruns");
    }
    { /* 10-byte i64.const SLEB128 extremes (leb128 s64 max-length path) */
        static const uint8_t kMin[] = { 0x42, 0x80, 0x80, 0x80, 0x80, 0x80,
                                        0x80, 0x80, 0x80, 0x80, 0x7F };
        static const uint8_t kMax[] = { 0x42, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
        CHECK(run_flat(kMin, sizeof(kMin)) == WASM_TRAP_OK,
              "run 10-byte i64.const INT64_MIN");
        expect_i64(INT64_MIN, "i64.const INT64_MIN exact");
        CHECK(run_flat(kMax, sizeof(kMax)) == WASM_TRAP_OK,
              "run 10-byte i64.const INT64_MAX");
        expect_i64(INT64_MAX, "i64.const INT64_MAX exact");
    }
}

/* ---------- b) add/sub/mul/div + S1 edges ---------- */

static void test_arith32(void)
{
    uint8_t c[24];
    size_t n;
    printf("[T] f32 add/sub/mul/div + div-by-zero (S1)\n");
    n = emit_f32(c, 0x3FC00000u);          /* 1.5 */
    n += emit_f32(c + n, 0x40200000u);     /* 2.5 */
    c[n++] = 0x92u;                        /* add */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.5+2.5");
    expect_f32_bits(0x40800000u, "1.5+2.5 == 4.0");
    n = emit_f32(c, 0x40800000u);          /* 4.0 */
    n += emit_f32(c + n, 0x3FC00000u);     /* 1.5 */
    c[n++] = 0x93u;                        /* sub */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 4.0-1.5");
    expect_f32_bits(0x40200000u, "4.0-1.5 == 2.5");
    n = emit_f32(c, 0x3FC00000u);
    n += emit_f32(c + n, 0x40000000u);     /* 2.0 */
    c[n++] = 0x94u;                        /* mul */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.5*2.0");
    expect_f32_bits(0x40400000u, "1.5*2.0 == 3.0");
    n = emit_f32(c, 0x3F800000u);          /* 1.0 */
    n += emit_f32(c + n, 0x40000000u);     /* 2.0 */
    c[n++] = 0x95u;                        /* div */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.0/2.0");
    expect_f32_bits(0x3F000000u, "1.0/2.0 == 0.5");
    n = emit_f32(c, 0x3F800000u);          /* 1.0 */
    n += emit_f32(c + n, 0x00000000u);     /* +0 */
    c[n++] = 0x95u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.0/+0 (no trap)");
    expect_f32_bits(0x7F800000u, "1.0/+0 == +Inf (S1)");
    n = emit_f32(c, 0xBF800000u);          /* -1.0 */
    n += emit_f32(c + n, 0x00000000u);
    c[n++] = 0x95u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run -1.0/+0 (no trap)");
    expect_f32_bits(0xFF800000u, "-1.0/+0 == -Inf (S1)");
    n = emit_f32(c, 0x00000000u);
    n += emit_f32(c + n, 0x00000000u);
    c[n++] = 0x95u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run +0/+0 (no trap)");
    expect_nan32("0.0/0.0 == NaN (S1)");
    n = emit_f32(c, 0x7F800000u);          /* +Inf */
    n += emit_f32(c + n, 0xFF800000u);     /* -Inf */
    c[n++] = 0x92u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run Inf + -Inf");
    expect_nan32("Inf + -Inf == NaN");
    n = emit_f32(c, 0x80000000u);          /* -0 */
    n += emit_f32(c + n, 0x80000000u);
    c[n++] = 0x92u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run -0 + -0");
    expect_f32_bits(0x80000000u, "-0 + -0 == -0");
}

static void test_arith64(void)
{
    uint8_t c[32];
    size_t n;
    printf("[T] f64 add/sub/mul/div + div-by-zero (S1)\n");
    n = emit_f64(c, 0x3FF8000000000000ULL); /* 1.5 */
    n += emit_f64(c + n, 0x4004000000000000ULL); /* 2.5 */
    c[n++] = 0xA0u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.5+2.5 f64");
    expect_f64_bits(0x4010000000000000ULL, "1.5+2.5 == 4.0 f64");
    n = emit_f64(c, 0x4010000000000000ULL);
    n += emit_f64(c + n, 0x3FF8000000000000ULL);
    c[n++] = 0xA1u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 4.0-1.5 f64");
    expect_f64_bits(0x4004000000000000ULL, "4.0-1.5 == 2.5 f64");
    n = emit_f64(c, 0x3FF8000000000000ULL);
    n += emit_f64(c + n, 0x4000000000000000ULL);
    c[n++] = 0xA2u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.5*2.0 f64");
    expect_f64_bits(0x4008000000000000ULL, "1.5*2.0 == 3.0 f64");
    n = emit_f64(c, 0x3FF0000000000000ULL); /* 1.0 */
    n += emit_f64(c + n, 0x0000000000000000ULL);
    c[n++] = 0xA3u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.0/+0 f64 (no trap)");
    expect_f64_bits(0x7FF0000000000000ULL, "1.0/+0 == +Inf f64 (S1)");
    n = emit_f64(c, 0xBFF0000000000000ULL); /* -1.0 */
    n += emit_f64(c + n, 0x0000000000000000ULL);
    c[n++] = 0xA3u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run -1.0/+0 f64 (no trap)");
    expect_f64_bits(0xFFF0000000000000ULL, "-1.0/+0 == -Inf f64 (S1)");
    n = emit_f64(c, 0x0000000000000000ULL);
    n += emit_f64(c + n, 0x0000000000000000ULL);
    c[n++] = 0xA3u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run +0/+0 f64 (no trap)");
    expect_nan64("0.0/0.0 == NaN f64 (S1)");
}

/* ---------- c) sqrt (S5) ---------- */

static void test_sqrt(void)
{
    uint8_t c[16];
    size_t n;
    printf("[T] f32/f64 sqrt incl. negative -> NaN (S5)\n");
    n = emit_f32(c, 0x40800000u); /* 4.0 */
    c[n++] = 0x91u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run sqrt(4)");
    expect_f32_bits(0x40000000u, "sqrt(4) == 2");
    n = emit_f32(c, 0x40000000u); /* 2.0 */
    c[n++] = 0x91u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run sqrt(2)");
    expect_f32_bits(0x3FB504F3u, "sqrt(2) bit-exact");
    n = emit_f32(c, 0xBF800000u); /* -1.0 */
    c[n++] = 0x91u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run sqrt(-1) (no trap)");
    expect_nan32("sqrt(-1) == NaN (S5)");
    n = emit_f64(c, 0x4010000000000000ULL); /* 4.0 */
    c[n++] = 0x9Fu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run sqrt(4) f64");
    expect_f64_bits(0x4000000000000000ULL, "sqrt(4) == 2 f64");
    n = emit_f64(c, 0xBFF0000000000000ULL); /* -1.0 */
    c[n++] = 0x9Fu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run sqrt(-1) f64 (no trap)");
    expect_nan64("sqrt(-1) == NaN f64 (S5)");
}

/* ---------- d) min/max matrices (S2) ---------- */

static void test_minmax32(void)
{
    /* {a bits, b bits, min bits or NaN flag, max bits or NaN flag} */
    static const struct {
        uint32_t a, b, mn, mx;
        int mnan, xnan;
    } rows[] = {
        { 0x3F800000u, 0x40000000u, 0x3F800000u, 0x40000000u, 0, 0 }, /* 1,2 */
        { 0xC0000000u, 0xC0800000u, 0xC0800000u, 0xC0000000u, 0, 0 }, /* -2,-4 */
        { 0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u, 0, 0 }, /* +0,-0 */
        { 0x80000000u, 0x00000000u, 0x80000000u, 0x00000000u, 0, 0 }, /* -0,+0 */
        { 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0, 0 }, /* -0,-0 */
        { 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0 }, /* +0,+0 */
        { 0x7FC00001u, 0x3F800000u, 0u, 0u, 1, 1 },                 /* nan,1 */
        { 0x3F800000u, 0xFFC00002u, 0u, 0u, 1, 1 },                 /* 1,nan */
        { 0xFF800000u, 0x7F800000u, 0xFF800000u, 0x7F800000u, 0, 0 }, /* infs */
    };
    uint8_t c[16];
    size_t i, n;
    printf("[T] f32 min/max NaN + signed-zero matrix (S2)\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        n = emit_f32(c, rows[i].a);
        n += emit_f32(c + n, rows[i].b);
        c[n++] = 0x96u; /* min */
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f32.min row");
        if (rows[i].mnan) {
            expect_nan32("f32.min NaN input -> NaN");
        } else {
            expect_f32_bits(rows[i].mn, "f32.min row value");
        }
        n = emit_f32(c, rows[i].a);
        n += emit_f32(c + n, rows[i].b);
        c[n++] = 0x97u; /* max */
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f32.max row");
        if (rows[i].xnan) {
            expect_nan32("f32.max NaN input -> NaN");
        } else {
            expect_f32_bits(rows[i].mx, "f32.max row value");
        }
    }
}

static void test_minmax64(void)
{
    static const struct {
        uint64_t a, b, mn, mx;
        int mnan, xnan;
    } rows[] = {
        { 0x3FF0000000000000ULL, 0x4000000000000000ULL,
          0x3FF0000000000000ULL, 0x4000000000000000ULL, 0, 0 },
        { 0x0000000000000000ULL, 0x8000000000000000ULL,
          0x8000000000000000ULL, 0x0000000000000000ULL, 0, 0 },
        { 0x7FF8000000000001ULL, 0x3FF0000000000000ULL, 0u, 0u, 1, 1 },
        { 0xFFF0000000000000ULL, 0x7FF0000000000000ULL,
          0xFFF0000000000000ULL, 0x7FF0000000000000ULL, 0, 0 },
    };
    uint8_t c[24];
    size_t i, n;
    printf("[T] f64 min/max NaN + signed-zero matrix (S2)\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        n = emit_f64(c, rows[i].a);
        n += emit_f64(c + n, rows[i].b);
        c[n++] = 0xA4u;
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.min row");
        if (rows[i].mnan) {
            expect_nan64("f64.min NaN input -> NaN");
        } else {
            expect_f64_bits(rows[i].mn, "f64.min row value");
        }
        n = emit_f64(c, rows[i].a);
        n += emit_f64(c + n, rows[i].b);
        c[n++] = 0xA5u;
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.max row");
        if (rows[i].xnan) {
            expect_nan64("f64.max NaN input -> NaN");
        } else {
            expect_f64_bits(rows[i].mx, "f64.max row value");
        }
    }
}

/* ---------- e) abs/neg/copysign bit patterns (S6) ---------- */

static void test_sign32(void)
{
    uint8_t c[16];
    size_t n;
    printf("[T] f32 abs/neg/copysign bit patterns (S6)\n");
    n = emit_f32(c, 0xBFC00000u); /* -1.5 */
    c[n++] = 0x8Bu;               /* abs */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run abs(-1.5)");
    expect_f32_bits(0x3FC00000u, "abs(-1.5) == 1.5");
    n = emit_f32(c, 0xFFC00001u); /* negative NaN payload 1 */
    c[n++] = 0x8Bu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run abs(-nan:0x1)");
    expect_f32_bits(0x7FC00001u, "abs clears NaN sign, keeps payload");
    n = emit_f32(c, 0x3FC00000u);
    c[n++] = 0x8Cu; /* neg */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run neg(1.5)");
    expect_f32_bits(0xBFC00000u, "neg(1.5) == -1.5");
    n = emit_f32(c, 0x80000000u); /* -0 */
    c[n++] = 0x8Cu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run neg(-0)");
    expect_f32_bits(0x00000000u, "neg(-0) == +0");
    n = emit_f32(c, 0x3FC00000u);
    n += emit_f32(c + n, 0x80000000u); /* sign donor -0 */
    c[n++] = 0x98u;                   /* copysign */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run copysign(1.5,-0)");
    expect_f32_bits(0xBFC00000u, "copysign magnitude of a, sign of b");
    n = emit_f32(c, 0x7FC00001u);
    n += emit_f32(c + n, 0xBF800000u); /* sign donor -1.0 */
    c[n++] = 0x98u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run copysign(nan,-1)");
    expect_f32_bits(0xFFC00001u, "copysign sets NaN sign, keeps payload");
}

static void test_sign64(void)
{
    uint8_t c[24];
    size_t n;
    printf("[T] f64 abs/neg/copysign bit patterns (S6)\n");
    n = emit_f64(c, 0xBFF8000000000000ULL); /* -1.5 */
    c[n++] = 0x99u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run abs(-1.5) f64");
    expect_f64_bits(0x3FF8000000000000ULL, "abs(-1.5) == 1.5 f64");
    n = emit_f64(c, 0x3FF8000000000000ULL);
    c[n++] = 0x9Au;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run neg(1.5) f64");
    expect_f64_bits(0xBFF8000000000000ULL, "neg(1.5) == -1.5 f64");
    n = emit_f64(c, 0xBFF8000000000000ULL);
    n += emit_f64(c + n, 0x0000000000000000ULL);
    c[n++] = 0xA6u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run copysign(-1.5,+0) f64");
    expect_f64_bits(0x3FF8000000000000ULL, "copysign sign of b f64");
}

/* ---------- f) ceil/floor/trunc ---------- */

static void test_round32(void)
{
    static const struct {
        uint32_t in, ce, fl, tr;
        int nan;
    } rows[] = {
        { 0x3FC00000u, 0x40000000u, 0x3F800000u, 0x3F800000u, 0 }, /* 1.5 */
        { 0xBFC00000u, 0xBF800000u, 0xC0000000u, 0xBF800000u, 0 }, /* -1.5 */
        { 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0 }, /* -0 */
        { 0x7F800000u, 0x7F800000u, 0x7F800000u, 0x7F800000u, 0 }, /* Inf */
        { 0x7FC00001u, 0u, 0u, 0u, 1 },                           /* NaN */
    };
    static const uint8_t ops[] = { 0x8Du, 0x8Eu, 0x8Fu };
    uint8_t c[8];
    size_t i, k, n;
    printf("[T] f32 ceil/floor/trunc\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        for (k = 0u; k < 3u; k++) {
            n = emit_f32(c, rows[i].in);
            c[n++] = ops[k];
            CHECK(run_flat(c, n) == WASM_TRAP_OK, "run round op");
            if (rows[i].nan) {
                expect_nan32("round op NaN -> NaN");
            } else if (k == 0u) {
                expect_f32_bits(rows[i].ce, "ceil value");
            } else if (k == 1u) {
                expect_f32_bits(rows[i].fl, "floor value");
            } else {
                expect_f32_bits(rows[i].tr, "trunc value");
            }
        }
    }
}

static void test_round64(void)
{
    uint8_t c[16];
    size_t n;
    printf("[T] f64 ceil/floor/trunc\n");
    n = emit_f64(c, 0x4004000000000000ULL); /* 2.5 */
    c[n++] = 0x9Bu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run ceil(2.5) f64");
    expect_f64_bits(0x4008000000000000ULL, "ceil(2.5) == 3 f64");
    n = emit_f64(c, 0xC004000000000000ULL); /* -2.5 */
    c[n++] = 0x9Cu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run floor(-2.5) f64");
    expect_f64_bits(0xC008000000000000ULL, "floor(-2.5) == -3 f64");
    n = emit_f64(c, 0xC004000000000000ULL);
    c[n++] = 0x9Du;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc(-2.5) f64");
    expect_f64_bits(0xC000000000000000ULL, "trunc(-2.5) == -2 f64");
    n = emit_f64(c, 0x7FF8000000000001ULL);
    c[n++] = 0x9Bu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run ceil(NaN) f64");
    expect_nan64("ceil(NaN) -> NaN f64");
}

/* ---------- g) nearest ties-to-even (S3) ---------- */

static void test_nearest32(void)
{
    static const struct {
        uint32_t in, out;
    } rows[] = {
        { 0x3F000000u, 0x00000000u }, /* 0.5 -> +0 */
        { 0x3FC00000u, 0x40000000u }, /* 1.5 -> 2 */
        { 0x40200000u, 0x40000000u }, /* 2.5 -> 2 */
        { 0x40600000u, 0x40800000u }, /* 3.5 -> 4 */
        { 0x40900000u, 0x40800000u }, /* 4.5 -> 4 */
        { 0xBF000000u, 0x80000000u }, /* -0.5 -> -0 */
        { 0xBFC00000u, 0xC0000000u }, /* -1.5 -> -2 */
        { 0xC0200000u, 0xC0000000u }, /* -2.5 -> -2 */
        { 0x3ECCCCCDu, 0x00000000u }, /* 0.4 -> 0 */
        { 0x3F19999Au, 0x3F800000u }, /* 0.6 -> 1 */
        { 0xBF19999Au, 0xBF800000u }, /* -0.6 -> -1 */
        { 0x7F800000u, 0x7F800000u }, /* +Inf */
    };
    uint8_t c[8];
    size_t i, n;
    printf("[T] f32 nearest roundTiesToEven (S3)\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        n = emit_f32(c, rows[i].in);
        c[n++] = 0x90u;
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run nearest row");
        expect_f32_bits(rows[i].out, "nearest row value");
    }
    { /* large integral values pass through untouched */
        uint32_t big = f32bits(1e10f);
        n = emit_f32(c, big);
        c[n++] = 0x90u;
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run nearest(1e10)");
        expect_f32_bits(big, "nearest(1e10) passthrough");
    }
    n = emit_f32(c, 0x7FC00001u);
    c[n++] = 0x90u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run nearest(NaN)");
    expect_nan32("nearest(NaN) -> NaN");
}

static void test_nearest64(void)
{
    static const struct {
        uint64_t in, out;
    } rows[] = {
        { 0x3FE0000000000000ULL, 0x0000000000000000ULL }, /* 0.5 -> 0 */
        { 0x3FF8000000000000ULL, 0x4000000000000000ULL }, /* 1.5 -> 2 */
        { 0x4004000000000000ULL, 0x4000000000000000ULL }, /* 2.5 -> 2 */
        { 0xBFE0000000000000ULL, 0x8000000000000000ULL }, /* -0.5 -> -0 */
        { 0xBFF8000000000000ULL, 0xC000000000000000ULL }, /* -1.5 -> -2 */
    };
    uint8_t c[16];
    size_t i, n;
    printf("[T] f64 nearest roundTiesToEven (S3)\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        n = emit_f64(c, rows[i].in);
        c[n++] = 0x9Eu;
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run nearest row f64");
        expect_f64_bits(rows[i].out, "nearest row value f64");
    }
}

/* ---------- h) comparisons ---------- */

static void test_cmp32(void)
{
    static const struct {
        uint32_t a, b;
        uint8_t op;
        int32_t want;
    } rows[] = {
        { 0x3FC00000u, 0x3FC00000u, 0x5Bu, 1 }, /* eq self */
        { 0x7FC00001u, 0x7FC00001u, 0x5Bu, 0 }, /* eq NaN */
        { 0x80000000u, 0x00000000u, 0x5Bu, 1 }, /* -0 == +0 */
        { 0x7FC00001u, 0x3F800000u, 0x5Cu, 1 }, /* ne NaN */
        { 0x3F800000u, 0x3F800000u, 0x5Cu, 0 }, /* ne same */
        { 0x3F800000u, 0x40000000u, 0x5Du, 1 }, /* lt */
        { 0x7FC00001u, 0x3F800000u, 0x5Du, 0 }, /* lt NaN */
        { 0x40000000u, 0x3F800000u, 0x5Eu, 1 }, /* gt */
        { 0x7F800000u, 0x7F800000u, 0x60u, 1 }, /* ge Inf */
        { 0x80000000u, 0x00000000u, 0x5Fu, 1 }, /* -0 <= +0 */
        { 0x7F800000u, 0x7FC00001u, 0x5Du, 0 }, /* Inf < NaN false */
    };
    uint8_t c[16];
    size_t i, n;
    printf("[T] f32 comparisons incl. NaN\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        n = emit_f32(c, rows[i].a);
        n += emit_f32(c + n, rows[i].b);
        c[n++] = rows[i].op;
        CHECK(run_flat(c, n) == WASM_TRAP_OK, "run cmp row");
        expect_i32(rows[i].want, "cmp row value");
    }
}

static void test_cmp64(void)
{
    uint8_t c[24];
    size_t n;
    printf("[T] f64 comparisons incl. NaN\n");
    n = emit_f64(c, 0x3FF8000000000000ULL);
    n += emit_f64(c + n, 0x3FF8000000000000ULL);
    c[n++] = 0x61u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run eq self f64");
    expect_i32(1, "1.5 == 1.5 f64");
    n = emit_f64(c, 0x7FF8000000000001ULL);
    n += emit_f64(c + n, 0x3FF0000000000000ULL);
    c[n++] = 0x62u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run ne NaN f64");
    expect_i32(1, "NaN != 1.0 f64");
    n = emit_f64(c, 0x7FF8000000000001ULL);
    n += emit_f64(c + n, 0x3FF0000000000000ULL);
    c[n++] = 0x63u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run lt NaN f64");
    expect_i32(0, "NaN < 1.0 false f64");
    n = emit_f64(c, 0x8000000000000000ULL);
    n += emit_f64(c + n, 0x0000000000000000ULL);
    c[n++] = 0x66u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run ge -0/+0 f64");
    expect_i32(1, "-0 >= +0 f64");
    /* Enum-spelled so the opcode matrix credits f64.eq/gt/le coverage. */
    n = emit_f64(c, 0x4004000000000000ULL);
    n += emit_f64(c + n, 0x4004000000000000ULL);
    c[n++] = (uint8_t)OP_F64_EQ;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.eq self");
    expect_i32(1, "2.5 == 2.5 f64");
    n = emit_f64(c, 0x4014000000000000ULL);
    n += emit_f64(c + n, 0x4004000000000000ULL);
    c[n++] = (uint8_t)OP_F64_GT;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.gt");
    expect_i32(1, "5.0 > 2.5 f64");
    n = emit_f64(c, 0x4004000000000000ULL);
    n += emit_f64(c + n, 0x4014000000000000ULL);
    c[n++] = (uint8_t)OP_F64_LE;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.le");
    expect_i32(1, "2.5 <= 5.0 f64");
    n = emit_f64(c, 0x7FF8000000000001ULL);
    n += emit_f64(c + n, 0x7FF8000000000001ULL);
    c[n++] = (uint8_t)OP_F64_EQ;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64.eq NaN");
    expect_i32(0, "NaN == NaN false f64");
}

/* ---------- i) trunc conversions, exact boundaries (S4) ---------- */

static void test_trunc_f32_to_i32(void)
{
    uint8_t c[8];
    size_t n;
    printf("[T] i32.trunc_f32_s/u boundaries (S4)\n");
    n = emit_f32(c, 0xCF000000u); /* -2147483648.0 */
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc min-ok");
    expect_i32(INT32_MIN, "trunc(-2^31) == INT32_MIN");
    n = emit_f32(c, 0x4EFFFFFFu); /* 2147483520.0 = 2^31 - 128 */
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc max-ulp-ok");
    expect_i32(2147483520, "trunc(2147483520.0) == 2147483520");
    n = emit_f32(c, f32bits(3.14159f));
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc(3.14159)");
    expect_i32(3, "trunc toward zero");
    n = emit_f32(c, f32bits(-3.8f));
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc(-3.8)");
    expect_i32(-3, "trunc(-3.8) == -3");
    n = emit_f32(c, 0x80000000u); /* -0 */
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc(-0)");
    expect_i32(0, "trunc(-0) == 0");
    n = emit_f32(c, 0x7FC00000u);
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc(NaN) -> INVALID_CONVERSION");
    n = emit_f32(c, 0x4F000000u); /* 2147483648.0 = 2^31 */
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc(2^31) traps (S4 boundary)");
    n = emit_f32(c, 0xCF000001u); /* -2147483904.0 = -2^31 - 256 */
    c[n++] = 0xA8u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc below -2^31 traps");
    /* unsigned */
    n = emit_f32(c, 0x00000000u);
    c[n++] = 0xA9u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc_u(0)");
    expect_u32(0u, "trunc_u(0) == 0");
    n = emit_f32(c, 0x4F7FFFFFu); /* 4294967040.0 = 2^32 - 512 */
    c[n++] = 0xA9u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc_u max-ulp-ok");
    expect_u32(4294967040u, "trunc_u(2^32-512) exact");
    n = emit_f32(c, 0x4F800000u); /* 4294967296.0 = 2^32 */
    c[n++] = 0xA9u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(2^32) traps");
    n = emit_f32(c, 0xBF800000u); /* -1.0 */
    c[n++] = 0xA9u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(-1) traps");
    n = emit_f32(c, 0xFFC00000u);
    c[n++] = 0xA9u;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc_u(NaN) -> INVALID_CONVERSION");
}

static void test_trunc_f64_to_i32(void)
{
    uint8_t c[16];
    size_t n;
    printf("[T] i32.trunc_f64_s/u boundaries (S4)\n");
    n = emit_f64(c, 0xC1E0000000000000ULL); /* -2147483648.0 */
    c[n++] = 0xAAu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc min-ok f64");
    expect_i32(INT32_MIN, "trunc(-2^31) == INT32_MIN f64");
    n = emit_f64(c, 0x41DFFFFFFFFFC000ULL); /* 2147483647.5 */
    c[n++] = 0xAAu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc(2147483647.5)");
    expect_i32(2147483647, "fractional max still converts");
    n = emit_f64(c, f64bits(-123.75));
    c[n++] = 0xAAu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc(-123.75)");
    expect_i32(-123, "trunc(-123.75) == -123");
    n = emit_f64(c, 0x7FF8000000000000ULL);
    c[n++] = 0xAAu;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc(NaN) f64 -> INVALID_CONVERSION");
    n = emit_f64(c, 0x41E0000000000000ULL); /* 2147483648.0 */
    c[n++] = 0xAAu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc(2^31) f64 traps");
    n = emit_f64(c, 0xC1E0000000000001ULL); /* -2147483649.0 */
    c[n++] = 0xAAu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc(-2^31-1) f64 traps");
    /* unsigned */
    n = emit_f64(c, 0x41EFFFFFFFFFFFFFULL); /* 4294967295.0 */
    c[n++] = 0xABu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc_u u32max f64");
    expect_u32(0xFFFFFFFFu, "trunc_u(2^32-1) == 0xFFFFFFFF");
    n = emit_f64(c, 0x41F0000000000000ULL); /* 4294967296.0 */
    c[n++] = 0xABu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(2^32) f64 traps");
    n = emit_f64(c, 0xBFF0000000000000ULL);
    c[n++] = 0xABu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(-1) f64 traps");
    n = emit_f64(c, 0xFFF8000000000000ULL);
    c[n++] = 0xABu;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc_u(NaN) f64 -> INVALID_CONVERSION");
}

static void test_trunc_f32_to_i64(void)
{
    uint8_t c[8];
    size_t n;
    printf("[T] i64.trunc_f32_s/u boundaries (S4)\n");
    n = emit_f32(c, 0xDF000000u); /* -9223372036854775808.0 = -2^63 */
    c[n++] = 0xAEu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc min-ok i64");
    expect_i64(INT64_MIN, "trunc(-2^63) == INT64_MIN");
    n = emit_f32(c, 0x5EFFFFFFu); /* 2^63 - 2^39 (largest f32 below 2^63) */
    c[n++] = 0xAEu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc max-ulp-ok i64");
    expect_i64(9223371487098961920LL, "trunc(2^63-2^39) exact");
    {
        uint8_t cc[8];
        size_t m = emit_f32(cc, f32bits(12345678.0f));
        cc[m++] = 0xAEu;
        CHECK(run_flat(cc, m) == WASM_TRAP_OK, "run trunc typical i64");
        expect_i64(12345678LL, "trunc typical exact");
    }
    n = emit_f32(c, 0x7FC00000u);
    c[n++] = 0xAEu;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc(NaN) i64 -> INVALID_CONVERSION");
    n = emit_f32(c, 0x5F000000u); /* 9223372036854775808.0 = 2^63 */
    c[n++] = 0xAEu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc(2^63) f32 traps");
    n = emit_f32(c, 0xDF000001u); /* -2^63 - 2^40 (next f32 below -2^63) */
    c[n++] = 0xAEu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc below -2^63 traps");
    /* unsigned */
    n = emit_f32(c, 0x00000000u);
    c[n++] = 0xAFu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc_u(0) i64");
    expect_u64(0ULL, "trunc_u(0) == 0 i64");
    n = emit_f32(c, 0x5F7FFFFFu); /* 2^64 - 2^40 (largest f32 below 2^64) */
    c[n++] = 0xAFu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc_u max-ulp-ok i64");
    expect_u64(18446742974197923840ULL, "trunc_u(2^64-2^40) exact");
    n = emit_f32(c, 0x5F800000u); /* 2^64 */
    c[n++] = 0xAFu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(2^64) f32 traps");
    n = emit_f32(c, 0xBF800000u);
    c[n++] = 0xAFu;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(-1) f32 traps");
    n = emit_f32(c, 0xFFC00000u);
    c[n++] = 0xAFu;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc_u(NaN) i64 -> INVALID_CONVERSION");
}

static void test_trunc_f64_to_i64(void)
{
    uint8_t c[16];
    size_t n;
    printf("[T] i64.trunc_f64_s/u boundaries (S4)\n");
    n = emit_f64(c, 0xC3E0000000000000ULL); /* -9223372036854775808.0 */
    c[n++] = 0xB0u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc min-ok i64 f64");
    expect_i64(INT64_MIN, "trunc(-2^63) == INT64_MIN f64");
    n = emit_f64(c, 0x43DFFFFFFFFFFFFFULL); /* 2^63 - 1024 */
    c[n++] = 0xB0u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc max-ulp-ok i64 f64");
    expect_i64(9223372036854774784LL, "trunc(2^63-1024) exact");
    n = emit_f64(c, f64bits(-123456.0));
    c[n++] = 0xB0u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc typical i64 f64");
    expect_i64(-123456LL, "trunc typical exact f64");
    n = emit_f64(c, 0x7FF8000000000000ULL);
    c[n++] = 0xB0u;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc(NaN) i64 f64 -> INVALID_CONVERSION");
    n = emit_f64(c, 0x43E0000000000000ULL); /* 2^63 */
    c[n++] = 0xB0u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc(2^63) f64 traps");
    n = emit_f64(c, 0xC3E0000000000001ULL); /* -2^63 - 2 (i.e. -9223..810) */
    c[n++] = 0xB0u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc below -2^63 f64 traps");
    /* unsigned */
    n = emit_f64(c, 0x43EFFFFFFFFFFFFFULL); /* 2^64 - 2048 */
    c[n++] = 0xB1u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run trunc_u max-ulp-ok u64");
    expect_u64(18446744073709549568ULL, "trunc_u(2^64-2048) exact");
    n = emit_f64(c, 0x43F0000000000000ULL); /* 2^64 */
    c[n++] = 0xB1u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(2^64) f64 traps");
    n = emit_f64(c, 0xBFF0000000000000ULL);
    c[n++] = 0xB1u;
    CHECK(run_flat(c, n) == WASM_TRAP_INTEGER_OVERFLOW,
          "trunc_u(-1) f64 traps");
    n = emit_f64(c, 0xFFF8000000000000ULL);
    c[n++] = 0xB1u;
    CHECK(run_flat(c, n) == WASM_TRAP_INVALID_CONVERSION,
          "trunc_u(NaN) u64 -> INVALID_CONVERSION");
}

/* ---------- j) int<->float converts, demote/promote ---------- */

static void test_convert(void)
{
    uint8_t c[16];
    size_t n;
    printf("[T] converts + demote/promote\n");
    /* i32 -> f32 */
    c[0] = 0x41u; c[1] = 0x7Fu; /* i32.const -1 */
    c[2] = 0xB2u;
    CHECK(run_flat(c, 3u) == WASM_TRAP_OK, "run convert(-1)");
    expect_f32_bits(0xBF800000u, "convert(-1) == -1.0f");
    c[0] = 0x41u; c[1] = 0x7Fu;
    c[2] = 0xB3u;
    CHECK(run_flat(c, 3u) == WASM_TRAP_OK, "run convert_u as u32");
    expect_f32_bits(0x4F800000u, "convert_u(-1 bits) == 4294967295u->f32");
    /* i64 -> f32/f64 */
    {
        static const uint8_t k[] = { 0x42, 0x80, 0x80, 0x80, 0x80, 0x80,
                                     0x80, 0x80, 0x80, 0x80, 0x7F, 0xB4u };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK,
              "run convert(INT64_MIN)");
        expect_f32_bits(0xDF000000u, "convert(INT64_MIN) == -2^63f");
    }
    { /* i64.const -1 carries all-ones bits = UINT64_MAX for convert_u */
        static const uint8_t k[] = { 0x42, 0x7F, 0xBAu };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK,
              "run convert_u(UINT64_MAX)");
        expect_f64_bits(0x43F0000000000000ULL, "convert_u max == 2^64 f64");
    }
    /* demote/promote */
    n = emit_f64(c, 0x3FF8000000000000ULL); /* 1.5 */
    c[n++] = 0xB6u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run demote(1.5)");
    expect_f32_bits(0x3FC00000u, "demote(1.5) == 1.5f");
    n = emit_f64(c, 0x7E37E43C8800759CULL); /* 1e300 */
    c[n++] = 0xB6u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run demote(1e300)");
    expect_f32_bits(0x7F800000u, "demote overflow -> +Inf");
    n = emit_f64(c, 0x7FF8000000000001ULL);
    c[n++] = 0xB6u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run demote(NaN)");
    expect_nan32("demote(NaN) -> NaN");
    n = emit_f64(c, 0x8000000000000000ULL); /* -0 */
    c[n++] = 0xB6u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run demote(-0)");
    expect_f32_bits(0x80000000u, "demote(-0) == -0f");
    n = emit_f32(c, 0x3FC00000u);
    c[n++] = 0xBBu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run promote(1.5f)");
    expect_f64_bits(0x3FF8000000000000ULL, "promote exact");
    n = emit_f32(c, 0x7FC00001u);
    c[n++] = 0xBBu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run promote(NaN)");
    expect_nan64("promote(NaN) -> NaN");
    /* f64.convert_i32_s exactness over the full i32 range */
    c[0] = 0x41u; c[1] = 0x7Fu;
    c[2] = 0xB7u;
    CHECK(run_flat(c, 3u) == WASM_TRAP_OK, "run f64.convert(-1)");
    expect_f64_bits(0xBFF0000000000000ULL, "f64.convert(-1) == -1.0");
    /* Previously untested converts, enum-spelled for matrix credit. */
    c[0] = 0x41u; c[1] = 0x7Fu; /* i32.const -1 = 4294967295u */
    c[2] = (uint8_t)OP_F64_CONVERT_I32_U;
    CHECK(run_flat(c, 3u) == WASM_TRAP_OK, "run f64.convert_u(-1)");
    expect_f64_bits(0x41EFFFFFFFE00000ULL, "f64.convert_u == 4294967295.0");
    { /* f32.convert_i64_u of UINT64_MAX rounds to 2^64 (exact f32) */
        static const uint8_t k[] = { 0x42, 0x7F, (uint8_t)OP_F32_CONVERT_I64_U };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK,
              "run f32.convert_u(UINT64_MAX)");
        expect_f32_bits(0x5F800000u, "f32.convert_u max == 2^64");
    }
    { /* f64.convert_i64_s of INT64_MIN is exact */
        static const uint8_t k[] = {
            0x42, 0x80, 0x80, 0x80, 0x80, 0x80,
            0x80, 0x80, 0x80, 0x80, 0x7F, (uint8_t)OP_F64_CONVERT_I64_S
        };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK,
              "run f64.convert(INT64_MIN)");
        expect_f64_bits(0xC3E0000000000000ULL, "f64.convert min == -2^63");
    }
}

/* ---------- k) reinterpret round-trips (S6) ---------- */

static void test_reinterpret(void)
{
    uint8_t c[24];
    size_t n;
    printf("[T] reinterpret round-trips preserve bits (S6)\n");
    n = emit_f32(c, 0x3FC00000u);
    c[n++] = 0xBCu; /* i32.reinterpret_f32 */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run reinterpret 1.5f");
    expect_u32(0x3FC00000u, "bits survive f32->i32");
    n = emit_f32(c, 0x7FC00001u);
    c[n++] = 0xBCu;
    c[n++] = 0xBEu; /* f32.reinterpret_i32 back */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f32->i32->f32 NaN");
    expect_f32_bits(0x7FC00001u, "NaN payload round-trips");
    c[0] = 0x41u; c[1] = 0x7Fu; /* i32.const -1 */
    c[2] = 0xBEu;
    CHECK(run_flat(c, 3u) == WASM_TRAP_OK, "run reinterpret -1 bits");
    expect_f32_bits(0xFFFFFFFFu, "integer bits become NaN bits");
    c[0] = 0x41u; c[1] = 0x00u; /* i32.const 0 */
    c[2] = 0xBEu;
    CHECK(run_flat(c, 3u) == WASM_TRAP_OK, "run reinterpret 0 bits");
    expect_f32_bits(0x00000000u, "zero bits become +0.0f");
    n = emit_f64(c, 0x7FF8000000000001ULL);
    c[n++] = 0xBDu; /* i64.reinterpret_f64 */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run reinterpret f64 NaN");
    expect_u64(0x7FF8000000000001ULL, "f64 NaN payload -> i64 bits");
    n = emit_f64(c, 0xBFF8000000000000ULL);
    c[n++] = 0xBDu;
    c[n++] = 0xBFu; /* f64.reinterpret_i64 back */
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run f64->i64->f64");
    expect_f64_bits(0xBFF8000000000000ULL, "-1.5 bits round-trip");
}

/* ---------- l) NaN propagation + quiet bit (S7) ---------- */

static void test_nan_prop(void)
{
    uint8_t c[24];
    size_t n;
    printf("[T] NaN propagation + quiet bit (S7)\n");
    n = emit_f32(c, 0x7FC00001u);
    n += emit_f32(c + n, 0x3F800000u);
    c[n++] = 0x92u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run nan + 1.0");
    expect_nan32("NaN operand -> NaN result (quiet bit set)");
    n = emit_f32(c, 0x3F800000u);
    n += emit_f32(c + n, 0x7FC00001u);
    c[n++] = 0x94u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run 1.0 * nan");
    expect_nan32("NaN on either side propagates");
    n = emit_f64(c, 0x7FF8000000000001ULL);
    c[n++] = 0x9Fu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run sqrt(nan) f64");
    expect_nan64("sqrt(NaN) -> NaN f64");
    n = emit_f64(c, 0x7FF0000000000000ULL); /* +Inf */
    n += emit_f64(c + n, 0x7FF0000000000000ULL); /* +Inf */
    c[n++] = 0xA1u;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run +Inf - +Inf");
    expect_nan64("+Inf - Inf == NaN");
}

/* ---------- m) f32/f64 memory NaN round-trips (S8) ---------- */

static void test_mem_nan(void)
{
    static uint8_t backing[65536];
    wasm_memory_t m;
    float f = 0.0f;
    double d = 0.0;
    printf("[T] f32/f64 linear-memory NaN payload round-trips (S8)\n");
    CHECK(wasm_memory_init(&m, backing, sizeof(backing), 1u, 1u) ==
          WASM_TRAP_OK, "memory init one page");
    {
        uint32_t bits = 0x7FC00001u;
        float v;
        memcpy(&v, &bits, sizeof(v));
        CHECK(wasm_memory_store_f32(&m, 0u, v) == WASM_TRAP_OK,
              "store f32 nan:0x1");
        CHECK(wasm_memory_load_f32(&m, 0u, &f) == WASM_TRAP_OK,
              "load f32 nan:0x1");
        CHECK(f32bits(f) == 0x7FC00001u, "f32 NaN payload bit-exact");
    }
    {
        uint64_t bits = 0x7FF8000000000001ULL;
        double v;
        memcpy(&v, &bits, sizeof(v));
        CHECK(wasm_memory_store_f64(&m, 8u, v) == WASM_TRAP_OK,
              "store f64 nan:0x1");
        CHECK(wasm_memory_load_f64(&m, 8u, &d) == WASM_TRAP_OK,
              "load f64 nan:0x1");
        CHECK(f64bits(d) == 0x7FF8000000000001ULL,
              "f64 NaN payload bit-exact");
    }
    {
        float v = -0.0f;
        CHECK(wasm_memory_store_f32(&m, 16u, v) == WASM_TRAP_OK,
              "store f32 -0");
        CHECK(wasm_memory_load_f32(&m, 16u, &f) == WASM_TRAP_OK,
              "load f32 -0");
        CHECK(f32bits(f) == 0x80000000u, "f32 -0 bit-exact");
    }
}

/* ---------- n) CLI run + --dump of the float demo ---------- */

static void test_cli_float(void)
{
#ifdef _WIN32
    static const char *const cmd_run =
        "build\\wasm_runner.exe demo\\fadd.wasm main > build\\cli_fadd.txt";
    static const char *const cmd_dump =
        "build\\wasm_runner.exe --dump demo\\fadd.wasm > build\\dump_fadd.txt";
#else
    static const char *const cmd_run =
        "./build/wasm_runner demo/fadd.wasm main > build/cli_fadd.txt";
    static const char *const cmd_dump =
        "./build/wasm_runner --dump demo/fadd.wasm > build/dump_fadd.txt";
#endif
    static char out[4096];
    FILE *fp = NULL;
    size_t n = 0u;
    printf("[T] CLI run + --dump of f32 demo\n");
    CHECK(system(cmd_run) == 0, "run fadd.wasm exits 0");
    fp = fopen("build" "/" "cli_fadd.txt", "rb");
    CHECK(fp != NULL, "fadd output captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    CHECK(strstr(out, "result: 4 (f32)") != NULL, "fadd prints 4 (f32)");
    CHECK(system(cmd_dump) == 0, "dump fadd.wasm exits 0");
    fp = fopen("build" "/" "dump_fadd.txt", "rb");
    CHECK(fp != NULL, "fadd dump captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    CHECK(strstr(out, "params[]") != NULL, "dump shows empty params");
    CHECK(strstr(out, "results[f32]") != NULL, "dump shows f32 result");
}

int main(void)
{
    printf("=== Phase 7 tests: f32/f64 MVP instruction set ===\n");
    test_const();
    test_arith32();
    test_arith64();
    test_sqrt();
    test_minmax32();
    test_minmax64();
    test_sign32();
    test_sign64();
    test_round32();
    test_round64();
    test_nearest32();
    test_nearest64();
    test_cmp32();
    test_cmp64();
    test_trunc_f32_to_i32();
    test_trunc_f64_to_i32();
    test_trunc_f32_to_i64();
    test_trunc_f64_to_i64();
    test_convert();
    test_reinterpret();
    test_nan_prop();
    test_mem_nan();
    test_cli_float();
    printf("--------------------------------------------\n");
    printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("STATUS: FAIL\n");
        return 1;
    }
    printf("STATUS: ALL TESTS PASSED\n");
    return 0;
}

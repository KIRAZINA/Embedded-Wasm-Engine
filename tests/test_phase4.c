/*
 * test_phase4.c — Phase 4 verification: instruction execution core.
 * Standalone, C11, stdio.h/stdint.h only. Hand-assembled bytecode vectors.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -Iinclude src/leb128.c
 *        src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c src/wasm_frame.c
 *        src/wasm_vm.c tests/test_phase4.c -o build/test_phase4
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasm_vm.h"
#include "harness.h"


/* Static VM + 1-page arena (never malloc'd). */
static wasm_vm_t g_vm;
static uint8_t g_arena[65536];
static wasm_memory_t g_mem;
/* Two-page arena for grow-then-access tests (never malloc'd). */
static uint8_t g_arena2[131072];
static wasm_memory_t g_mem2;

/* ---- test-side bytecode assemblers (SLEB/ULEB encoders) ---- */

static size_t enc_u32(uint8_t *out, uint32_t v)
{
    size_t n = 0u;
    do {
        uint8_t b = (uint8_t)(v & 0x7Fu);
        v >>= 7u;
        if (v != 0u) {
            b |= 0x80u;
        }
        out[n++] = b;
    } while (v != 0u);
    return n;
}

static size_t enc_s32(uint8_t *out, int32_t v)
{
    size_t n = 0u;
    int more = 1;
    while (more) {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7; /* gcc: arithmetic shift; test-side assembler only */
        if ((v == 0 && (b & 0x40u) == 0u) || (v == -1 && (b & 0x40u) != 0u)) {
            more = 0;
        } else {
            b |= 0x80u;
        }
        out[n++] = b;
    }
    return n;
}

/* Raw 4-byte LE float payload (f32.const immediate / expected bits). */
static size_t enc_f32(uint8_t *out, uint32_t bits)
{
    out[0] = (uint8_t)(bits & 0xFFu);
    out[1] = (uint8_t)((bits >> 8u) & 0xFFu);
    out[2] = (uint8_t)((bits >> 16u) & 0xFFu);
    out[3] = (uint8_t)((bits >> 24u) & 0xFFu);
    return 4u;
}

/* Raw 8-byte LE double payload (f64.const immediate / expected bits). */
static size_t enc_f64(uint8_t *out, uint64_t bits)
{
    unsigned k;
    for (k = 0u; k < 8u; k++) {
        out[k] = (uint8_t)((bits >> (8u * k)) & 0xFFu);
    }
    return 8u;
}

/* SLEB128 encode of a 64-bit value (i64.const immediates). */
static size_t enc_s64(uint8_t *out, int64_t v)
{
    size_t n = 0u;
    int more = 1;
    while (more) {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7; /* gcc: arithmetic shift; test-side assembler only */
        if ((v == 0 && (b & 0x40u) == 0u) || (v == -1 && (b & 0x40u) != 0u)) {
            more = 0;
        } else {
            b |= 0x80u;
        }
        out[n++] = b;
    }
    return n;
}

/* Fresh VM + bound memory, enter (0 params, nlocals, nret), attach, run. */
static wasm_trap_t exec_bytes(const uint8_t *code, size_t len,
                              uint32_t nlocals, uint32_t nret)
{
    wasm_trap_t tr;
    tr = wasm_vm_init(&g_vm);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_memory_init(&g_mem, g_arena, (uint32_t)sizeof(g_arena), 1u, 1u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_bind_memory(&g_vm, &g_mem);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_enter_function(&g_vm, 0u, 0u, nlocals, nret);
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
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static uint64_t f64bits(double v)
{
    uint64_t u = 0u;
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

/* Build [const a, const b, op], run, expect one i32 result. */
static void check_binop(int32_t a, int32_t b, uint8_t op, int32_t want, const char *name)
{
    uint8_t code[16];
    size_t n = 0u;
    code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, a);
    code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, b);
    code[n++] = op;
    CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, name);
    expect_i32(want, name);
}

/* Build [const a, const b, op], run, expect a trap. */
static void check_binop_trap(int32_t a, int32_t b, uint8_t op, wasm_trap_t want, const char *name)
{
    uint8_t code[16];
    size_t n = 0u;
    code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, a);
    code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, b);
    code[n++] = op;
    CHECK(exec_bytes(code, n, 0u, 0u) == want, name);
}

/* Build [i64.const a, i64.const b, op], run, expect one i64 result. */
static void check_i64_binop(int64_t a, int64_t b, uint8_t op, int64_t want, const char *name)
{
    uint8_t code[32];
    size_t n = 0u;
    code[n++] = 0x42u; n += enc_s64(code + n, a);
    code[n++] = 0x42u; n += enc_s64(code + n, b);
    code[n++] = op;
    CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, name);
    expect_i64(want, name);
}

/* Build [i64.const a, op], run, expect one i64 result. */
static void check_i64_unop(int64_t a, uint8_t op, int64_t want, const char *name)
{
    uint8_t code[24];
    size_t n = 0u;
    code[n++] = 0x42u; n += enc_s64(code + n, a);
    code[n++] = op;
    CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, name);
    expect_i64(want, name);
}

/* Build [i64.const a, i64.const b, op], run, expect one i32 result. */
static void check_i64_cmp(int64_t a, int64_t b, uint8_t op, int32_t want, const char *name)
{
    uint8_t code[32];
    size_t n = 0u;
    code[n++] = 0x42u; n += enc_s64(code + n, a);
    code[n++] = 0x42u; n += enc_s64(code + n, b);
    code[n++] = op;
    CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, name);
    expect_i32(want, name);
}

/* Build [i32.const a, op], run, expect one i32 result. */
static void check_unop(int32_t a, uint8_t op, int32_t want, const char *name)
{
    uint8_t code[16];
    size_t n = 0u;
    code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, a);
    code[n++] = op;
    CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, name);
    expect_i32(want, name);
}

/* ---------- a) arithmetic: (10 + 20) * 3 = 90 ---------- */

static void test_arith_basic(void)
{
    printf("[T] arithmetic: (10+20)*3 and wrap/negative/multi-byte immediates\n");
    {
        static const uint8_t code[] = {
            0x41, 0x0A,       /* i32.const 10 */
            0x41, 0x14,       /* i32.const 20 */
            0x6A,             /* i32.add      */
            0x41, 0x03,       /* i32.const 3  */
            0x6C              /* i32.mul      */
        };
        CHECK(exec_bytes(code, sizeof(code), 0u, 0u) == WASM_TRAP_OK, "run (10+20)*3");
        expect_i32(90, "(10+20)*3 == 90");
    }
    check_binop(100, 58, (uint8_t)OP_I32_SUB, 42, "100-58 == 42");
    check_binop(-5, 8, (uint8_t)OP_I32_ADD, 3, "-5+8 == 3 (negative SLEB)");
    check_binop(10, 300, (uint8_t)OP_I32_ADD, 310, "10+300 == 310 (multi-byte SLEB)");
    check_binop(65536, 65536, (uint8_t)OP_I32_MUL, 0, "65536*65536 wraps to 0 (no UB trap)");
    check_binop(7, 2, (uint8_t)OP_I32_DIV_S, 3, "7/2 == 3 truncation");
    check_binop(-7, 2, (uint8_t)OP_I32_DIV_S, -3, "-7/2 == -3 truncation toward zero");
    check_binop(7, -2, (uint8_t)OP_I32_DIV_S, -3, "7/-2 == -3");
    check_binop(7, 2, (uint8_t)OP_I32_REM_S, 1, "7%2 == 1");
    check_binop(-7, 2, (uint8_t)OP_I32_REM_S, -1, "-7%2 == -1 (sign of dividend)");
    check_binop(-1, 2, (uint8_t)OP_I32_DIV_U, 2147483647, "0xFFFFFFFF/2 unsigned == 2147483647");
    check_binop(-1, 2, (uint8_t)OP_I32_REM_U, 1, "0xFFFFFFFF%2 unsigned == 1");
}

/* ---------- b) division by zero on all four ops ---------- */

static void test_div_by_zero(void)
{
    printf("[T] division-by-zero traps (DIV_S/DIV_U/REM_S/REM_U)\n");
    check_binop_trap(10, 0, (uint8_t)OP_I32_DIV_S, WASM_TRAP_DIV_BY_ZERO, "10/0 DIV_S -> DIV_BY_ZERO");
    check_binop_trap(10, 0, (uint8_t)OP_I32_DIV_U, WASM_TRAP_DIV_BY_ZERO, "10/0 DIV_U -> DIV_BY_ZERO");
    check_binop_trap(10, 0, (uint8_t)OP_I32_REM_S, WASM_TRAP_DIV_BY_ZERO, "10%0 REM_S -> DIV_BY_ZERO");
    check_binop_trap(10, 0, (uint8_t)OP_I32_REM_U, WASM_TRAP_DIV_BY_ZERO, "10%0 REM_U -> DIV_BY_ZERO");
    check_binop_trap(0, 0, (uint8_t)OP_I32_DIV_S, WASM_TRAP_DIV_BY_ZERO, "0/0 DIV_S -> DIV_BY_ZERO");
    check_binop_trap(-5, 0, (uint8_t)OP_I32_REM_U, WASM_TRAP_DIV_BY_ZERO, "-5%0 REM_U -> DIV_BY_ZERO");
}

/* ---------- c) INT32_MIN / -1 ---------- */

static void test_min_div_neg1(void)
{
    printf("[T] INT32_MIN / -1 overflow traps + unsigned control\n");
    check_binop_trap(INT32_MIN, -1, (uint8_t)OP_I32_DIV_S,
                     WASM_TRAP_INTEGER_OVERFLOW, "MIN/-1 DIV_S -> INTEGER_OVERFLOW");
    check_binop(INT32_MIN, -1, (uint8_t)OP_I32_REM_S, 0,
                "MIN%-1 REM_S == 0 (MVP, no trap)");
    check_binop(INT32_MIN, -1, (uint8_t)OP_I32_DIV_U, 0, "MIN/-1 DIV_U == 0 (unsigned, no trap)");
    check_binop(INT32_MIN, 2, (uint8_t)OP_I32_REM_S, 0, "MIN%2 == 0");
    check_binop(INT32_MAX, 1, (uint8_t)OP_I32_DIV_S, INT32_MAX, "MAX/1 == MAX");
}

/* ---------- d) bitwise + shifts with %32 masking ---------- */

static void test_bits_shifts(void)
{
    printf("[T] bitwise logic and shift-modulo-32 (no C UB)\n");
    check_binop(0xF0, 0x3C, (uint8_t)OP_I32_AND, 0x30, "0xF0 & 0x3C == 0x30");
    check_binop(0xF0, 0x3C, (uint8_t)OP_I32_OR, 0xFC, "0xF0 | 0x3C == 0xFC");
    check_binop(0xF0, 0x3C, (uint8_t)OP_I32_XOR, 0xCC, "0xF0 ^ 0x3C == 0xCC");
    check_binop(1, 31, (uint8_t)OP_I32_SHL, (int32_t)0x80000000u, "1<<31 == 0x80000000");
    check_binop(INT32_MIN, 1, (uint8_t)OP_I32_SHR_U, 1073741824, "MIN>>1 logical == 0x40000000");
    check_binop(INT32_MIN, 1, (uint8_t)OP_I32_SHR_S, (int32_t)0xC0000000u, "MIN>>1 arith == 0xC0000000");
    check_binop(-2, 1, (uint8_t)OP_I32_SHR_S, -1, "-2>>1 arith == -1");
    check_binop(-2, 1, (uint8_t)OP_I32_SHR_U, 2147483647, "-2>>1 logical == 0x7FFFFFFF");
    check_binop(1, 32, (uint8_t)OP_I32_SHL, 1, "shift 32 masked to 0: 1<<32 == 1");
    check_binop(255, 33, (uint8_t)OP_I32_SHL, 510, "shift 33 masked to 1: 255<<33 == 510");
    check_binop(-1, -1, (uint8_t)OP_I32_SHR_U, 1, "shr by -1 (mask 31): 0xFFFFFFFF>>31 == 1");
}

/* ---------- e) comparisons ---------- */

static void test_compare(void)
{
    static const struct { int32_t a; int32_t b; uint8_t op; int32_t exp; const char *name; } cases[] = {
        { 10, 20, (uint8_t)OP_I32_EQ, 0, "10==20 -> 0" },
        { 10, 10, (uint8_t)OP_I32_EQ, 1, "10==10 -> 1" },
        { 10, 20, (uint8_t)OP_I32_NE, 1, "10!=20 -> 1" },
        { 10, 10, (uint8_t)OP_I32_NE, 0, "10!=10 -> 0" },
        { 10, 20, (uint8_t)OP_I32_LT_S, 1, "10<20 signed -> 1" },
        { 20, 10, (uint8_t)OP_I32_LT_S, 0, "20<10 signed -> 0" },
        { -1,  1, (uint8_t)OP_I32_LT_S, 1, "-1<1 signed -> 1" },
        { -1,  1, (uint8_t)OP_I32_LT_U, 0, "-1<1 unsigned -> 0 (0xFFFFFFFF>1)" },
        {  1, -1, (uint8_t)OP_I32_LT_U, 1, "1<-1 unsigned -> 1" },
        { 20, 10, (uint8_t)OP_I32_GT_S, 1, "20>10 signed -> 1" },
        { -1,  1, (uint8_t)OP_I32_GT_U, 1, "-1>1 unsigned -> 1" },
        {  1, -1, (uint8_t)OP_I32_GT_U, 0, "1>-1 unsigned -> 0" },
        {  5,  5, (uint8_t)OP_I32_LE_S, 1, "5<=5 -> 1" },
        {  6,  5, (uint8_t)OP_I32_LE_S, 0, "6<=5 -> 0" },
        { -1,  1, (uint8_t)OP_I32_LE_U, 0, "-1<=1 unsigned -> 0" },
        {  5,  5, (uint8_t)OP_I32_GE_S, 1, "5>=5 -> 1" },
        {  4,  5, (uint8_t)OP_I32_GE_S, 0, "4>=5 -> 0" },
        { -1,  1, (uint8_t)OP_I32_GE_U, 1, "-1>=1 unsigned -> 1" },
    };
    size_t i;
    printf("[T] i32 comparisons incl. signed/unsigned split\n");
    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check_binop(cases[i].a, cases[i].b, cases[i].op, cases[i].exp, cases[i].name);
    }
    { /* EQZ takes a single operand. */
        uint8_t c0[] = { 0x41, 0x00, 0x45 };
        uint8_t c5[] = { 0x41, 0x05, 0x45 };
        CHECK(exec_bytes(c0, sizeof(c0), 0u, 0u) == WASM_TRAP_OK, "run eqz(0)");
        expect_i32(1, "eqz(0) -> 1");
        CHECK(exec_bytes(c5, sizeof(c5), 0u, 0u) == WASM_TRAP_OK, "run eqz(5)");
        expect_i32(0, "eqz(5) -> 0");
    }
}

/* ---------- f) memory load/store round-trips + offsets ---------- */

static void test_mem_ops(void)
{
    printf("[T] I32 LOAD/STORE round-trip, offsets, OOB, wraparound\n");
    { /* store 0x01020304 @100, load it back; check LE bytes in arena. */
        uint8_t code[32];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 100);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0x01020304);
        code[n++] = (uint8_t)OP_I32_STORE; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 100);
        code[n++] = (uint8_t)OP_I32_LOAD; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run store/load @100");
        expect_i32(0x01020304, "load returns stored 0x01020304");
        CHECK(g_arena[100] == 0x04u && g_arena[101] == 0x03u &&
              g_arena[102] == 0x02u && g_arena[103] == 0x01u, "LE bytes 04 03 02 01 in arena");
    }
    { /* nonzero memarg offset: base=100 + offset=16 -> EA 116. */
        uint8_t code[32];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 100);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 7777);
        code[n++] = (uint8_t)OP_I32_STORE; code[n++] = 0x00; n += enc_u32(code + n, 16u);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 116);
        code[n++] = (uint8_t)OP_I32_LOAD; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run store @100+16, load @116");
        expect_i32(7777, "offset addressing round-trips 7777");
    }
    { /* boundary OOB: only 2 bytes left for a 4-byte load. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 65534);
        code[n++] = (uint8_t)OP_I32_LOAD; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "load @cap-2 -> OOB");
    }
    { /* EA wraparound: 0xFFFFFFFF + 1 overflows u32. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, -1);
        code[n++] = (uint8_t)OP_I32_LOAD; code[n++] = 0x00; n += enc_u32(code + n, 1u);
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "EA wrap MAX+1 -> OOB");
    }
    { /* unbound memory traps instead of crashing. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0);
        code[n++] = (uint8_t)OP_I32_LOAD; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init (no memory bound)");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "enter");
        CHECK(wasm_vm_attach_code(&g_vm, code, n) == WASM_TRAP_OK, "attach");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "load unbound -> OOB");
    }
    { /* alias API used by the core behaves identically. */
        int32_t v = 0;
        CHECK(wasm_memory_init(&g_mem, g_arena, (uint32_t)sizeof(g_arena), 1u, 1u) == WASM_TRAP_OK, "mem init");
        CHECK(wasm_mem_write_i32(&g_mem, 200u, -123456) == WASM_TRAP_OK, "alias write");
        CHECK(wasm_mem_read_i32(&g_mem, 200u, &v) == WASM_TRAP_OK && v == -123456, "alias read round-trip");
    }
}

/* Build [i64.const a, wrap], run, expect one i32 result. */
static void check_wrap(int64_t a, int32_t want, const char *name)
{
    uint8_t code[24];
    size_t n = 0u;
    code[n++] = 0x42u; n += enc_s64(code + n, a);
    code[n++] = (uint8_t)OP_I32_WRAP_I64;
    CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, name);
    expect_i32(want, name);
}

static void test_bitops_i64(void)
{
    printf("[T] I32/I64 BIT COUNTS, ROTATES, I64 COMPARE, WRAP/EXTEND\n");
    check_unop(0, (uint8_t)OP_I32_CLZ, 32, "clz(0) == 32");
    check_unop(1, (uint8_t)OP_I32_CLZ, 31, "clz(1) == 31");
    check_unop((int32_t)0x80000000u, (uint8_t)OP_I32_CLZ, 0, "clz(0x80000000) == 0");
    check_unop(0x00F00000, (uint8_t)OP_I32_CLZ, 8, "clz(0x00F00000) == 8");
    check_unop(0, (uint8_t)OP_I32_CTZ, 32, "ctz(0) == 32");
    check_unop(1, (uint8_t)OP_I32_CTZ, 0, "ctz(1) == 0");
    check_unop((int32_t)0x80000000u, (uint8_t)OP_I32_CTZ, 31, "ctz(0x80000000) == 31");
    check_unop(0x00001000, (uint8_t)OP_I32_CTZ, 12, "ctz(0x1000) == 12");
    check_unop(0, (uint8_t)OP_I32_POPCNT, 0, "popcnt(0) == 0");
    check_unop(-1, (uint8_t)OP_I32_POPCNT, 32, "popcnt(-1) == 32");
    check_unop(0x0F0F0F0F, (uint8_t)OP_I32_POPCNT, 16, "popcnt(0x0F0F0F0F) == 16");
    check_binop((int32_t)0x80000001u, 1, (uint8_t)OP_I32_ROTL, 3, "rotl(0x80000001,1) == 3");
    check_binop((int32_t)0x80000001u, 1, (uint8_t)OP_I32_ROTR, (int32_t)0xC0000000u, "rotr(0x80000001,1) == 0xC0000000");
    check_binop(0x12345678, 0, (uint8_t)OP_I32_ROTL, 0x12345678, "rotl(x,0) == x");
    check_binop(0x12345678, 32, (uint8_t)OP_I32_ROTR, 0x12345678, "rotr(x,32) == x");
    check_binop(0x12345678, 36, (uint8_t)OP_I32_ROTL, 0x23456781, "rotl(x,36) == rotl(x,4)");
    check_binop(0x12345678, -4, (uint8_t)OP_I32_ROTR, 0x23456781, "rotr(x,-4) == rotl(x,4)");
    check_i64_unop(0, (uint8_t)OP_I64_CLZ, 64, "i64 clz(0) == 64");
    check_i64_unop(1, (uint8_t)OP_I64_CLZ, 63, "i64 clz(1) == 63");
    check_i64_unop((int64_t)0x8000000000000000LL, (uint8_t)OP_I64_CLZ, 0, "i64 clz(top) == 0");
    check_i64_unop(0, (uint8_t)OP_I64_CTZ, 64, "i64 ctz(0) == 64");
    check_i64_unop((int64_t)0x8000000000000000LL, (uint8_t)OP_I64_CTZ, 63, "i64 ctz(top) == 63");
    check_i64_unop(0, (uint8_t)OP_I64_POPCNT, 0, "i64 popcnt(0) == 0");
    check_i64_unop(-1, (uint8_t)OP_I64_POPCNT, 64, "i64 popcnt(-1) == 64");
    check_i64_unop((int64_t)0x0F0F0F0F0F0F0F0FLL, (uint8_t)OP_I64_POPCNT, 32, "i64 popcnt(0x0F..) == 32");
    check_i64_binop(1, 63, (uint8_t)OP_I64_ROTL, (int64_t)0x8000000000000000LL, "i64 rotl(1,63) == top");
    check_i64_binop(1, 1, (uint8_t)OP_I64_ROTR, (int64_t)0x8000000000000000LL, "i64 rotr(1,1) == top");
    check_i64_binop((int64_t)0x123456789ABCDEF0LL, 0, (uint8_t)OP_I64_ROTL, (int64_t)0x123456789ABCDEF0LL, "i64 rotl(x,0) == x");
    check_i64_binop((int64_t)0x123456789ABCDEF0LL, 64, (uint8_t)OP_I64_ROTR, (int64_t)0x123456789ABCDEF0LL, "i64 rotr(x,64) == x");
    check_i64_binop((int64_t)0x123456789ABCDEF0LL, 68, (uint8_t)OP_I64_ROTL, (int64_t)0x23456789ABCDEF01LL, "i64 rotl(x,68) == rotl(x,4)");
    check_i64_binop(7, 2, (uint8_t)OP_I64_REM_S, 1, "i64 7%2 == 1");
    check_i64_binop(-7, 2, (uint8_t)OP_I64_REM_S, -1, "i64 -7%2 == -1 (sign of dividend)");
    check_i64_binop((int64_t)0x8000000000000000LL, -1, (uint8_t)OP_I64_REM_S, 0, "i64 MIN%-1 == 0 (MVP, no trap)");
    check_i64_binop((int64_t)0xFFFFFFFFFFFFFFFFLL, 2, (uint8_t)OP_I64_REM_U, 1, "i64 u64max%2 == 1");
    check_i64_binop(12, 10, (uint8_t)OP_I64_AND, 8, "i64 12&10 == 8");
    check_i64_binop(12, 10, (uint8_t)OP_I64_OR, 14, "i64 12|10 == 14");
    check_i64_binop(12, 10, (uint8_t)OP_I64_XOR, 6, "i64 12^10 == 6");
    check_i64_binop(1, 63, (uint8_t)OP_I64_SHL, (int64_t)0x8000000000000000LL, "i64 1<<63 == MIN");
    check_i64_binop(1, 65, (uint8_t)OP_I64_SHL, 2, "i64 shift 65 masked to 1");
    check_i64_binop(-1, 1, (uint8_t)OP_I64_SHR_S, -1, "i64 -1>>1 arith == -1");
    check_i64_binop((int64_t)0x8000000000000000LL, 1, (uint8_t)OP_I64_SHR_U, (int64_t)0x4000000000000000LL, "i64 MIN>>1 logical");
    check_i64_binop(-1, 65, (uint8_t)OP_I64_SHR_U, INT64_MAX, "i64 shr count masked %64");
    {
        uint8_t code[24];
        size_t n = 0u;
        code[n++] = 0x42u; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I64_EQZ;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run i64.eqz(0)");
        expect_i32(1, "i64.eqz(0) -> 1");
    }
    {
        uint8_t code[24];
        size_t n = 0u;
        code[n++] = 0x42u; n += enc_s64(code + n, 5);
        code[n++] = (uint8_t)OP_I64_EQZ;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run i64.eqz(5)");
        expect_i32(0, "i64.eqz(5) -> 0");
    }
    check_i64_cmp(10, 10, (uint8_t)OP_I64_EQ, 1, "i64 10==10 -> 1");
    check_i64_cmp(10, 20, (uint8_t)OP_I64_EQ, 0, "i64 10==20 -> 0");
    check_i64_cmp(10, 20, (uint8_t)OP_I64_NE, 1, "i64 10!=20 -> 1");
    check_i64_cmp(-1, 1, (uint8_t)OP_I64_LT_S, 1, "i64 -1<1 signed -> 1");
    check_i64_cmp(-1, 1, (uint8_t)OP_I64_LT_U, 0, "i64 -1<1 unsigned -> 0");
    check_i64_cmp(1, -1, (uint8_t)OP_I64_LT_U, 1, "i64 1<-1 unsigned -> 1");
    check_i64_cmp(-1, 1, (uint8_t)OP_I64_GT_U, 1, "i64 -1>1 unsigned -> 1");
    check_i64_cmp(1, -1, (uint8_t)OP_I64_GT_U, 0, "i64 1>-1 unsigned -> 0");
    check_i64_cmp(20, 10, (uint8_t)OP_I64_GT_S, 1, "i64 20>10 signed -> 1");
    check_i64_cmp(-1, 1, (uint8_t)OP_I64_LE_U, 0, "i64 -1<=1 unsigned -> 0");
    check_i64_cmp(5, 5, (uint8_t)OP_I64_LE_S, 1, "i64 5<=5 -> 1");
    check_i64_cmp(5, 5, (uint8_t)OP_I64_GE_S, 1, "i64 5>=5 -> 1");
    check_i64_cmp(-1, 1, (uint8_t)OP_I64_GE_U, 1, "i64 -1>=1 unsigned -> 1");
    check_i64_cmp(4, 5, (uint8_t)OP_I64_GE_S, 0, "i64 4>=5 -> 0");
    check_wrap(-1, -1, "wrap(-1) == -1");
    check_wrap((int64_t)0x100000000LL, 0, "wrap(2^32) == 0");
    check_wrap((int64_t)0x1FFFFFFFFLL, -1, "wrap(2^33-1) == -1");
    {
        uint8_t code[24];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, -1);
        code[n++] = (uint8_t)OP_I64_EXTEND_I32_S;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run extend_s(-1)");
        expect_i64(-1, "extend_s(-1) == -1");
    }
    {
        uint8_t code[24];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, -1);
        code[n++] = (uint8_t)OP_I64_EXTEND_I32_U;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run extend_u(-1)");
        expect_i64((int64_t)4294967295u, "extend_u(-1) == 4294967295");
    }
    {
        uint8_t code[24];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, (int32_t)0x80000000u);
        code[n++] = (uint8_t)OP_I64_EXTEND_I32_S;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run extend_s(INT32_MIN)");
        expect_i64((int64_t)0xFFFFFFFF80000000LL, "extend_s(MIN) == 0xFFFFFFFF80000000");
    }
    {
        uint8_t code[24];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, (int32_t)0x80000000u);
        code[n++] = (uint8_t)OP_I64_EXTEND_I32_U;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run extend_u(INT32_MIN)");
        expect_i64((int64_t)2147483648u, "extend_u(MIN) == 2147483648");
    }
    { /* mistyped operands trap cleanly. */
        uint8_t k1[16];
        size_t n = 0u;
        uint8_t k2[24];
        size_t m = 0u;
        uint8_t k3[24];
        size_t q = 0u;
        k1[n++] = 0x42u; k1[n++] = 0x05;
        k1[n++] = (uint8_t)OP_I32_CLZ;
        CHECK(exec_bytes(k1, n, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "clz i64 -> TYPE_MISMATCH");
        k2[m++] = (uint8_t)OP_I32_CONST; m += enc_s32(k2 + m, 7);
        k2[m++] = (uint8_t)OP_I32_WRAP_I64;
        CHECK(exec_bytes(k2, m, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "wrap i32 -> TYPE_MISMATCH");
        k3[q++] = 0x42u; k3[q++] = 0x05;
        k3[q++] = (uint8_t)OP_I64_EXTEND_I32_S;
        CHECK(exec_bytes(k3, q, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "extend i64 -> TYPE_MISMATCH");
    }
    { /* i64.eq with mixed operand types traps. */
        uint8_t code[24];
        size_t n = 0u;
        code[n++] = 0x42u; code[n++] = 0x01;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 1);
        code[n++] = (uint8_t)OP_I64_EQ;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "i64.eq mixed -> TYPE_MISMATCH");
    }
    CHECK((uint8_t)OP_I32_CLZ == 0x67u && (uint8_t)OP_I32_CTZ == 0x68u &&
          (uint8_t)OP_I32_POPCNT == 0x69u && (uint8_t)OP_I32_ROTL == 0x77u &&
          (uint8_t)OP_I32_ROTR == 0x78u && (uint8_t)OP_I64_CLZ == 0x79u &&
          (uint8_t)OP_I64_CTZ == 0x7Au && (uint8_t)OP_I64_POPCNT == 0x7Bu &&
          (uint8_t)OP_I64_ROTL == 0x89u && (uint8_t)OP_I64_ROTR == 0x8Au &&
          (uint8_t)OP_I64_EQZ == 0x50u && (uint8_t)OP_I64_EQ == 0x51u &&
          (uint8_t)OP_I64_NE == 0x52u && (uint8_t)OP_I64_LT_S == 0x53u &&
          (uint8_t)OP_I64_LT_U == 0x54u && (uint8_t)OP_I64_GT_S == 0x55u &&
          (uint8_t)OP_I64_GT_U == 0x56u && (uint8_t)OP_I64_LE_S == 0x57u &&
          (uint8_t)OP_I64_LE_U == 0x58u && (uint8_t)OP_I64_GE_S == 0x59u &&
          (uint8_t)OP_I64_GE_U == 0x5Au && (uint8_t)OP_I32_WRAP_I64 == 0xA7u &&
          (uint8_t)OP_I64_EXTEND_I32_S == 0xACu &&
          (uint8_t)OP_I64_EXTEND_I32_U == 0xADu, "alu opcode encodings");
}

/* ---------- consts, locals-via-step, misc traps ---------- */

static void test_mem_float(void)
{
    printf("[T] F32/F64 LOAD/STORE round-trip, unaligned, OOB, after grow\n");
    { /* f32 aligned round-trip @100; LE bytes land in the arena. */
        uint8_t code[48];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 100);
        code[n++] = (uint8_t)OP_F32_CONST; n += enc_f32(code + n, 0x40490FDBu);
        code[n++] = (uint8_t)OP_F32_STORE; code[n++] = 0x02; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 100);
        code[n++] = (uint8_t)OP_F32_LOAD; code[n++] = 0x02; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run f32 store/load @100");
        expect_f32_bits(0x40490FDBu, "f32 pi round-trips bit-exact");
        CHECK(g_arena[100] == 0xDBu && g_arena[101] == 0x0Fu &&
              g_arena[102] == 0x49u && g_arena[103] == 0x40u, "LE bytes DB 0F 49 40 in arena");
    }
    { /* f64 aligned round-trip @200. */
        uint8_t code[64];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 200);
        code[n++] = (uint8_t)OP_F64_CONST; n += enc_f64(code + n, 0x400921FB54442D18uLL);
        code[n++] = (uint8_t)OP_F64_STORE; code[n++] = 0x03; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 200);
        code[n++] = (uint8_t)OP_F64_LOAD; code[n++] = 0x03; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run f64 store/load @200");
        expect_f64_bits(0x400921FB54442D18uLL, "f64 pi round-trips bit-exact");
    }
    { /* f32 unaligned @3 with a nonzero align hint (hint is ignored). */
        uint8_t code[48];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 3);
        code[n++] = (uint8_t)OP_F32_CONST; n += enc_f32(code + n, 0x3FC00000u);
        code[n++] = (uint8_t)OP_F32_STORE; code[n++] = 0x02; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 3);
        code[n++] = (uint8_t)OP_F32_LOAD; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run f32 unaligned @3");
        expect_f32_bits(0x3FC00000u, "unaligned f32 1.5 round-trips");
    }
    { /* f64 unaligned @5. */
        uint8_t code[64];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 5);
        code[n++] = (uint8_t)OP_F64_CONST; n += enc_f64(code + n, 0xC000000000000000uLL);
        code[n++] = (uint8_t)OP_F64_STORE; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 5);
        code[n++] = (uint8_t)OP_F64_LOAD; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run f64 unaligned @5");
        expect_f64_bits(0xC000000000000000uLL, "unaligned f64 -2.0 round-trips");
    }
    { /* boundary OOB: 2 bytes left for a 4-byte f32 load. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 65534);
        code[n++] = (uint8_t)OP_F32_LOAD; code[n++] = 0x02; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "f32 load @cap-2 -> OOB");
    }
    { /* boundary OOB: f64 store @cap-5 overruns by 3 bytes. */
        uint8_t code[32];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 65531);
        code[n++] = (uint8_t)OP_F64_CONST; n += enc_f64(code + n, 0uLL);
        code[n++] = (uint8_t)OP_F64_STORE; code[n++] = 0x03; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "f64 store @cap-5 -> OOB");
    }
    { /* grown memory becomes addressable to float ops. */
        uint8_t code[48];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 70000);
        code[n++] = (uint8_t)OP_F32_CONST; n += enc_f32(code + n, 0x41200000u);
        code[n++] = (uint8_t)OP_F32_STORE; code[n++] = 0x02; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 70000);
        code[n++] = (uint8_t)OP_F32_LOAD; code[n++] = 0x02; code[n++] = 0x00;
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init grow arena vm");
        CHECK(wasm_memory_init(&g_mem2, g_arena2, (uint32_t)sizeof(g_arena2), 1u, 2u) == WASM_TRAP_OK, "1-page mem, max 2");
        CHECK(wasm_vm_bind_memory(&g_vm, &g_mem2) == WASM_TRAP_OK, "bind grow arena");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "enter");
        CHECK(wasm_vm_attach_code(&g_vm, code, n) == WASM_TRAP_OK, "attach");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "store @70000 before grow -> OOB");
        CHECK(wasm_mem_grow(&g_mem2, 1u, NULL) == WASM_TRAP_OK, "grow to 2 pages");
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "re-init after grow");
        CHECK(wasm_vm_bind_memory(&g_vm, &g_mem2) == WASM_TRAP_OK, "re-bind grown mem");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "re-enter");
        CHECK(wasm_vm_attach_code(&g_vm, code, n) == WASM_TRAP_OK, "re-attach");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "store/load @70000 after grow");
        expect_f32_bits(0x41200000u, "grown-page f32 10.0 round-trips");
    }
    { /* NaN payloads survive a store/load round-trip bit-exact. */
        uint8_t code[48];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_F32_CONST; n += enc_f32(code + n, 0x7FC00001u);
        code[n++] = (uint8_t)OP_F32_STORE; code[n++] = 0x02; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_F32_LOAD; code[n++] = 0x02; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run NaN round-trip");
        expect_f32_bits(0x7FC00001u, "NaN payload bit-exact through memory");
    }
    { /* store with an i32 (not f32) value traps on the type. */
        uint8_t code[32];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 8);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 1);
        code[n++] = (uint8_t)OP_F32_STORE; code[n++] = 0x02; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "f32.store i32 value -> TYPE_MISMATCH");
    }
    { /* f64 load with an i64 (not i32) base traps on the type. */
        uint8_t code[32];
        size_t n = 0u;
        code[n++] = 0x42u; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_F64_LOAD; code[n++] = 0x03; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "f64.load i64 base -> TYPE_MISMATCH");
    }
    { /* unbound memory traps instead of crashing. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0);
        code[n++] = (uint8_t)OP_F64_LOAD; code[n++] = 0x03; code[n++] = 0x00;
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init (no memory bound)");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "enter");
        CHECK(wasm_vm_attach_code(&g_vm, code, n) == WASM_TRAP_OK, "attach");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "f64 load unbound -> OOB");
    }
    CHECK((uint8_t)OP_F32_LOAD == 0x2Au && (uint8_t)OP_F64_LOAD == 0x2Bu &&
          (uint8_t)OP_F32_STORE == 0x38u && (uint8_t)OP_F64_STORE == 0x39u,
          "float mem opcode encodings");
}

static void test_mem_extended(void)
{
    printf("[T] I64/SUB-WORD LOADS/STORES, MEMORY.SIZE/GROW\n");
    { /* i64 aligned round-trip @300 with high bits set. */
        uint8_t code[64];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 300);
        code[n++] = 0x42u; n += enc_s64(code + n, (int64_t)0x0102030405060708LL);
        code[n++] = (uint8_t)OP_I64_STORE; code[n++] = 0x03; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 300);
        code[n++] = (uint8_t)OP_I64_LOAD; code[n++] = 0x03; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run i64 store/load @300");
        expect_i64((int64_t)0x0102030405060708LL, "i64 bytes round-trip exact");
        CHECK(g_arena[300] == 0x08u && g_arena[303] == 0x05u &&
              g_arena[306] == 0x02u && g_arena[307] == 0x01u, "LE bytes 08..01 in arena");
    }
    { /* i32 sub-word sign/zero extension from 0xFFFFFFFF. */
        uint8_t code[64];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 50);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, -1);
        code[n++] = (uint8_t)OP_I32_STORE; code[n++] = 0x02; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 50);
        code[n++] = (uint8_t)OP_I32_LOAD8_S; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 50);
        code[n++] = (uint8_t)OP_I32_LOAD8_U; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 50);
        code[n++] = (uint8_t)OP_I32_LOAD16_S; code[n++] = 0x01; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 50);
        code[n++] = (uint8_t)OP_I32_LOAD16_U; code[n++] = 0x01; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run sub-word loads");
        expect_i32(65535, "load16_u(0xFFFF) == 65535");
        expect_i32(-1, "load16_s(0xFFFF) == -1");
        expect_i32(255, "load8_u(0xFF) == 255");
        expect_i32(-1, "load8_s(0xFF) == -1");
    }
    { /* i32.load8_s of 0x7F stays positive (sign bit clear). */
        uint8_t code[48];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 52);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0x7F);
        code[n++] = (uint8_t)OP_I32_STORE8; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 52);
        code[n++] = (uint8_t)OP_I32_LOAD8_S; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run store8/load8_s 0x7F");
        expect_i32(127, "load8_s(0x7F) == 127");
    }
    { /* i64 sub-word extension from all-set bytes. */
        uint8_t code[96];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = 0x42u; code[n++] = 0x7F;
        code[n++] = (uint8_t)OP_I64_STORE; code[n++] = 0x03; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_I64_LOAD8_S; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_I64_LOAD8_U; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_I64_LOAD16_S; code[n++] = 0x01; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_I64_LOAD16_U; code[n++] = 0x01; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_I64_LOAD32_S; code[n++] = 0x02; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 400);
        code[n++] = (uint8_t)OP_I64_LOAD32_U; code[n++] = 0x02; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run i64 sub-word loads");
        expect_i64((int64_t)4294967295u, "load32_u(-1) == 4294967295");
        expect_i64(-1, "load32_s(-1) == -1");
        expect_i64(65535, "load16_u(-1) == 65535");
        expect_i64(-1, "load16_s(-1) == -1");
        expect_i64(255, "load8_u(-1) == 255");
        expect_i64(-1, "load8_s(-1) == -1");
    }
    { /* unaligned sub-word load16 @1. */
        uint8_t code[48];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 1);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0xABCD);
        code[n++] = (uint8_t)OP_I32_STORE16; code[n++] = 0x01; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 1);
        code[n++] = (uint8_t)OP_I32_LOAD16_U; code[n++] = 0x01; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run store16/load16 @1");
        expect_i32(0xABCD, "unaligned load16_u == 0xABCD");
        CHECK(g_arena[1] == 0xCDu && g_arena[2] == 0xABu, "LE bytes CD AB in arena");
    }
    { /* store truncation: only the low byte(s) land. */
        uint8_t code[64];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 70);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0x1234);
        code[n++] = (uint8_t)OP_I32_STORE8; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 72);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0x123456);
        code[n++] = (uint8_t)OP_I32_STORE16; code[n++] = 0x01; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 74);
        code[n++] = 0x42u; n += enc_s64(code + n, (int64_t)0x1FFLL);
        code[n++] = (uint8_t)OP_I64_STORE8; code[n++] = 0x00; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 76);
        code[n++] = 0x42u; n += enc_s64(code + n, (int64_t)0x12345LL);
        code[n++] = (uint8_t)OP_I64_STORE16; code[n++] = 0x01; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run truncating stores");
        CHECK(g_arena[70] == 0x34u, "store8 keeps low byte 0x34");
        CHECK(g_arena[72] == 0x56u && g_arena[73] == 0x34u, "store16 keeps 56 34");
        CHECK(g_arena[74] == 0xFFu, "i64 store8 keeps 0xFF");
        CHECK(g_arena[76] == 0x45u && g_arena[77] == 0x23u, "i64 store16 keeps 45 23");
    }
    { /* i64.store32 keeps the low 32 bits; load back signed. */
        uint8_t code[48];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 410);
        code[n++] = 0x42u; code[n++] = 0x7F;
        code[n++] = (uint8_t)OP_I64_STORE32; code[n++] = 0x02; code[n++] = 0x00;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 410);
        code[n++] = (uint8_t)OP_I64_LOAD32_S; code[n++] = 0x02; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run store32/load32_s");
        expect_i64(-1, "store32(-1)/load32_s == -1");
        CHECK(g_arena[410] == 0xFFu && g_arena[413] == 0xFFu, "store32 wrote 4 bytes");
    }
    { /* sub-word boundary OOB: 1 byte left for a 2-byte load. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 65535);
        code[n++] = (uint8_t)OP_I32_LOAD16_S; code[n++] = 0x01; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "load16 @cap-1 -> OOB");
    }
    { /* store8 exactly at the end of memory is out of bounds. */
        uint8_t code[32];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 65536);
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 9);
        code[n++] = (uint8_t)OP_I32_STORE8; code[n++] = 0x00; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "store8 @cap -> OOB");
    }
    { /* memory.size reports the bound page count. */
        static const uint8_t k[] = { 0x3F, 0x00 };
        CHECK(exec_bytes(k, sizeof(k), 0u, 0u) == WASM_TRAP_OK, "run memory.size");
        expect_i32(1, "size == 1 page");
    }
    { /* memory.grow failure (max 1) returns -1, never traps. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 1);
        code[n++] = (uint8_t)OP_MEMORY_GROW; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run failing grow");
        expect_i32(-1, "grow past max -> -1");
    }
    { /* memory.grow with a huge delta returns -1, never traps. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 2147483647);
        code[n++] = (uint8_t)OP_MEMORY_GROW; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run huge grow");
        expect_i32(-1, "grow 2^31-1 pages -> -1");
    }
    { /* memory.grow with a negative delta returns -1, never traps. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, -1);
        code[n++] = (uint8_t)OP_MEMORY_GROW; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run negative grow");
        expect_i32(-1, "grow -1 pages -> -1");
    }
    { /* memory.grow delta 0 succeeds and reports the current size. */
        uint8_t code[16];
        size_t n = 0u;
        code[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(code + n, 0);
        code[n++] = (uint8_t)OP_MEMORY_GROW; code[n++] = 0x00;
        CHECK(exec_bytes(code, n, 0u, 0u) == WASM_TRAP_OK, "run grow 0");
        expect_i32(1, "grow 0 -> 1 (current pages)");
    }
    { /* successful grow publishes a new page that size/loads can see. */
        uint8_t grow[16];
        size_t n = 0u;
        static const uint8_t size[] = { 0x3F, 0x00 };
        grow[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(grow + n, 1);
        grow[n++] = (uint8_t)OP_MEMORY_GROW; grow[n++] = 0x00;
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init grow-2 vm");
        CHECK(wasm_memory_init(&g_mem2, g_arena2, (uint32_t)sizeof(g_arena2), 1u, 2u) == WASM_TRAP_OK, "1-page mem, max 2");
        CHECK(wasm_vm_bind_memory(&g_vm, &g_mem2) == WASM_TRAP_OK, "bind grow-2 arena");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "enter");
        CHECK(wasm_vm_attach_code(&g_vm, grow, n) == WASM_TRAP_OK, "attach grow");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run grow 1");
        expect_i32(1, "grow 1 from 1 page -> prev 1");
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "re-init for size");
        CHECK(wasm_vm_bind_memory(&g_vm, &g_mem2) == WASM_TRAP_OK, "re-bind grown mem");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "re-enter");
        CHECK(wasm_vm_attach_code(&g_vm, size, sizeof(size)) == WASM_TRAP_OK, "attach size");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run size after grow");
        expect_i32(2, "size == 2 after grow");
    }
    { /* nonzero reserved byte rejected; missing reserved overruns. */
        static const uint8_t bad[] = { 0x3F, 0x01 };
        static const uint8_t bare[] = { 0x3F };
        static const uint8_t badg[] = { 0x41, 0x00, 0x40, 0x02 };
        CHECK(exec_bytes(bad, sizeof(bad), 0u, 0u) == WASM_TRAP_INVALID_ARGUMENT, "size reserved 0x01 -> INVALID_ARGUMENT");
        CHECK(exec_bytes(bare, sizeof(bare), 0u, 0u) == WASM_TRAP_CODE_OVERRUN, "size w/o reserved -> CODE_OVERRUN");
        CHECK(exec_bytes(badg, sizeof(badg), 0u, 0u) == WASM_TRAP_INVALID_ARGUMENT, "grow reserved 0x02 -> INVALID_ARGUMENT");
    }
    { /* unbound memory: size is 0, grow is -1, loads still trap. */
        static const uint8_t size[] = { 0x3F, 0x00 };
        uint8_t grow[16];
        size_t n = 0u;
        uint8_t load[16];
        size_t m = 0u;
        grow[n++] = (uint8_t)OP_I32_CONST; n += enc_s32(grow + n, 1);
        grow[n++] = (uint8_t)OP_MEMORY_GROW; grow[n++] = 0x00;
        load[m++] = (uint8_t)OP_I32_CONST; m += enc_s32(load + m, 0);
        load[m++] = (uint8_t)OP_I64_LOAD8_U; load[m++] = 0x00; load[m++] = 0x00;
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init (no memory bound)");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "enter");
        CHECK(wasm_vm_attach_code(&g_vm, size, sizeof(size)) == WASM_TRAP_OK, "attach size");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run unbound size");
        expect_i32(0, "unbound size == 0");
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "re-init (no memory bound)");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "re-enter");
        CHECK(wasm_vm_attach_code(&g_vm, grow, n) == WASM_TRAP_OK, "attach grow");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run unbound grow");
        expect_i32(-1, "unbound grow -> -1");
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "re-init (no memory bound)");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "re-enter");
        CHECK(wasm_vm_attach_code(&g_vm, load, m) == WASM_TRAP_OK, "attach load");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "unbound sub-word load -> OOB");
    }
    { /* mistyped operands trap: i64 base, i64 value for i32 store8. */
        uint8_t k1[16];
        size_t n = 0u;
        uint8_t k2[32];
        size_t m = 0u;
        k1[n++] = 0x42u; k1[n++] = 0x00;
        k1[n++] = (uint8_t)OP_I64_LOAD; k1[n++] = 0x03; k1[n++] = 0x00;
        CHECK(exec_bytes(k1, n, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "i64.load i64 base -> TYPE_MISMATCH");
        k2[m++] = (uint8_t)OP_I32_CONST; m += enc_s32(k2 + m, 8);
        k2[m++] = 0x42u; k2[m++] = 0x01;
        k2[m++] = (uint8_t)OP_I32_STORE8; k2[m++] = 0x00; k2[m++] = 0x00;
        CHECK(exec_bytes(k2, m, 0u, 0u) == WASM_TRAP_TYPE_MISMATCH, "i32.store8 i64 value -> TYPE_MISMATCH");
    }
    CHECK((uint8_t)OP_I64_LOAD == 0x29u && (uint8_t)OP_I32_LOAD8_S == 0x2Cu &&
          (uint8_t)OP_I32_LOAD16_U == 0x2Fu && (uint8_t)OP_I64_LOAD8_S == 0x30u &&
          (uint8_t)OP_I64_LOAD16_U == 0x33u && (uint8_t)OP_I64_LOAD32_S == 0x34u &&
          (uint8_t)OP_I64_LOAD32_U == 0x35u && (uint8_t)OP_I64_STORE == 0x37u &&
          (uint8_t)OP_I32_STORE8 == 0x3Au && (uint8_t)OP_I32_STORE16 == 0x3Bu &&
          (uint8_t)OP_I64_STORE8 == 0x3Cu && (uint8_t)OP_I64_STORE16 == 0x3Du &&
          (uint8_t)OP_I64_STORE32 == 0x3Eu && (uint8_t)OP_MEMORY_SIZE == 0x3Fu &&
          (uint8_t)OP_MEMORY_GROW == 0x40u, "extended mem opcode encodings");
}

static void test_consts_locals_traps(void)
{
    printf("[T] I64 const, locals via step, unknown/unreachable/overrun traps\n");
    {
        static const uint8_t c[] = { 0x42, 0x05 };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run i64.const 5");
        expect_i64(5, "i64 5 on stack");
    }
    {
        static const uint8_t c[] = { 0x42, 0x7F };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run i64.const -1");
        expect_i64(-1, "i64 -1 on stack");
    }
    { /* locals through the step path: const 42; set 1; get 1. */
        static const uint8_t c[] = { 0x41, 0x2A, 0x21, 0x01, 0x20, 0x01 };
        CHECK(exec_bytes(c, sizeof(c), 2u, 0u) == WASM_TRAP_OK, "run const/set/get local");
        expect_i32(42, "local.get returns 42");
    }
    { /* tee keeps the value on the stack. */
        static const uint8_t c[] = { 0x41, 0x07, 0x22, 0x00 };
        CHECK(exec_bytes(c, sizeof(c), 1u, 0u) == WASM_TRAP_OK, "run const/tee local");
        expect_i32(7, "tee leaves 7 on stack");
    }
    { /* OOB local index through step decodes then traps. */
        static const uint8_t c[] = { 0x20, 0x09 };
        CHECK(exec_bytes(c, sizeof(c), 2u, 0u) == WASM_TRAP_INVALID_LOCAL_INDEX, "get idx 9 of 2 -> INVALID_LOCAL_INDEX");
    }
    {
        static const uint8_t c[] = { 0xFF };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_UNKNOWN_OPCODE, "0xFF -> UNKNOWN_OPCODE");
    }
    {
        static const uint8_t c[] = { 0x00 };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_UNREACHABLE, "unreachable -> UNREACHABLE");
    }
    {
        static const uint8_t c[] = { 0x41 };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_CODE_OVERRUN, "bare const opcode -> CODE_OVERRUN");
    }
    {
        static const uint8_t c[] = { 0x41, 0x80 };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_CODE_OVERRUN, "unterminated SLEB -> CODE_OVERRUN");
    }
    {
        static const uint8_t c[] = { 0x20 };
        CHECK(exec_bytes(c, sizeof(c), 1u, 0u) == WASM_TRAP_CODE_OVERRUN, "local.get w/o idx -> CODE_OVERRUN");
    }
    {
        static const uint8_t c[] = { 0x28, 0x00 };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_CODE_OVERRUN, "load w/o offset -> CODE_OVERRUN");
    }
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init for no-frame step");
    CHECK(wasm_vm_step(&g_vm) == WASM_TRAP_CALL_STACK_UNDERFLOW, "step with no frame -> UNDERFLOW");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run with no frames -> OK (nothing to do)");
    CHECK(wasm_vm_step(NULL) == WASM_TRAP_NULL, "step NULL -> NULL");
    CHECK(wasm_vm_run(NULL) == WASM_TRAP_NULL, "run NULL -> NULL");
}

static void test_footprint(void)
{
    printf("[T] footprint — struct sizes\n");
    printf("  sizeof(wasm_frame_t)=%u sizeof(wasm_call_stack_t)=%u sizeof(wasm_vm_t)=%u\n",
           (unsigned)sizeof(wasm_frame_t),
           (unsigned)sizeof(wasm_call_stack_t),
           (unsigned)sizeof(wasm_vm_t));
    CHECK(sizeof(wasm_frame_t) == 4u * (unsigned)sizeof(uint32_t) +
          2u * (unsigned)sizeof(const uint8_t *) +
          (unsigned)sizeof(wasm_control_stack_t) +
          (unsigned)WASM_MAX_LOCALS_PER_FRAME * (unsigned)sizeof(wasm_val_t), "frame layout exact");
    CHECK(sizeof(wasm_call_stack_t) >= (unsigned)WASM_MAX_CALL_STACK_DEPTH * (unsigned)sizeof(wasm_frame_t),
          "call stack holds 64 frames");
    CHECK(sizeof(wasm_vm_t) >= sizeof(wasm_stack_t) + sizeof(wasm_call_stack_t), "vm == operands + calls + membind");
}

int main(void)
{
    static const harness_test_t tests[] = {
        { "arith_basic", test_arith_basic },
        { "div_by_zero", test_div_by_zero },
        { "min_div_neg1", test_min_div_neg1 },
        { "bits_shifts", test_bits_shifts },
        { "compare", test_compare },
        { "mem_ops", test_mem_ops },
        { "mem_float", test_mem_float },
        { "mem_extended", test_mem_extended },
        { "bitops_i64", test_bitops_i64 },
        { "consts_locals_traps", test_consts_locals_traps },
        { "footprint", test_footprint },
    };
    printf("=== Phase 4 tests: execution core (i32 ALU, compare, mem) ===\n");
    return harness_run("phase4", tests,
                       sizeof(tests) / sizeof(tests[0]));
}

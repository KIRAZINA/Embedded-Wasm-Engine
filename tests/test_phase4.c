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

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

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
    check_binop_trap(INT32_MIN, -1, (uint8_t)OP_I32_REM_S,
                     WASM_TRAP_INTEGER_OVERFLOW, "MIN%-1 REM_S -> INTEGER_OVERFLOW");
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
    printf("=== Phase 4 tests: execution core (i32 ALU, compare, mem) ===\n");
    test_arith_basic();
    test_div_by_zero();
    test_min_div_neg1();
    test_bits_shifts();
    test_compare();
    test_mem_ops();
    test_mem_float();
    test_consts_locals_traps();
    test_footprint();
    printf("--------------------------------------------\n");
    printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("STATUS: FAIL\n");
        return 1;
    }
    printf("STATUS: ALL TESTS PASSED\n");
    return 0;
}

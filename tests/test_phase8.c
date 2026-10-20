/*
 * test_phase8.c — Phase 2 (control-flow completion): br_table + select.
 * Standalone, C11, assert-style. br_table vectors are emitted at runtime
 * (SLEB/ULEB encoders below) so label bytes are exact by construction.
 *
 * Covers: br_table all-arms/default/0xFFFFFFFF/nested depths/br-to-LOOP/
 * n=0/malformed count/cap/empty-stack/bad-label/scanner interplay;
 * select over i32/i64/f32/f64 incl. negative conditions, NaN operands,
 * mismatches, underflows, exact stack effect; CLI run of dispatch.wasm.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $(SRC)
 *        tests/test_phase8.c -o build/test_phase8 $(LDLIBS)
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_vm.h"
#include "harness.h"


static wasm_vm_t g_vm;

/* Fresh VM, empty frame, attach, run to completion (no params/locals). */
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

/* ULEB128 encode (labels, counts). */
static size_t emit_u32(uint8_t *b, uint32_t v)
{
    size_t n = 0u;
    for (;;) {
        uint8_t byte = (uint8_t)(v & 0x7Fu);
        v >>= 7u;
        if (v == 0u) {
            b[n++] = byte;
            return n;
        }
        b[n++] = (uint8_t)(byte | 0x80u);
    }
}

/* SLEB128 encode (i32.const immediates), aliasing-safe. */
static size_t emit_i32_const(uint8_t *b, int32_t v)
{
    uint32_t u;
    size_t n = 1u;
    memcpy(&u, &v, sizeof(u));
    b[0] = 0x41u;
    for (;;) {
        uint8_t byte = (uint8_t)(u & 0x7Fu);
        uint32_t sign = u & 0x80000000u;
        u = (u >> 7u) | (sign != 0u ? 0xFE000000u : 0u);
        if ((u == 0u && (byte & 0x40u) == 0u) ||
            (u == 0xFFFFFFFFu && (byte & 0x40u) != 0u)) {
            b[n++] = byte;
            return n;
        }
        b[n++] = (uint8_t)(byte | 0x80u);
    }
}

/*
 * Emit a dispatch probe: (narms+1) nested void blocks (labels narms..0
 * outside-in), `i32.const idx; br_table [0..narms-1] default narms`, then
 * after each inner end either `br` out past the outermost end (the wanted
 * arm) or `unreachable` (every other arm). Correct dispatch runs clean
 * with an empty stack; any misdispatch traps on unreachable.
 */
static size_t emit_probe(uint8_t *b, uint32_t narms, int32_t idx,
                         uint32_t want)
{
    size_t n = 0u;
    uint32_t k;
    for (k = 0u; k < narms + 1u; k++) {
        b[n++] = 0x02u;
        b[n++] = 0x40u;
    }
    n += emit_i32_const(b + n, idx);
    b[n++] = 0x0Eu; /* br_table */
    n += emit_u32(b + n, narms);
    for (k = 0u; k < narms; k++) {
        n += emit_u32(b + n, k);
    }
    n += emit_u32(b + n, narms); /* default */
    b[n++] = 0x00u; /* unreachable: fallthrough detector */
    for (k = 0u; k < narms; k++) {
        b[n++] = 0x0Bu; /* end of L_k */
        if (k == want) {
            b[n++] = 0x0Cu; /* br: exit past the outermost end */
            n += emit_u32(b + n, narms - k - 1u);
        } else {
            b[n++] = 0x00u; /* unreachable: wrong arm */
        }
    }
    b[n++] = 0x0Bu; /* end of outermost block */
    return n;
}

static void probe_ok(uint32_t narms, int32_t idx, uint32_t want,
                     const char *name)
{
    uint8_t c[96];
    size_t n = emit_probe(c, narms, idx, want);
    CHECK(run_flat(c, n) == WASM_TRAP_OK, name);
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "probe left no residue");
}

static void probe_traps(uint32_t narms, int32_t idx, uint32_t want,
                        const char *name)
{
    uint8_t c[96];
    size_t n = emit_probe(c, narms, idx, want);
    CHECK(run_flat(c, n) == WASM_TRAP_UNREACHABLE, name);
}

/* ---------- br_table: every arm at several nesting depths ---------- */

static void test_all_arms(void)
{
    printf("[T] br_table all arms + default at depths 2,3,4\n");
    probe_ok(2u, 0, 0u, "depth2 idx0 -> L0");
    probe_ok(2u, 1, 1u, "depth2 idx1 -> L1");
    probe_ok(2u, 9, 2u, "depth2 idx9 -> default");
    probe_ok(3u, 0, 0u, "depth3 idx0 -> L0");
    probe_ok(3u, 1, 1u, "depth3 idx1 -> L1");
    probe_ok(3u, 2, 2u, "depth3 idx2 -> L2");
    probe_ok(3u, 5, 3u, "depth3 idx5 -> default");
    probe_ok(4u, 0, 0u, "depth4 idx0 -> L0");
    probe_ok(4u, 1, 1u, "depth4 idx1 -> L1");
    probe_ok(4u, 2, 2u, "depth4 idx2 -> L2");
    probe_ok(4u, 3, 3u, "depth4 idx3 -> L3");
    probe_ok(4u, 50, 4u, "depth4 idx50 -> default");
    probe_traps(2u, 1, 0u, "idx1 must not land on L0");
    probe_traps(2u, 0, 1u, "idx0 must not land on L1");
}

/* ---------- br_table: unsigned index, n=0, loop target ---------- */

static void test_index_unsigned(void)
{
    uint8_t c[96];
    size_t n;
    printf("[T] br_table unsigned index: 0xFFFFFFFF -> default\n");
    n = emit_probe(c, 2u, -1, 2u);
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "index -1 selects default");
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "no residue");
    n = emit_probe(c, 2u, INT32_MIN, 2u);
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "index INT32_MIN selects default");
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "no residue");
}

static void test_n0_default_only(void)
{
    /* block; i32.const 5; br_table(count 0, default 0); unreachable; end */
    static const uint8_t k[] = { 0x02, 0x40, 0x41, 0x05, 0x0E, 0x00, 0x00,
                                 0x00, 0x0B };
    printf("[T] br_table n=0 (default only)\n");
    CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "n=0 dispatches default");
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "no residue");
}

static void test_loop_target(void)
{
    /*
     * counter=3 in local 0; block L1 { loop L0 {
     *   get 0; const 1; sub; tee 0; br_table {1,0} dflt 0 } };
     * get 0; const 42. Index is counter-1: 2/1 (and default) rewind
     * the LOOP (label 0); 0 exits (label 1).
     * Three iterations leave counter == 0.
     */
    uint8_t c[64];
    size_t n = 0u;
    printf("[T] br_table to a LOOP rewinds to loop start\n");
    n += emit_i32_const(c + n, 3);
    c[n++] = 0x21u; c[n++] = 0x00u;             /* local.set 0 */
    c[n++] = 0x02u; c[n++] = 0x40u;             /* block L1 */
    c[n++] = 0x03u; c[n++] = 0x40u;             /* loop L0 */
    c[n++] = 0x20u; c[n++] = 0x00u;             /* local.get 0 */
    n += emit_i32_const(c + n, 1);
    c[n++] = 0x6Bu;                             /* i32.sub */
    c[n++] = 0x22u; c[n++] = 0x00u;             /* local.tee 0 */
    c[n++] = 0x0Eu;                             /* br_table */
    n += emit_u32(c + n, 2u);                   /* count */
    n += emit_u32(c + n, 1u);                   /* labels[0] = 1 (exit) */
    n += emit_u32(c + n, 0u);                   /* labels[1] = 0 (loop) */
    n += emit_u32(c + n, 0u);                   /* default = 0 (loop) */
    c[n++] = 0x00u;                             /* unreachable */
    c[n++] = 0x0Bu;                             /* end loop */
    c[n++] = 0x00u;                             /* unreachable: loop fell out */
    c[n++] = 0x0Bu;                             /* end block */
    c[n++] = 0x20u; c[n++] = 0x00u;             /* local.get 0 */
    n += emit_i32_const(c + n, 42);
    {
        wasm_trap_t tr = wasm_vm_init(&g_vm);
        CHECK(tr == WASM_TRAP_OK, "vm init");
        if (tr != WASM_TRAP_OK) {
            return;
        }
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 1u, 0u) == WASM_TRAP_OK,
              "enter with 1 local");
        CHECK(wasm_vm_attach_code(&g_vm, c, n) == WASM_TRAP_OK, "attach");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "loop runs to exit");
    }
    expect_i32(42, "result 42 on top");
    expect_i32(0, "counter is 0: loop rewound twice, exited once");
}

/* ---------- br_table: result arity carried across the branch ---------- */

static void test_valued_carry(void)
{
    /* block(result i32) { const 7; const IDX; br_table {0,0} dflt 0 } -> 7 */
    uint8_t c[32];
    size_t n;
    printf("[T] br_table carries the block result value\n");
    c[0] = 0x02u; c[1] = 0x7Fu;
    n = 2u + emit_i32_const(c + 2u, 7);
    n += emit_i32_const(c + n, 0);
    c[n++] = 0x0Eu;
    n += emit_u32(c + n, 2u);
    n += emit_u32(c + n, 0u);
    n += emit_u32(c + n, 0u);
    n += emit_u32(c + n, 0u);
    c[n++] = 0x00u;
    c[n++] = 0x0Bu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run valued branch, label path");
    expect_i32(7, "label path carried 7");
    c[0] = 0x02u; c[1] = 0x7Fu;
    n = 2u + emit_i32_const(c + 2u, 7);
    n += emit_i32_const(c + n, 42);
    c[n++] = 0x0Eu;
    n += emit_u32(c + n, 2u);
    n += emit_u32(c + n, 0u);
    n += emit_u32(c + n, 0u);
    n += emit_u32(c + n, 0u);
    c[n++] = 0x00u;
    c[n++] = 0x0Bu;
    CHECK(run_flat(c, n) == WASM_TRAP_OK, "run valued branch, default path");
    expect_i32(7, "default path carried 7");
}

/* ---------- br_table: malformed encodings rejected ---------- */

static void test_malformed(void)
{
    printf("[T] br_table malformed encodings rejected\n");
    {
        /* count=5 but only 1 byte remains: 5+1 labels need >= 6 bytes. */
        static const uint8_t k[] = { 0x0E, 0x05, 0x00 };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_CODE_OVERRUN,
              "count exceeds remaining bytes");
    }
    {
        /* count=1, label present, default missing. */
        static const uint8_t k[] = { 0x0E, 0x01, 0x00 };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_CODE_OVERRUN,
              "missing default byte overruns");
    }
    {
        /* truncated count immediate. */
        static const uint8_t k[] = { 0x0E };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_CODE_OVERRUN,
              "truncated count overruns");
    }
    {
        /* count=257 exceeds WASM_MAX_BR_TABLE_LABELS (256). */
        static const uint8_t k[] = { 0x0E, 0x81, 0x02, 0x00, 0x00 };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_INVALID_ARGUMENT,
              "over-cap count rejected without proportional reads");
    }
    {
        /* valid encoding, empty operand stack. */
        static const uint8_t k[] = { 0x02, 0x40, 0x0E, 0x00, 0x00,
                                     0x00, 0x0B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_STACK_UNDERFLOW,
              "missing index underflows");
    }
    {
        /* label 5 with only 1 open block. */
        static const uint8_t k[] = { 0x02, 0x40, 0x41, 0x00, 0x0E, 0x01,
                                     0x05, 0x00, 0x00, 0x0B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_INVALID_BLOCK_LABEL,
              "out-of-range label rejected at branch time");
    }
}

/* ---------- br_table: forward scanner interplay ---------- */

static void test_scanner(void)
{
    /*
     * const 0; if { const 1; br_table {11=0x0B} dflt 0 }; end; const 9.
     * The untaken branch holds a 0x0B label byte; the scanner must skip
     * the table as immediates (not as END) and land on the real end.
     */
    static const uint8_t k[] = { 0x41, 0x00, 0x04, 0x40, 0x41, 0x01,
                                 0x0E, 0x01, 0x0B, 0x00, 0x0B, 0x41, 0x09 };
    printf("[T] scanner skips br_table immediates (0x0B label byte)\n");
    CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run with skipped table");
    expect_i32(9, "landed on the true end");
}

/* ---------- select over all four types ---------- */

static void test_select_i32(void)
{
    printf("[T] select i32 incl. negative conditions\n");
    { /* [10, 20, 1] -> 10 */
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x14, 0x41, 0x01,
                                     0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select true");
        expect_i32(10, "cond 1 picks first");
    }
    { /* [10, 20, 0] -> 20 */
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x14, 0x41, 0x00,
                                     0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select false");
        expect_i32(20, "cond 0 picks second");
    }
    { /* [10, 20, -1] -> 10 */
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x14, 0x41, 0x7F,
                                     0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select -1");
        expect_i32(10, "cond -1 (nonzero) picks first");
    }
    { /* INT32_MIN condition -> first */
        static const uint8_t k[] = { 0x41, 0x05, 0x41, 0x06, 0x41, 0x80,
                                     0x80, 0x80, 0x80, 0x78, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select MIN");
        expect_i32(5, "cond INT32_MIN picks first");
    }
}

static void test_select_i64(void)
{
    printf("[T] select i64\n");
    { /* [100, 200, 0] -> 200 */
        static const uint8_t k[] = { 0x42, 0xE4, 0x00, 0x42, 0xC8, 0x01,
                                     0x41, 0x00, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select i64");
        expect_i64(200, "cond 0 picks second i64");
    }
    { /* [100, 200, -5] -> 100 */
        static const uint8_t k[] = { 0x42, 0xE4, 0x00, 0x42, 0xC8, 0x01,
                                     0x41, 0x7B, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select i64 neg");
        expect_i64(100, "cond -5 picks first i64");
    }
}

static void test_select_f32(void)
{
    printf("[T] select f32 incl. NaN operand\n");
    { /* [1.5, 2.5, 1] -> 1.5 */
        static const uint8_t k[] = { 0x43, 0x00, 0x00, 0xC0, 0x3F,
                                     0x43, 0x00, 0x00, 0x20, 0x40,
                                     0x41, 0x01, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select f32");
        expect_f32_bits(0x3FC00000u, "cond 1 picks first f32");
    }
    { /* [1.5, 2.5, 0] -> 2.5 */
        static const uint8_t k[] = { 0x43, 0x00, 0x00, 0xC0, 0x3F,
                                     0x43, 0x00, 0x00, 0x20, 0x40,
                                     0x41, 0x00, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select f32 zero");
        expect_f32_bits(0x40200000u, "cond 0 picks second f32");
    }
    { /* [nan:0x1, 1.0, 1] -> nan:0x1 (no trap, payload kept) */
        static const uint8_t k[] = { 0x43, 0x01, 0x00, 0xC0, 0x7F,
                                     0x43, 0x00, 0x00, 0x80, 0x3F,
                                     0x41, 0x01, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select NaN f32");
        expect_f32_bits(0x7FC00001u, "NaN selected bit-exact");
    }
}

static void test_select_f64(void)
{
    printf("[T] select f64\n");
    { /* [1.5, 2.5, 0] -> 2.5 */
        static const uint8_t k[] = { 0x44, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0xF8, 0x3F, 0x44, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x04, 0x40,
                                     0x41, 0x00, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select f64");
        expect_f64_bits(0x4004000000000000ULL, "cond 0 picks second f64");
    }
    { /* [1.5, 2.5, -1] -> 1.5 */
        static const uint8_t k[] = { 0x44, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0xF8, 0x3F, 0x44, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x04, 0x40,
                                     0x41, 0x7F, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select f64 neg");
        expect_f64_bits(0x3FF8000000000000ULL, "cond -1 picks first f64");
    }
    { /* [nan, 1.0, 0] -> 1.0: unselected NaN never surfaces */
        static const uint8_t k[] = { 0x44, 0x01, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0xF8, 0x7F, 0x44, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F,
                                     0x41, 0x00, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select f64 NaN");
        expect_f64_bits(0x3FF0000000000000ULL, "second picked over NaN");
    }
}

/* ---------- select: mismatches, underflows, exact stack effect ---------- */

static void test_select_traps(void)
{
    printf("[T] select type mismatches trap\n");
    { /* condition is i64, not i32 */
        static const uint8_t k[] = { 0x41, 0x01, 0x41, 0x02, 0x42, 0x01,
                                     0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_TYPE_MISMATCH,
              "select cond i64 traps");
    }
    { /* v2=i64, v1=i32 */
        static const uint8_t k[] = { 0x42, 0x01, 0x41, 0x02, 0x41, 0x01,
                                     0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_TYPE_MISMATCH,
              "i64/i32 mismatch traps");
        CHECK(wasm_stack_depth(&g_vm.operands) == 3u, "stack restored");
    }
    { /* v2=i32, v1=i64 */
        static const uint8_t k[] = { 0x41, 0x01, 0x42, 0x02, 0x41, 0x01,
                                     0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_TYPE_MISMATCH,
              "i32/i64 mismatch traps");
        CHECK(wasm_stack_depth(&g_vm.operands) == 3u, "stack restored");
    }
    { /* v2=f32, v1=i32 */
        static const uint8_t k[] = { 0x43, 0x00, 0x00, 0xC0, 0x3F,
                                     0x41, 0x01, 0x41, 0x01, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_TYPE_MISMATCH,
              "f32/i32 mismatch traps");
        CHECK(wasm_stack_depth(&g_vm.operands) == 3u, "stack restored");
    }
    { /* v2=f64, v1=i64 */
        static const uint8_t k[] = { 0x44, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0xF8, 0x3F, 0x42, 0x01,
                                     0x41, 0x01, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_TYPE_MISMATCH,
              "f64/i64 mismatch traps");
        CHECK(wasm_stack_depth(&g_vm.operands) == 3u, "stack restored");
    }
}

static void test_select_stack(void)
{
    printf("[T] select underflows + exact stack effect\n");
    {
        static const uint8_t k[] = { 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_STACK_UNDERFLOW,
              "bare select underflows");
    }
    {
        static const uint8_t k[] = { 0x41, 0x01, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_STACK_UNDERFLOW,
              "select with cond only underflows");
    }
    {
        static const uint8_t k[] = { 0x41, 0x01, 0x41, 0x02, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_STACK_UNDERFLOW,
              "select missing second operand underflows");
    }
    { /* 3 in, 1 out: condition consumed exactly once */
        static const uint8_t k[] = { 0x41, 0x0A, 0x41, 0x14, 0x41, 0x01,
                                     0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run select depth");
        CHECK(wasm_stack_depth(&g_vm.operands) == 1u, "3 pops + 1 push");
        expect_i32(10, " survivors value correct");
    }
    { /* chained selects preserve types end to end */
        static const uint8_t k[] = { 0x41, 0x01, 0x41, 0x02, 0x41, 0x01,
                                     0x1B, 0x41, 0x0A, 0x41, 0x14, 0x41,
                                     0x00, 0x1B };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run chained select");
        expect_i32(20, "select(select(1,2,1),10,20,0) == 20");
    }
}

/* ---------- drop + nop (MVP parametric / no-op) ---------- */

static void test_drop(void)
{
    printf("[T] drop + nop execution\n");
    { /* drop after const: one value consumed exactly */
        static const uint8_t k[] = { 0x41, 0x2A, 0x1A, 0x41, 0x07 };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run drop after const");
        CHECK(wasm_stack_depth(&g_vm.operands) == 1u, "drop pops exactly one");
        expect_i32(7, "survivor after drop correct");
    }
    { /* nop leaves the stack untouched */
        static const uint8_t k[] = { 0x01, 0x41, 0x05, 0x01 };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run nops");
        CHECK(wasm_stack_depth(&g_vm.operands) == 1u, "nop keeps depth");
        expect_i32(5, "value after nops correct");
    }
    { /* drop inside a block, then branch out */
        static const uint8_t k[] = { 0x02, 0x40, 0x41, 0x09, 0x1A, 0x0B,
                                     0x41, 0x03 };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run drop in block");
        CHECK(wasm_stack_depth(&g_vm.operands) == 1u, "block drop depth");
        expect_i32(3, "value after block drop correct");
    }
    { /* bare drop traps cleanly, stack stays empty */
        static const uint8_t k[] = { 0x1A };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_STACK_UNDERFLOW,
              "bare drop underflows");
        CHECK(wasm_stack_depth(&g_vm.operands) == 0u, "stack empty after trap");
    }
    { /* drop discards the top, deeper values survive in order */
        static const uint8_t k[] = { 0x41, 0x01, 0x41, 0x02, 0x1A };
        CHECK(run_flat(k, sizeof(k)) == WASM_TRAP_OK, "run stacked drop");
        CHECK(wasm_stack_depth(&g_vm.operands) == 1u, "one survivor");
        expect_i32(1, "drop keeps deeper value");
    }
    { /* enum spellings keep the matrix honest about coverage */
        CHECK((uint8_t)OP_DROP == 0x1Au && (uint8_t)OP_NOP == 0x01u,
              "OP_DROP/OP_NOP encodings");
    }
}

/* ---------- CLI run of the dispatch demo ---------- */

static void test_cli_dispatch(void)
{
#ifdef _WIN32
    static const char *const fmt =
        "build\\wasm_runner.exe demo\\dispatch.wasm main %d > build\\cli_dispatch_%d.txt";
#else
    static const char *const fmt =
        "./build/wasm_runner demo/dispatch.wasm main %d > build/cli_dispatch_%d.txt";
#endif
    static const struct {
        int arg;
        const char *want;
    } rows[] = {
        { 0, "result: 100 (i32)" },
        { 1, "result: 200 (i32)" },
        { 2, "result: 300 (i32)" },
        { 99, "result: 300 (i32)" },
    };
    static char out[1024];
    char cmd[256];
    char path[64];
    size_t i, n;
    FILE *fp = NULL;
    printf("[T] CLI dispatch demo via br_table\n");
    for (i = 0u; i < sizeof(rows) / sizeof(rows[0]); i++) {
        sprintf(cmd, fmt, rows[i].arg, rows[i].arg);
        sprintf(path, "build/cli_dispatch_%d.txt", rows[i].arg);
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
        { "all_arms", test_all_arms },
        { "index_unsigned", test_index_unsigned },
        { "n0_default_only", test_n0_default_only },
        { "loop_target", test_loop_target },
        { "valued_carry", test_valued_carry },
        { "malformed", test_malformed },
        { "scanner", test_scanner },
        { "select_i32", test_select_i32 },
        { "select_i64", test_select_i64 },
        { "select_f32", test_select_f32 },
        { "select_f64", test_select_f64 },
        { "select_traps", test_select_traps },
        { "select_stack", test_select_stack },
        { "drop", test_drop },
        { "cli_dispatch", test_cli_dispatch },
    };
    printf("=== Phase 8 tests: br_table + select ===\n");
    return harness_run("phase8", tests,
                       sizeof(tests) / sizeof(tests[0]));
}

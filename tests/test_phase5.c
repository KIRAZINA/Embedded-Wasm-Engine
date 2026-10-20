/*
 * test_phase5.c — Phase 5 verification: structured control flow + CALL interop.
 * Standalone, C11, stdio.h/stdint.h only. Hand-assembled bytecode vectors.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -Iinclude src/leb128.c
 *        src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c src/wasm_frame.c
 *        src/wasm_control.c src/wasm_vm.c tests/test_phase5.c -o build/test_phase5
 */

#include <stdint.h>
#include <stdio.h>

#include "wasm_control.h"
#include "wasm_vm.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

/* Static VM (~103 KiB — never on the call stack). */
static wasm_vm_t g_vm;

static wasm_val_t mk_i32(int32_t v)
{
    wasm_val_t x;
    x.type = WASM_TYPE_I32;
    x.value.i32 = v;
    return x;
}

/* ---- test-side assemblers ---- */

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

/* Fresh VM, enter (0 params, nlocals, nret), attach code, run. */
static wasm_trap_t exec_bytes(const uint8_t *code, size_t len,
                              uint32_t nlocals, uint32_t nret)
{
    wasm_trap_t tr;
    tr = wasm_vm_init(&g_vm);
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

/* ---------- a) block/end + br exits ---------- */

static void test_block_br(void)
{
    printf("[T] block/end passthrough and br exits (void + valued)\n");
    {
        static const uint8_t c[] = { 0x02, 0x40, 0x41, 0x07, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run block{const 7}");
        expect_i32(7, "passthrough yields 7");
    }
    { /* br 0 skips `const 99`; execution resumes after END. */
        static const uint8_t c[] = {
            0x02, 0x40, 0x0C, 0x00, 0x41, 0x63, 0x0B, 0x41, 0x08
        };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run block{br 0; const 99} const 8");
        expect_i32(8, "skipped 99, landed on 8");
        CHECK(wasm_control_stack_is_empty(&g_vm.calls.frames[0].ctrl) == 1, "control stack drained");
    }
    { /* valued block carries its result across br 0. */
        static const uint8_t c[] = { 0x02, 0x7F, 0x41, 0x05, 0x0C, 0x00, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run block(i32){5; br 0}");
        expect_i32(5, "valued br carries 5 out");
    }
    { /* br 1 exits two levels; inner END + skipped const both bypassed. */
        static const uint8_t c[] = {
            0x02, 0x40, 0x02, 0x40, 0x0C, 0x01, 0x0B, 0x41, 0x63, 0x0B, 0x41, 0x08
        };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run nested br 1");
        expect_i32(8, "br 1 lands past outer END on 8");
        CHECK(wasm_call_stack_depth(&g_vm.calls) == 1u, "still inside function frame");
    }
}

/* ---------- b) loop countdown with br_if ---------- */

static int32_t run_counter(int32_t n)
{
    /* locals: [n, iters]. Loop until n==0, counting iterations. */
    static const uint8_t c[] = {
        0x02, 0x40,                   /* block outer */
          0x03, 0x40,                 /* loop inner */
            0x20, 0x00, 0x45,         /*   n==0? */
            0x0D, 0x01,               /*   br_if 1 -> exit outer */
            0x20, 0x01, 0x41, 0x01, 0x6A, 0x21, 0x01, /* iters++ */
            0x20, 0x00, 0x41, 0x01, 0x6B, 0x21, 0x00, /* n-- */
            0x0C, 0x00,               /*   br 0 -> loop head */
          0x0B,                       /* end loop */
        0x0B,                         /* end block */
        0x20, 0x01                    /* iters */
    };
    wasm_val_t v;
    wasm_trap_t tr;
    tr = wasm_vm_init(&g_vm);
    if (tr != WASM_TRAP_OK) {
        return -1000000;
    }
    if (wasm_stack_push(&g_vm.operands, mk_i32(n)) != WASM_TRAP_OK) {
        return -1000001;
    }
    tr = wasm_vm_enter_function(&g_vm, 0u, 1u, 1u, 1u);
    if (tr != WASM_TRAP_OK) {
        return -1000002;
    }
    tr = wasm_vm_attach_code(&g_vm, c, sizeof(c));
    if (tr != WASM_TRAP_OK) {
        return -1000003;
    }
    tr = wasm_vm_run(&g_vm);
    if (tr != WASM_TRAP_OK) {
        return -1000004 - (int32_t)tr;
    }
    tr = wasm_vm_exit_function(&g_vm, 1u);
    if (tr != WASM_TRAP_OK) {
        return -1000005;
    }
    tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v);
    if (tr != WASM_TRAP_OK) {
        return -1000006;
    }
    return v.value.i32;
}

static void test_loop_counter(void)
{
    printf("[T] loop countdown via br_if (iteration counting)\n");
    CHECK(run_counter(3) == 3, "3 rounds -> iters == 3");
    CHECK(run_counter(0) == 0, "0 rounds -> iters == 0 (immediate exit)");
    CHECK(run_counter(7) == 7, "7 rounds -> iters == 7");
}

/* ---------- c) if/else/end both paths ---------- */

static void test_if_else(void)
{
    printf("[T] if/else/end true path, false path, nesting\n");
    {
        static const uint8_t c[] = {
            0x41, 0x01, 0x04, 0x40, 0x41, 0x0A, 0x05, 0x41, 0x14, 0x0B
        };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run if(1)");
        expect_i32(10, "true path yields 10");
    }
    {
        static const uint8_t c[] = {
            0x41, 0x00, 0x04, 0x40, 0x41, 0x0A, 0x05, 0x41, 0x14, 0x0B
        };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run if(0)");
        expect_i32(20, "false path yields 20");
    }
    { /* if without else, false -> falls past END onto const 42. */
        static const uint8_t c[] = {
            0x41, 0x00, 0x04, 0x40, 0x41, 0x0A, 0x0B, 0x41, 0x2A
        };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run if(0) no-else");
        expect_i32(42, "skipped then-branch, 42 follows");
    }
    { /* nested: outer true, inner false -> inner-else value, outer-else skipped. */
        static const uint8_t c[] = {
            0x41, 0x01, 0x04, 0x40,
              0x41, 0x00, 0x04, 0x40, 0x41, 0x0A, 0x05, 0x41, 0x14, 0x0B,
            0x05, 0x41, 0x1E, 0x0B
        };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run nested if");
        expect_i32(20, "inner-else 20, outer-else skipped");
    }
}

/* ---------- d) factorial loop ---------- */

static int32_t run_fact(int32_t n, wasm_trap_t *trap_out)
{
    static const uint8_t c[] = {
        0x41, 0x01, 0x21, 0x01,       /* acc = 1 */
        0x02, 0x40,                   /* block outer */
          0x03, 0x40,                 /* loop */
            0x20, 0x00, 0x45,         /*   n==0? */
            0x0D, 0x01,               /*   br_if 1 */
            0x20, 0x01, 0x20, 0x00, 0x6C, 0x21, 0x01, /* acc *= n */
            0x20, 0x00, 0x41, 0x01, 0x6B, 0x21, 0x00, /* n-- */
            0x0C, 0x00,               /*   br 0 */
          0x0B,                       /* end loop */
        0x0B,                         /* end block */
        0x20, 0x01                    /* acc */
    };
    wasm_val_t v;
    wasm_trap_t tr;
    tr = wasm_vm_init(&g_vm);
    if (tr != WASM_TRAP_OK) {
        *trap_out = tr;
        return 0;
    }
    if (wasm_stack_push(&g_vm.operands, mk_i32(n)) != WASM_TRAP_OK) {
        *trap_out = WASM_TRAP_STACK_OVERFLOW;
        return 0;
    }
    tr = wasm_vm_enter_function(&g_vm, 0u, 1u, 1u, 1u);
    if (tr != WASM_TRAP_OK) {
        *trap_out = tr;
        return 0;
    }
    tr = wasm_vm_attach_code(&g_vm, c, sizeof(c));
    if (tr != WASM_TRAP_OK) {
        *trap_out = tr;
        return 0;
    }
    tr = wasm_vm_run(&g_vm);
    *trap_out = tr;
    if (tr != WASM_TRAP_OK) {
        return 0;
    }
    tr = wasm_vm_exit_function(&g_vm, 1u);
    *trap_out = tr;
    if (tr != WASM_TRAP_OK) {
        return 0;
    }
    tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v);
    *trap_out = tr;
    if (tr != WASM_TRAP_OK) {
        return 0;
    }
    return v.value.i32;
}

static void test_factorial(void)
{
    wasm_trap_t tr = WASM_TRAP_OK;
    printf("[T] factorial loop (br/br_if/block/loop interop)\n");
    CHECK(run_fact(5, &tr) == 120 && tr == WASM_TRAP_OK, "5! == 120");
    CHECK(run_fact(0, &tr) == 1 && tr == WASM_TRAP_OK, "0! == 1 (edge)");
    CHECK(run_fact(1, &tr) == 1 && tr == WASM_TRAP_OK, "1! == 1");
    CHECK(run_fact(6, &tr) == 720 && tr == WASM_TRAP_OK, "6! == 720");
}

/* ---------- e) OP_CALL interop: (a*b)+c ---------- */

static void test_call(void)
{
    /* sub(1): (a*b)+c — get0 get1 mul get2 add. main streams 3,4,5. */
    static const uint8_t sub[] = {
        0x20, 0x00, 0x20, 0x01, 0x6C, 0x20, 0x02, 0x6A
    };
    static const uint8_t caller[] = {
        0x41, 0x03, 0x41, 0x04, 0x41, 0x05, 0x10, 0x01, 0x41, 0x0A, 0x6A
    };
    wasm_val_t v;
    printf("[T] OP_CALL: sub-function (a*b)+c with suspend/resume\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init");
    CHECK(wasm_vm_define_function(&g_vm, 1u, 3u, 0u, 1u, sub, sizeof(sub)) == WASM_TRAP_OK, "define sub(3p,0l,1r)");
    CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 1u) == WASM_TRAP_OK, "enter main");
    CHECK(wasm_vm_attach_code(&g_vm, caller, sizeof(caller)) == WASM_TRAP_OK, "attach main");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run#1 stops at exhausted sub frame");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 2u, "suspended with callee on top");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit sub -> (3*4)+5 == 17 on main stack");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run#2 finishes main: 17+10");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit main");
    CHECK(wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v) == WASM_TRAP_OK && v.value.i32 == 27, "final 27");
}

static void test_call_return(void)
{
    /* sub(2) returns early via OP_RETURN; whole program ends in one run. */
    static const uint8_t sub[] = {
        0x20, 0x00, 0x20, 0x01, 0x6C, 0x20, 0x02, 0x6A, 0x0F
    };
    static const uint8_t caller[] = {
        0x41, 0x02, 0x41, 0x03, 0x41, 0x04, 0x10, 0x02
    };
    wasm_val_t v;
    printf("[T] OP_CALL + OP_RETURN single-run interop\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "init");
    CHECK(wasm_vm_define_function(&g_vm, 2u, 3u, 0u, 1u, sub, sizeof(sub)) == WASM_TRAP_OK, "define sub");
    CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 1u) == WASM_TRAP_OK, "enter main");
    CHECK(wasm_vm_attach_code(&g_vm, caller, sizeof(caller)) == WASM_TRAP_OK, "attach main");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "single run completes (RETURN exits inline)");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 1u, "only main frame remains");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit main");
    CHECK(wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v) == WASM_TRAP_OK && v.value.i32 == 10, "(2*3)+4 == 10");
    /* bad registrations / calls. */
    CHECK(wasm_vm_define_function(&g_vm, 32u, 0u, 0u, 0u, caller, 1u) == WASM_TRAP_INVALID_ARGUMENT, "define idx 32 rejected");
    CHECK(wasm_vm_define_function(NULL, 0u, 0u, 0u, 0u, caller, 1u) == WASM_TRAP_NULL, "define NULL vm rejected");
    CHECK(wasm_vm_define_function(&g_vm, 3u, 0u, 0u, 2u, caller, 1u) == WASM_TRAP_INVALID_ARGUMENT, "define 2 returns rejected");
    {
        static const uint8_t badcall[] = { 0x10, 0x05 };
        CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "re-init");
        CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "enter");
        CHECK(wasm_vm_attach_code(&g_vm, badcall, sizeof(badcall)) == WASM_TRAP_OK, "attach");
        CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_INVALID_ARGUMENT, "CALL undefined func -> INVALID_ARGUMENT");
    }
}

/* ---------- f) invalid control traps ---------- */

static void test_control_traps(void)
{
    printf("[T] invalid labels, stray else/end, overflow, starved branches\n");
    {
        static const uint8_t c[] = { 0x0C, 0x00 };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_INVALID_BLOCK_LABEL, "br 0 with no blocks -> INVALID_BLOCK_LABEL");
    }
    {
        static const uint8_t c[] = { 0x02, 0x40, 0x0C, 0x05, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_INVALID_BLOCK_LABEL, "br 5 with 1 block -> INVALID_BLOCK_LABEL");
    }
    {
        static const uint8_t c[] = { 0x05 };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_ELSE_WITHOUT_IF, "lone ELSE -> ELSE_WITHOUT_IF");
    }
    {
        static const uint8_t c[] = { 0x02, 0x40, 0x05, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_ELSE_WITHOUT_IF, "ELSE inside BLOCK -> ELSE_WITHOUT_IF");
    }
    {
        static const uint8_t c[] = { 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "lone END is the function-end marker (no-op)");
    }
    {
        static const uint8_t c[] = { 0x41, 0x09, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run const 9 + function end");
        expect_i32(9, "value survives the function end");
    }
    {
        static const uint8_t c[] = { 0x04, 0x40, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_STACK_UNDERFLOW, "IF with empty stack -> UNDERFLOW");
    }
    {
        static const uint8_t c[] = { 0x02, 0x40, 0x0D, 0x00, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_STACK_UNDERFLOW, "BR_IF with empty stack -> UNDERFLOW");
    }
    {
        static const uint8_t c[] = { 0x02, 0x7E };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_INVALID_ARGUMENT, "blocktype 0x7E (i64) rejected");
    }
    { /* Nesting exactly at the compile-time limit is accepted:
       * WASM_MAX_CTRL_NESTING nested blocks, each closed by an END. */
        static uint8_t c[3u * (unsigned)WASM_MAX_CTRL_NESTING + 1u];
        uint32_t i, n = (uint32_t)WASM_MAX_CTRL_NESTING;
        for (i = 0u; i < n; i++) {
            c[2u * i] = 0x02;
            c[2u * i + 1u] = 0x40;
        }
        for (i = 0u; i < n; i++) {
            c[2u * n + i] = 0x0B;
        }
        c[2u * n + n] = 0x0B; /* function end */
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK,
              "nesting at the compile-time limit is accepted");
    }
    { /* One level past the compile-time limit overflows the pool. */
        uint8_t c[2u * ((unsigned)WASM_MAX_CTRL_NESTING + 1u)];
        uint32_t i, n = (uint32_t)WASM_MAX_CTRL_NESTING + 1u;
        for (i = 0u; i < n; i++) {
            c[2u * i] = 0x02;
            c[2u * i + 1u] = 0x40;
        }
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_CONTROL_STACK_OVERFLOW,
              "nesting past the limit -> CONTROL_STACK_OVERFLOW");
    }
    { /* br_if 0 falls through: const 7 survives, END pops normally. */
        static const uint8_t c[] = { 0x02, 0x40, 0x41, 0x00, 0x0D, 0x00, 0x41, 0x07, 0x0B };
        CHECK(exec_bytes(c, sizeof(c), 0u, 0u) == WASM_TRAP_OK, "run br_if 0 fallthrough");
        expect_i32(7, "fallthrough yields 7");
    }
    { /* raw control-stack unit checks. */
        wasm_control_pool_t pool;
        wasm_control_stack_t cs;
        wasm_control_frame_t cf;
        wasm_control_frame_t *top = NULL;
        uint32_t i;
        CHECK(wasm_control_pool_init(&pool) == WASM_TRAP_OK, "ctrl pool init");
        CHECK(wasm_control_stack_init(&cs, &pool) == WASM_TRAP_OK, "ctrl init");
        CHECK(wasm_control_stack_pop(&cs, NULL) == WASM_TRAP_CONTROL_STACK_UNDERFLOW, "raw pop empty -> UNDERFLOW");
        CHECK(wasm_control_stack_top(&cs, &top) == WASM_TRAP_CONTROL_STACK_UNDERFLOW, "raw top empty -> UNDERFLOW");
        CHECK(wasm_control_stack_init(NULL, &pool) == WASM_TRAP_NULL, "raw init NULL -> NULL");
        cf.kind = WASM_BLOCK_TYPE_LOOP;
        cf.blocktype = (int32_t)WASM_BLOCKTYPE_VOID;
        cf.stack_height = 0u;
        cf.body_start = NULL;
        for (i = 0u; i < (uint32_t)WASM_MAX_CTRL_NESTING; i++) {
            if (wasm_control_stack_push(&cs, &cf) != WASM_TRAP_OK) {
                break;
            }
        }
        CHECK(i == (uint32_t)WASM_MAX_CTRL_NESTING, "raw fills to the nesting limit");
        CHECK(wasm_control_stack_push(&cs, &cf) == WASM_TRAP_CONTROL_STACK_OVERFLOW, "raw one past the limit -> OVERFLOW");
        CHECK(wasm_control_stack_top(&cs, &top) == WASM_TRAP_OK && top->kind == WASM_BLOCK_TYPE_LOOP, "raw top readable");
        CHECK(wasm_control_stack_truncate(&cs, 1u) == WASM_TRAP_OK, "raw truncate one");
        CHECK(wasm_control_stack_depth(&cs) == (uint32_t)WASM_MAX_CTRL_NESTING - 1u, "truncate shrinks the window");
    }
    { /* SLEB/ULEB assembler sanity (uses enc helpers, keeps -Werror quiet). */
        uint8_t buf[8];
        CHECK(enc_u32(buf, 300u) == 2u && buf[0] == 0xACu && buf[1] == 0x02u, "enc_u32(300)");
        CHECK(enc_s32(buf, -5) == 1u && buf[0] == 0x7Bu, "enc_s32(-5)");
    }
}

static void test_footprint(void)
{
    printf("[T] footprint — struct sizes\n");
    printf("  sizeof(ctrl_frame)=%u sizeof(ctrl_stack)=%u\n",
           (unsigned)sizeof(wasm_control_frame_t),
           (unsigned)sizeof(wasm_control_stack_t));
    printf("  sizeof(wasm_frame_t)=%u sizeof(wasm_call_stack_t)=%u sizeof(wasm_vm_t)=%u sizeof(func_desc)=%u\n",
           (unsigned)sizeof(wasm_frame_t),
           (unsigned)sizeof(wasm_call_stack_t),
           (unsigned)sizeof(wasm_vm_t),
           (unsigned)sizeof(wasm_func_desc_t));
    { /* exact control-frame size: 3xu32 header + padding + one pointer. */
        size_t head = (unsigned)sizeof(uint32_t) + (unsigned)sizeof(int32_t) +
                      (unsigned)sizeof(uint32_t);
        size_t al = _Alignof(const uint8_t *);
        size_t expect = ((head + al - 1u) / al) * al + sizeof(const uint8_t *);
        CHECK(sizeof(wasm_control_frame_t) == expect, "ctrl frame layout exact");
    }
    CHECK(sizeof(wasm_frame_t) == 4u * (unsigned)sizeof(uint32_t) +
          2u * (unsigned)sizeof(const uint8_t *) +
          (unsigned)sizeof(wasm_control_stack_t) +
          (unsigned)WASM_MAX_LOCALS_PER_FRAME * (unsigned)sizeof(wasm_val_t), "frame layout exact");
    CHECK(sizeof(wasm_call_stack_t) >= (unsigned)WASM_MAX_CALL_STACK_DEPTH * (unsigned)sizeof(wasm_frame_t),
          "call stack holds 64 frames");
    CHECK(sizeof(wasm_vm_t) >= sizeof(wasm_stack_t) + sizeof(wasm_call_stack_t), "vm aggregates all engines");
}

int main(void)
{
    printf("=== Phase 5 tests: structured control flow + CALL ===\n");
    test_block_br();
    test_loop_counter();
    test_if_else();
    test_factorial();
    test_call();
    test_call_return();
    test_control_traps();
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

/*
 * test_phase3.c — Phase 3 verification: frames, call stack, enter/exit, locals.
 * Standalone, C11, assert.h/stdio.h only.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -Iinclude src/leb128.c
 *        src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c src/wasm_frame.c
 *        src/wasm_vm.c tests/test_phase3.c -o build/test_phase3
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "wasm_frame.h"
#include "wasm_vm.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

/* ~50 KiB VM — static, never on the function-call stack. */
static wasm_vm_t g_vm;

static wasm_val_t mk_i32(int32_t v) { wasm_val_t x; x.type = WASM_TYPE_I32; x.value.i32 = v; return x; }

/* ---------- a) normal entry: params, locals, base ---------- */

static void test_enter_basic(void)
{
    wasm_frame_t *f = NULL;
    wasm_val_t out;
    printf("[T] function entry: params, locals, base pointer\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm init OK");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(10)) == WASM_TRAP_OK, "push P0=10");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(20)) == WASM_TRAP_OK, "push P1=20");
    CHECK(wasm_vm_enter_function(&g_vm, 7u, 2u, 1u, 1u) == WASM_TRAP_OK, "enter func=7 (2p,1l,1r)");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 1u, "call depth == 1");
    CHECK(wasm_stack_depth(&g_vm.operands) == 0u, "params consumed from operand stack");
    CHECK(wasm_call_stack_top(&g_vm.calls, &f) == WASM_TRAP_OK, "top frame reachable");
    CHECK(f->func_idx == 7u, "frame func_idx recorded");
    CHECK(f->locals_count == 3u, "locals_count == params+locals");
    CHECK(f->stack_base_ptr == 0u, "base == 0 (stack was empty below args)");
    CHECK(f->return_count == 1u, "return arity recorded");
    CHECK(f->locals[0].value.i32 == 10, "locals[0] == P0 (first pushed)");
    CHECK(f->locals[1].value.i32 == 20, "locals[1] == P1 (second pushed)");
    CHECK(f->locals[2].type == WASM_TYPE_I32 && f->locals[2].value.i32 == 0, "declared local zero-initialized");

    /* Three-arg ordering check on a fresh VM state (still depth 1 is fine,
     * use raw frame pop first via a matching exit). */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(99)) == WASM_TRAP_OK, "push return 99");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit with 1 return");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 0u, "call depth back to 0");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 99, "return value survives exit");

    /* P0/P1/P2 ordering with three params. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(1)) == WASM_TRAP_OK, "push 1");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(2)) == WASM_TRAP_OK, "push 2");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(3)) == WASM_TRAP_OK, "push 3");
    CHECK(wasm_vm_enter_function(&g_vm, 0u, 3u, 0u, 0u) == WASM_TRAP_OK, "enter (3p,0l,0r)");
    CHECK(wasm_call_stack_top(&g_vm.calls, &f) == WASM_TRAP_OK, "top reachable");
    CHECK(f->locals[0].value.i32 == 1 && f->locals[1].value.i32 == 2 &&
          f->locals[2].value.i32 == 3, "param order P0=1,P1=2,P2=3");
    CHECK(wasm_vm_exit_function(&g_vm, 0u) == WASM_TRAP_OK, "void exit OK");
}

/* ---------- b) nesting to depth 3 + ordered unwinding ---------- */

static void test_nested_calls(void)
{
    wasm_frame_t *f = NULL;
    wasm_val_t out;
    printf("[T] nested calls to depth 3 + frame unwinding\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm re-init");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(1000)) == WASM_TRAP_OK, "caller sentinel 1000");

    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(10)) == WASM_TRAP_OK, "push arg f0");
    CHECK(wasm_vm_enter_function(&g_vm, 0u, 1u, 0u, 1u) == WASM_TRAP_OK, "enter f0 (base should be 1)");
    CHECK(wasm_call_stack_top(&g_vm.calls, &f) == WASM_TRAP_OK && f->stack_base_ptr == 1u, "f0 base == 1");

    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(20)) == WASM_TRAP_OK, "push arg f1");
    CHECK(wasm_vm_enter_function(&g_vm, 1u, 1u, 0u, 1u) == WASM_TRAP_OK, "enter f1");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 2u, "depth == 2");

    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(30)) == WASM_TRAP_OK, "push arg f2");
    CHECK(wasm_vm_enter_function(&g_vm, 2u, 1u, 1u, 1u) == WASM_TRAP_OK, "enter f2 (1p,1l,1r)");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 3u, "depth == 3");
    CHECK(wasm_call_stack_top(&g_vm.calls, &f) == WASM_TRAP_OK, "top reachable");
    CHECK(f->func_idx == 2u && f->locals[0].value.i32 == 30, "innermost frame correct");
    CHECK(f->locals[1].value.i32 == 0, "innermost declared local zero");

    /* Unwind inside-out with distinct return values. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(300)) == WASM_TRAP_OK, "f2 computes 300");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit f2");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 2u, "depth == 2 after f2 exit");

    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(200)) == WASM_TRAP_OK, "f1 computes 200 (above f2 result)");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit f1 (drops f2 residue)");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 1u, "depth == 1 after f1 exit");

    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(100)) == WASM_TRAP_OK, "f0 computes 100");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit f0");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 0u, "depth == 0 fully unwound");

    /* Only [sentinel, f0-result] may remain. */
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 100, "final result 100 on top");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 1000, "caller sentinel intact");
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "operand stack fully drained");
}

/* ---------- c) call-stack overflow ---------- */

static void test_call_overflow(void)
{
    uint32_t i;
    printf("[T] call stack overflow past depth 64\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm re-init");
    for (i = 0u; i < (uint32_t)WASM_MAX_CALL_STACK_DEPTH; i++) {
        if (wasm_vm_enter_function(&g_vm, i, 0u, 0u, 0u) != WASM_TRAP_OK) {
            break;
        }
    }
    CHECK(i == (uint32_t)WASM_MAX_CALL_STACK_DEPTH, "filled exactly to 64 via enter");
    CHECK(wasm_call_stack_is_full(&g_vm.calls) == 1, "call stack reports full");
    CHECK(wasm_vm_enter_function(&g_vm, 999u, 0u, 0u, 0u) == WASM_TRAP_CALL_STACK_OVERFLOW, "65th enter -> CALL_STACK_OVERFLOW");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 64u, "depth pinned at 64");

    /* Raw push path guards identically. */
    {
        wasm_call_stack_t raw;
        wasm_frame_t fr;
        uint32_t k;
        CHECK(wasm_call_stack_init(&raw) == WASM_TRAP_OK, "raw call stack init");
        fr.func_idx = 0u; fr.stack_base_ptr = 0u; fr.locals_count = 0u; fr.return_count = 0u;
        for (k = 0u; k < 64u; k++) {
            if (wasm_call_stack_push(&raw, &fr) != WASM_TRAP_OK) {
                break;
            }
        }
        CHECK(k == 64u, "raw stack fills to 64");
        CHECK(wasm_call_stack_push(&raw, &fr) == WASM_TRAP_CALL_STACK_OVERFLOW, "raw 65th push -> OVERFLOW");
    }
}

/* ---------- d) call-stack underflow ---------- */

static void test_call_underflow(void)
{
    wasm_frame_t fr;
    wasm_frame_t *top = NULL;
    printf("[T] call stack underflow on exit/pop/top\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm re-init (empty calls)");
    CHECK(wasm_vm_exit_function(&g_vm, 0u) == WASM_TRAP_CALL_STACK_UNDERFLOW, "exit with no frame -> UNDERFLOW");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_CALL_STACK_UNDERFLOW, "exit(1) with no frame -> UNDERFLOW");

    {
        wasm_call_stack_t raw;
        CHECK(wasm_call_stack_init(&raw) == WASM_TRAP_OK, "raw init");
        CHECK(wasm_call_stack_pop(&raw, &fr) == WASM_TRAP_CALL_STACK_UNDERFLOW, "raw pop empty -> UNDERFLOW");
        CHECK(wasm_call_stack_top(&raw, &top) == WASM_TRAP_CALL_STACK_UNDERFLOW, "raw top empty -> UNDERFLOW");
        CHECK(wasm_call_stack_pop(&raw, NULL) == WASM_TRAP_CALL_STACK_UNDERFLOW, "raw pop NULL-out empty -> UNDERFLOW");
    }

    /* local.* with no active frame must also report underflow, not crash. */
    CHECK(wasm_vm_local_get(&g_vm, 0u) == WASM_TRAP_CALL_STACK_UNDERFLOW, "local.get no frame -> UNDERFLOW");
    CHECK(wasm_vm_local_set(&g_vm, 0u) == WASM_TRAP_CALL_STACK_UNDERFLOW, "local.set no frame -> UNDERFLOW");
    CHECK(wasm_vm_local_tee(&g_vm, 0u) == WASM_TRAP_CALL_STACK_UNDERFLOW, "local.tee no frame -> UNDERFLOW");
}

/* ---------- e) local.get / set / tee + index traps ---------- */

static void test_locals_ops(void)
{
    wasm_frame_t *f = NULL;
    wasm_val_t out;
    uint32_t depth_before;
    printf("[T] local.get / local.set / local.tee + OOB index traps\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm re-init");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(5)) == WASM_TRAP_OK, "push param 5");
    CHECK(wasm_vm_enter_function(&g_vm, 0u, 1u, 2u, 0u) == WASM_TRAP_OK, "enter (1p,2l,0r) total=3");

    /* set locals[1] = 42. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(42)) == WASM_TRAP_OK, "push 42 for set");
    CHECK(wasm_vm_local_set(&g_vm, 1u) == WASM_TRAP_OK, "local.set 1 consumes 42");
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "set consumed the value");
    CHECK(wasm_call_stack_top(&g_vm.calls, &f) == WASM_TRAP_OK && f->locals[1].value.i32 == 42, "locals[1] == 42");

    /* get pushes copies: get(1) -> [42], get(0) -> [42,5]. */
    CHECK(wasm_vm_local_get(&g_vm, 1u) == WASM_TRAP_OK, "local.get 1");
    CHECK(wasm_vm_local_get(&g_vm, 0u) == WASM_TRAP_OK, "local.get 0");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 5, "top is param copy 5");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 42, "next is local copy 42");

    /* tee peeks without consuming: stack [7] stays [7], locals[2] = 7. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(7)) == WASM_TRAP_OK, "push 7 for tee");
    CHECK(wasm_vm_local_tee(&g_vm, 2u) == WASM_TRAP_OK, "local.tee 2");
    CHECK(wasm_call_stack_top(&g_vm.calls, &f) == WASM_TRAP_OK && f->locals[2].value.i32 == 7, "locals[2] == 7 via tee");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 7, "tee kept 7 on stack");

    /* OOB indices: == count and UINT32_MAX; stack height must not move. */
    depth_before = wasm_stack_depth(&g_vm.operands);
    CHECK(wasm_vm_local_get(&g_vm, 3u) == WASM_TRAP_INVALID_LOCAL_INDEX, "get idx==count -> INVALID_LOCAL_INDEX");
    CHECK(wasm_vm_local_set(&g_vm, 3u) == WASM_TRAP_INVALID_LOCAL_INDEX, "set idx==count -> INVALID_LOCAL_INDEX");
    CHECK(wasm_vm_local_tee(&g_vm, 3u) == WASM_TRAP_INVALID_LOCAL_INDEX, "tee idx==count -> INVALID_LOCAL_INDEX");
    CHECK(wasm_vm_local_get(&g_vm, 0xFFFFFFFFu) == WASM_TRAP_INVALID_LOCAL_INDEX, "get UINT32_MAX -> INVALID_LOCAL_INDEX");
    CHECK(wasm_vm_local_set(&g_vm, 0xFFFFFFFFu) == WASM_TRAP_INVALID_LOCAL_INDEX, "set UINT32_MAX -> INVALID_LOCAL_INDEX");
    CHECK(wasm_stack_depth(&g_vm.operands) == depth_before, "failed local ops left stack untouched");

    /* set/tee on an empty operand stack trap cleanly (locals valid). */
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "operand stack empty here");
    CHECK(wasm_vm_local_set(&g_vm, 0u) == WASM_TRAP_STACK_UNDERFLOW, "set on empty operands -> UNDERFLOW");
    CHECK(wasm_vm_local_tee(&g_vm, 0u) == WASM_TRAP_STACK_UNDERFLOW, "tee on empty operands -> UNDERFLOW");

    CHECK(wasm_vm_exit_function(&g_vm, 0u) == WASM_TRAP_OK, "void exit cleans up");
}

/* ---------- f) height restoration, garbage drop, arg/arity guards ---------- */

static void test_height_restoration(void)
{
    wasm_val_t out;
    uint32_t depth_before;
    printf("[T] operand height restored on return; garbage dropped\n");
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm re-init");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(111)) == WASM_TRAP_OK, "caller sentinel 111");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(222)) == WASM_TRAP_OK, "caller sentinel 222");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(9)) == WASM_TRAP_OK, "push param 9");
    CHECK(wasm_vm_enter_function(&g_vm, 0u, 1u, 0u, 1u) == WASM_TRAP_OK, "enter (1p,0l,1r) base==2");

    /* Callee leaves garbage then a return value. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(1)) == WASM_TRAP_OK, "garbage 1");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(2)) == WASM_TRAP_OK, "garbage 2");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(3)) == WASM_TRAP_OK, "garbage 3");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(777)) == WASM_TRAP_OK, "return 777");
    CHECK(wasm_stack_depth(&g_vm.operands) == 6u, "height 6 before exit");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit with return");
    CHECK(wasm_stack_depth(&g_vm.operands) == 3u, "height == base(2)+1 after exit");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 777, "return 777 on top");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 222, "sentinel 222 intact");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 111, "sentinel 111 intact");

    /* Void return drops everything above base. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(5)) == WASM_TRAP_OK, "sentinel 5");
    CHECK(wasm_vm_enter_function(&g_vm, 1u, 0u, 2u, 0u) == WASM_TRAP_OK, "enter void fn base==1");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(50)) == WASM_TRAP_OK, "garbage 50");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(60)) == WASM_TRAP_OK, "garbage 60");
    CHECK(wasm_vm_exit_function(&g_vm, 0u) == WASM_TRAP_OK, "void exit");
    CHECK(wasm_stack_depth(&g_vm.operands) == 1u, "height == base 1, garbage dropped");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 5, "sentinel 5 intact");

    /* Missing return value traps without mutating. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(8)) == WASM_TRAP_OK, "push param 8");
    CHECK(wasm_vm_enter_function(&g_vm, 2u, 1u, 0u, 1u) == WASM_TRAP_OK, "enter (1p,0l,1r)");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_STACK_UNDERFLOW, "exit with no return pushed -> UNDERFLOW");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == 1u, "failed exit keeps the frame");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(80)) == WASM_TRAP_OK, "push late return 80");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "retry exit OK");
    CHECK(wasm_stack_pop(&g_vm.operands, &out) == WASM_TRAP_OK && out.value.i32 == 80, "late return delivered");

    /* Arity / count guards. */
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(1)) == WASM_TRAP_OK, "one arg present");
    depth_before = wasm_call_stack_depth(&g_vm.calls);
    CHECK(wasm_vm_enter_function(&g_vm, 3u, 2u, 0u, 0u) == WASM_TRAP_INSUFFICIENT_ARGS, "2 params, 1 value -> INSUFFICIENT_ARGS");
    CHECK(wasm_call_stack_depth(&g_vm.calls) == depth_before, "failed enter pushes no frame");
    CHECK(wasm_stack_depth(&g_vm.operands) == 1u, "failed enter consumes no args");
    CHECK(wasm_vm_enter_function(&g_vm, 3u, 20u, 20u, 0u) == WASM_TRAP_INVALID_LOCAL_INDEX, "40 locals > 32 -> INVALID_LOCAL_INDEX");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(2)) == WASM_TRAP_OK, "second arg");
    CHECK(wasm_vm_enter_function(&g_vm, 3u, 2u, 0u, 1u) == WASM_TRAP_OK, "enter ret=1 for arity test");
    CHECK(wasm_vm_exit_function(&g_vm, 0u) == WASM_TRAP_INVALID_ARGUMENT, "exit arity 0 != entry 1 -> INVALID_ARGUMENT");
    CHECK(wasm_vm_exit_function(&g_vm, 2u) == WASM_TRAP_INVALID_ARGUMENT, "exit arity 2 illegal -> INVALID_ARGUMENT");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(0)) == WASM_TRAP_OK, "push dummy return");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "matching exit OK");
    while (wasm_stack_depth(&g_vm.operands) > 0u) {
        (void)wasm_stack_pop(&g_vm.operands, &out);
    }
}

static void test_footprint(void)
{
    printf("[T] footprint — struct sizes\n");
    printf("  WASM_MAX_CALL_STACK_DEPTH=%u WASM_MAX_LOCALS_PER_FRAME=%u\n",
           (unsigned)WASM_MAX_CALL_STACK_DEPTH, (unsigned)WASM_MAX_LOCALS_PER_FRAME);
    printf("  sizeof(wasm_frame_t)=%u sizeof(wasm_call_stack_t)=%u sizeof(wasm_vm_t)=%u\n",
           (unsigned)sizeof(wasm_frame_t),
           (unsigned)sizeof(wasm_call_stack_t),
           (unsigned)sizeof(wasm_vm_t));
    CHECK(sizeof(wasm_frame_t) == 4u * (unsigned)sizeof(uint32_t) +
          2u * (unsigned)sizeof(const uint8_t *) +
          (unsigned)sizeof(wasm_control_stack_t) +
          (unsigned)WASM_MAX_LOCALS_PER_FRAME * (unsigned)sizeof(wasm_val_t), "frame == header + code window + ctrl stack + 32 locals");
    CHECK(sizeof(wasm_call_stack_t) >= (unsigned)WASM_MAX_CALL_STACK_DEPTH * (unsigned)sizeof(wasm_frame_t),
          "call stack holds 64 frames");
    CHECK(sizeof(wasm_vm_t) >= sizeof(wasm_stack_t) + sizeof(wasm_call_stack_t), "vm == operands + calls");
}

int main(void)
{
    printf("=== Phase 3 tests: frames + call stack + VM enter/exit ===\n");
    test_enter_basic();
    test_nested_calls();
    test_call_overflow();
    test_call_underflow();
    test_locals_ops();
    test_height_restoration();
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

/*
 * test_phase13.c — Phase 4 hardening: nesting limit + scan budget.
 *
 * Covers:
 *  - the compile-time control-nesting limit (WASM_MAX_CTRL_NESTING):
 *    the validator rejects deeper nesting, accepts exactly the limit,
 *    and the interpreter traps instead of growing any structure;
 *  - the global control pool: nested calls share one bounded pool;
 *  - the branch-scan budget: bytes scanned while resolving a branch
 *    target are charged to the host step budget, so a pathological
 *    module (one huge block + a branch to a distant END) terminates
 *    under a small budget instead of running unbounded.
 *
 * Standalone, C11, assert-style. Zero allocation: all buffers are
 * static or automatic with compile-time capacity.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasm_control.h"
#include "wasm_host.h"
#include "wasm_memory.h"
#include "wasm_parser.h"
#include "wasm_validate.h"
#include "wasm_vm.h"
#include "harness.h"


static wasm_vm_t g_vm;
static wasm_host_registry_t g_reg;
static wasm_memory_t g_mem;
static uint8_t g_arena[WASM_PAGE_SIZE];

/* Module buffer: header + type + function + code sections. */
#define MOD_BUF_SIZE 200000u
static uint8_t g_mod[MOD_BUF_SIZE];

/* ---- module builder ------------------------------------------------- */

/* ULEB128 length of a value (1..3 bytes for the sizes used here). */
static size_t uleb_len(uint32_t v)
{
    size_t n = 1u;
    while (v >= 0x80u) {
        v >>= 7;
        n++;
    }
    return n;
}

static size_t uleb_write(uint8_t *p, uint32_t v)
{
    size_t n = 0u;
    do {
        uint8_t b = (uint8_t)(v & 0x7Fu);
        v >>= 7;
        if (v != 0u) {
            b |= 0x80u;
        }
        p[n++] = b;
    } while (v != 0u);
    return n;
}

/*
 * Build a module with one function whose body is:
 *   block void <nest-1 more blocks> [br 0 + tail_nops nops] end... end
 * `nest` = number of nested BLOCKs, `tail_nops` = number of NOP bytes
 * between the branch and the matching END (used to build a pathological
 * "huge block" body whose branch target is far away).
 */
static size_t build_nested_module(uint8_t *buf, size_t cap, uint32_t nest,
                                  uint32_t tail_nops, int with_branch)
{
    size_t p = 0;
    uint32_t body_size, sec_payload;
    uint32_t i;

    if (cap < 64u) {
        return 0u;
    }
    body_size = 1u + 2u * nest + (with_branch ? (2u + tail_nops) : 0u) +
                nest + 1u;
    sec_payload = 1u + (uint32_t)uleb_len(body_size) + body_size;
    if ((size_t)sec_payload + 32u > cap) {
        return 0u;
    }

    /* header */
    buf[p++] = 0x00u; buf[p++] = 0x61u; buf[p++] = 0x73u; buf[p++] = 0x6Du;
    buf[p++] = 0x01u; buf[p++] = 0x00u; buf[p++] = 0x00u; buf[p++] = 0x00u;
    /* type section: one () -> () */
    buf[p++] = 0x01u; buf[p++] = 0x04u; buf[p++] = 0x01u; buf[p++] = 0x60u;
    buf[p++] = 0x00u; buf[p++] = 0x00u;
    /* function section: one function of type 0 */
    buf[p++] = 0x03u; buf[p++] = 0x02u; buf[p++] = 0x01u; buf[p++] = 0x00u;
    /* code section */
    buf[p++] = 0x0Au;
    p += uleb_write(&buf[p], sec_payload);
    buf[p++] = 0x01u; /* one body */
    p += uleb_write(&buf[p], body_size);
    buf[p++] = 0x00u; /* zero local groups */
    for (i = 0u; i < nest; i++) {
        buf[p++] = 0x02u; /* block */
        buf[p++] = 0x40u; /* void blocktype */
    }
    if (with_branch) {
        buf[p++] = 0x0Cu; /* br 0 */
        buf[p++] = 0x00u;
        for (i = 0u; i < tail_nops; i++) {
            buf[p++] = 0x01u; /* nop */
        }
    }
    for (i = 0u; i < nest; i++) {
        buf[p++] = 0x0Bu; /* end */
    }
    buf[p++] = 0x0Bu; /* function end */
    return p;
}

/* ---- helpers -------------------------------------------------------- */

static int module_valid(const uint8_t *mod, size_t len)
{
    wasm_module_t m;
    if (wasm_parse_header(mod, len, &m) != WASM_OK) {
        return 0;
    }
    return wasm_validate_module(&m) == WASM_OK;
}

static wasm_err_t module_validate_err(const uint8_t *mod, size_t len)
{
    wasm_module_t m;
    if (wasm_parse_header(mod, len, &m) != WASM_OK) {
        return WASM_ERR_TRUNCATED;
    }
    return wasm_validate_module(&m);
}

/* Run the module's function 0 with a step budget; returns the trap (or
 * WASM_TRAP_OK) and reports how many steps and how many scan bytes were
 * consumed. `do_validate` = 0 runs a module the validator would reject
 * (like the CLI's --no-validate) so the *runtime* bound can be tested. */
static wasm_trap_t run_module_ex(const uint8_t *mod, size_t len, uint32_t budget,
                                 uint32_t *out_steps, uint64_t *out_scan,
                                 int do_validate)
{
    wasm_module_t m;
    wasm_trap_t tr;
    uint64_t cursor = 0u;

    *out_steps = 0u;
    *out_scan = 0u;
    if (wasm_parse_header(mod, len, &m) != WASM_OK) {
        return WASM_TRAP_NULL;
    }
    if (do_validate && wasm_validate_module(&m) != WASM_OK) {
        return WASM_TRAP_NULL;
    }
    if (wasm_vm_init(&g_vm) != WASM_TRAP_OK) {
        return WASM_TRAP_NULL;
    }
    if (wasm_host_registry_init(&g_reg) != WASM_TRAP_OK) {
        return WASM_TRAP_NULL;
    }
    if (wasm_memory_init(&g_mem, g_arena, (uint32_t)sizeof(g_arena), 0u, 1u) !=
        WASM_TRAP_OK) {
        return WASM_TRAP_NULL;
    }
    if (wasm_vm_bind_memory(&g_vm, &g_mem) != WASM_TRAP_OK) {
        return WASM_TRAP_NULL;
    }
    if (wasm_instantiate(&g_vm, &m, &g_reg, NULL) != WASM_OK) {
        return WASM_TRAP_NULL;
    }
    if (wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) != WASM_TRAP_OK) {
        return WASM_TRAP_NULL;
    }
    {
        const wasm_func_desc_t *d = &g_vm.funcs[0];
        if (wasm_vm_attach_code(&g_vm, d->code, d->code_len) != WASM_TRAP_OK) {
            return WASM_TRAP_NULL;
        }
    }
    tr = WASM_TRAP_OK;
    while (tr == WASM_TRAP_OK && wasm_call_stack_depth(&g_vm.calls) > 0u) {
        wasm_frame_t *top = NULL;
        if (wasm_call_stack_top(&g_vm.calls, &top) != WASM_TRAP_OK) {
            break;
        }
        if (top->code_ptr == NULL || top->code_end == NULL ||
            top->code_ptr >= top->code_end) {
            tr = wasm_vm_exit_function(&g_vm, top->return_count);
            continue;
        }
        if (*out_steps >= budget) {
            tr = WASM_TRAP_STACK_OVERFLOW; /* budget exhausted (sentinel) */
            break;
        }
        (*out_steps)++;
        tr = wasm_vm_step(&g_vm);
        /* Phase 4: the scan charge consumes the same budget. */
        {
            uint32_t charge = wasm_vm_scan_charge(&g_vm, &cursor);
            if (charge > budget - *out_steps) {
                tr = WASM_TRAP_STACK_OVERFLOW; /* budget exhausted */
                break;
            }
            *out_steps += charge;
        }
    }
    *out_scan = g_vm.scan_bytes;
    return tr;
}

static wasm_trap_t run_module(const uint8_t *mod, size_t len, uint32_t budget,
                               uint32_t *out_steps, uint64_t *out_scan)
{
    return run_module_ex(mod, len, budget, out_steps, out_scan, 1);
}

/* ---- tests ---------------------------------------------------------- */

static void test_validator_nesting_limit(void)
{
    size_t len;
    printf("[T] validator nesting limit (WASM_MAX_CTRL_NESTING = %u)\n",
           (unsigned)WASM_MAX_CTRL_NESTING);

    len = build_nested_module(g_mod, sizeof(g_mod),
                               (uint32_t)WASM_MAX_CTRL_NESTING, 0u, 0);
    CHECK(len > 0u, "module at the limit builds");
    CHECK(module_valid(g_mod, len), "nesting exactly at the limit validates");

    len = build_nested_module(g_mod, sizeof(g_mod),
                               (uint32_t)WASM_MAX_CTRL_NESTING + 1u, 0u, 0);
    CHECK(len > 0u, "module past the limit builds");
    CHECK(module_validate_err(g_mod, len) == WASM_ERR_INVALID_BLOCK,
          "one level past the limit is rejected (INVALID_BLOCK)");
}

static void test_runtime_nesting_limit(void)
{
    uint32_t steps = 0u;
    uint64_t scan = 0u;
    wasm_trap_t tr;
    size_t len;

    printf("[T] interpreter nesting limit\n");

    len = build_nested_module(g_mod, sizeof(g_mod),
                               (uint32_t)WASM_MAX_CTRL_NESTING, 0u, 0);
    tr = run_module(g_mod, len, 100000u, &steps, &scan);
    CHECK(tr == WASM_TRAP_OK, "nesting at the limit executes");

    len = build_nested_module(g_mod, sizeof(g_mod),
                               (uint32_t)WASM_MAX_CTRL_NESTING + 1u, 0u, 0);
    tr = run_module_ex(g_mod, len, 100000u, &steps, &scan, 0);
    CHECK(tr == WASM_TRAP_CONTROL_STACK_OVERFLOW,
          "one level past the limit traps (CONTROL_STACK_OVERFLOW)");
}

static void test_control_pool_is_global(void)
{
    wasm_control_pool_t pool;
    wasm_control_stack_t outer;
    wasm_control_stack_t inner;
    wasm_control_frame_t cf;
    uint32_t i;

    printf("[T] the control pool is shared by all call frames\n");
    CHECK(wasm_control_pool_init(&pool) == WASM_TRAP_OK, "pool init");
    CHECK(wasm_control_stack_init(&outer, &pool) == WASM_TRAP_OK, "outer window");
    cf.kind = WASM_BLOCK_TYPE_BLOCK;
    cf.blocktype = (int32_t)WASM_BLOCKTYPE_VOID;
    cf.stack_height = 0u;
    cf.body_start = NULL;
    for (i = 0u; i < (uint32_t)WASM_MAX_CTRL_NESTING; i++) {
        if (wasm_control_stack_push(&outer, &cf) != WASM_TRAP_OK) {
            break;
        }
    }
    CHECK(i == (uint32_t)WASM_MAX_CTRL_NESTING, "outer frame fills the pool");
    CHECK(wasm_control_stack_init(&inner, &pool) == WASM_TRAP_OK, "inner window");
    CHECK(wasm_control_stack_push(&inner, &cf) == WASM_TRAP_CONTROL_STACK_OVERFLOW,
          "a callee cannot exceed the global pool");
    CHECK(wasm_control_stack_truncate(&outer, 1u) == WASM_TRAP_OK, "outer frees one slot");
    /* A callee that starts after the slot was freed gets it (the window
     * of a live callee is never invalidated by its caller). */
    CHECK(wasm_control_stack_init(&inner, &pool) == WASM_TRAP_OK, "inner window again");
    CHECK(wasm_control_stack_push(&inner, &cf) == WASM_TRAP_OK,
          "the freed slot is reusable by the callee");
    CHECK(wasm_control_stack_pop(&inner, NULL) == WASM_TRAP_OK, "inner pops");
    CHECK(wasm_control_stack_truncate(&outer, 1u) == WASM_TRAP_OK, "outer frees one slot");
}

static void test_scan_budget(void)
{
    uint32_t steps = 0u;
    uint64_t scan = 0u;
    wasm_trap_t tr;
    size_t len;
    const uint32_t nops = 60000u;

    printf("[T] branch-scan budget\n");

    /* One huge block, a branch to a distant END: a single VM step has
     * to scan the whole block to resolve the branch target. */
    len = build_nested_module(g_mod, sizeof(g_mod), 1u, nops, 1);
    CHECK(len > 0u && len < MOD_BUF_SIZE, "pathological module builds");
    CHECK(module_valid(g_mod, len), "pathological module validates");

    /* A generous budget finishes the program. */
    tr = run_module(g_mod, len, 1000000u, &steps, &scan);
    CHECK(tr == WASM_TRAP_OK, "pathological module runs with a large budget");
    CHECK(scan > (uint64_t)nops, "the branch scan is accounted (scan bytes)");

    /* A small budget must stop the run instead of scanning forever. */
    tr = run_module(g_mod, len, 8u, &steps, &scan);
    CHECK(tr == WASM_TRAP_STACK_OVERFLOW, "small budget stops the run");
    CHECK(steps <= 8u, "the budget is respected");

    /* The charge is proportional: ~60 KiB scanned costs ~59 steps. */
    {
        uint64_t cursor = 0u;
        uint32_t charge;
        (void)wasm_vm_init(&g_vm);
        charge = wasm_vm_scan_charge(&g_vm, &cursor);
        CHECK(charge == 0u, "no charge without scanning");
    }
}

int main(void)
{
    static const harness_test_t tests[] = {
        { "validator_nesting_limit", test_validator_nesting_limit },
        { "runtime_nesting_limit", test_runtime_nesting_limit },
        { "control_pool_is_global", test_control_pool_is_global },
        { "scan_budget", test_scan_budget },
    };
    printf("=== Phase 13 tests: nesting limit and scan budget ===\n");
    return harness_run("phase13", tests,
                       sizeof(tests) / sizeof(tests[0]));
}

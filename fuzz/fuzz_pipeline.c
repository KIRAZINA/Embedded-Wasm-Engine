/*
 * fuzz_pipeline.c — the shared zero-allocation fuzzing pipeline.
 *
 * C11, -Wall -Wextra -Wpedantic -Werror clean. No malloc: every buffer
 * below is a file-scope static array with a compile-time capacity.
 *
 * The pipeline is deliberately *total*: every stage is fed whatever the
 * previous stage accepted, including structurally broken modules, so the
 * fuzzers reach the parser, the validator, the linker and the VM with
 * hostile bytes.
 */

#include "fuzz_pipeline.h"

#include <string.h>

#include "wasm_host.h"
#include "wasm_memory.h"
#include "wasm_parser.h"
#include "wasm_validate.h"
#include "wasm_vm.h"

/* ---- static storage (no dynamic allocation anywhere) ---- */

static uint8_t g_bytes[FUZZ_MAX_MODULE_BYTES];
static uint8_t g_arena[FUZZ_MEM_PAGES * WASM_PAGE_SIZE];
static wasm_vm_t g_vm;
static wasm_host_registry_t g_reg;
static wasm_memory_t g_mem;

/* ---- stage 2: walk every section header ---- */

/* Returns the number of sections walked, or -1 when the walk fails
 * (the parser already reported the structural error). */
static int fuzz_walk_sections(const wasm_module_t *mod)
{
    const uint8_t *cursor = mod->sections_begin;
    const uint8_t *end = mod->sections_end;
    int n = 0;

    if (cursor == NULL || end == NULL) {
        return 0;
    }
    while (cursor < end) {
        wasm_section_header_t sec;
        if (wasm_get_section(&cursor, end, &sec) != WASM_OK) {
            return -1;
        }
        if (sec.payload.len != (size_t)sec.size) {
            return -1; /* defensive: header/payload disagreement */
        }
        n++;
        if (n > 4096) {
            return -1; /* absurd section count: stop walking */
        }
    }
    return n;
}

/* ---- stage 5: bounded execution ---- */

/* Run one defined, zero-parameter function for at most `budget` steps.
 * Any trap simply ends the run: traps are expected, crashes are not.
 * Bytes scanned while resolving branch targets are charged to the same
 * budget (Phase 4 hardening), so a pathological module cannot make one
 * step arbitrarily expensive. */
static void fuzz_run_function(uint32_t idx, uint32_t budget)
{
    const wasm_func_desc_t *d;
    wasm_frame_t *top = NULL;
    uint64_t scan_cursor;

    if (idx >= (uint32_t)WASM_MAX_FUNCTIONS) {
        return;
    }
    d = &g_vm.funcs[idx];
    if (!d->valid || d->is_host || d->param_count != 0u) {
        return;
    }
    /* The scan counter is cumulative per VM: start from the current
     * value so earlier functions are not charged twice. */
    scan_cursor = g_vm.scan_bytes;
    /* Fresh operand stack per function: leftovers from a previous
     * (trapped) run must never influence the next one. */
    (void)wasm_stack_init(&g_vm.operands);
    if (wasm_vm_enter_function(&g_vm, idx, 0u, d->local_count,
                               d->return_count) != WASM_TRAP_OK) {
        return;
    }
    if (wasm_vm_attach_code(&g_vm, d->code, d->code_len) != WASM_TRAP_OK) {
        return;
    }
    while (budget-- > 0u) {
        uint32_t charge;
        if (wasm_call_stack_top(&g_vm.calls, &top) != WASM_TRAP_OK) {
            return;
        }
        if (top->code_ptr == NULL || top->code_end == NULL ||
            top->code_ptr >= top->code_end) {
            /* Function body consumed: leave the frame. */
            (void)wasm_vm_exit_function(&g_vm, top->return_count);
            if (wasm_call_stack_depth(&g_vm.calls) == 0u) {
                return;
            }
            continue;
        }
        if (wasm_vm_step(&g_vm) != WASM_TRAP_OK) {
            return; /* trap: expected outcome, not a finding */
        }
        charge = wasm_vm_scan_charge(&g_vm, &scan_cursor);
        if (charge > budget) {
            return; /* budget consumed by the scan */
        }
        budget -= charge;
    }
}

/* Execute every defined zero-parameter function (bounded count) with a
 * small step budget each. */
static void fuzz_execute(void)
{
    uint32_t i, ran = 0u;

    for (i = 0u; i < (uint32_t)WASM_MAX_FUNCTIONS; i++) {
        if (!g_vm.funcs[i].valid || g_vm.funcs[i].is_host) {
            continue;
        }
        if (g_vm.funcs[i].param_count != 0u) {
            continue;
        }
        fuzz_run_function(i, FUZZ_STEP_BUDGET);
        ran++;
        if (ran >= 8u) {
            break; /* keep the per-input cost bounded */
        }
    }
}

/* ---- the pipeline ---- */

int wasm_fuzz_pipeline(const uint8_t *data, size_t len)
{
    wasm_module_t mod;
    wasm_err_t ec;
    int stages = 0;

    if (data == NULL) {
        return 0;
    }
    if (len > (size_t)FUZZ_MAX_MODULE_BYTES) {
        len = (size_t)FUZZ_MAX_MODULE_BYTES;
    }
    memcpy(g_bytes, data, len);

    /* 1. header */
    ec = wasm_parse_header(g_bytes, len, &mod);
    if (ec != WASM_OK) {
        return stages;
    }
    stages++;

    /* 2. section walk */
    if (fuzz_walk_sections(&mod) < 0) {
        return stages;
    }
    stages++;

    /* 3. static validation */
    if (wasm_validate_module(&mod) != WASM_OK) {
        return stages;
    }
    stages++;

    /* 4. instantiation */
    (void)wasm_vm_init(&g_vm);
    (void)wasm_host_registry_init(&g_reg);
    /* One visible page (initial == max): the memory must really have the
     * page, otherwise every module with a data segment is rejected with
     * DATA_OUT_OF_BOUNDS and the data path is never executed. */
    if (wasm_memory_init(&g_mem, g_arena, (uint32_t)sizeof(g_arena),
                         FUZZ_MEM_PAGES, FUZZ_MEM_PAGES) != WASM_TRAP_OK) {
        return stages;
    }
    if (wasm_vm_bind_memory(&g_vm, &g_mem) != WASM_TRAP_OK) {
        return stages;
    }
    if (wasm_instantiate(&g_vm, &mod, &g_reg, NULL) != WASM_OK) {
        return stages;
    }
    stages++;

    /* 5. bounded execution */
    fuzz_execute();
    stages++;

    return stages;
}

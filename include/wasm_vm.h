#ifndef WASM_VM_H
#define WASM_VM_H

/*
 * wasm_vm.h — Phase 3-6 VM core: frames, locals, ALU, control flow, host calls.
 *
 * Composition: wasm_vm_t = operand stack (Phase 2) + call stack (frames,
 *   each with its own control stack) + borrowed linear memory + a
 *   host-registered function table for OP_CALL interop.
 * Everything owned is pre-allocated inside the struct (~103 KiB: declare
 * `static`). Code bytes and memory backing are always borrowed.
 *
 * Call protocol for OP_CALL: the host registers each callable function once
 * with wasm_vm_define_function (signature + borrowed code window); CALL then
 * behaves like enter_function + attach_code. When the callee's code is
 * consumed, wasm_vm_run returns OK with the callee frame still on top; the
 * host resumes with wasm_vm_exit_function (collecting the return value) and
 * calls wasm_vm_run again to continue the caller. OP_RETURN exits inline.
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_control.h"
#include "wasm_frame.h"
#include "wasm_memory.h"
#include "wasm_stack.h"
#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callee descriptor for OP_CALL (borrowed code or host callback).
 * WASM-code entries use param/local/return counts + code window.
 * Host entries (is_host != 0) invoke host_cb with param_count args and
 * expect return_count results; code/code_len are ignored. */
typedef struct {
    uint32_t param_count;
    uint32_t local_count;
    uint32_t return_count;
    const uint8_t *code;
    size_t code_len;
    int valid;
    int is_host;
    wasm_host_func_t host_cb;
} wasm_func_desc_t;

typedef struct wasm_vm {
    wasm_stack_t operands;
    wasm_call_stack_t calls;
    wasm_memory_t *mem; /* borrowed linear memory (NULL = unbound) */
    wasm_func_desc_t funcs[WASM_MAX_FUNCTIONS];
} wasm_vm_t;

wasm_trap_t wasm_vm_init(wasm_vm_t *vm);

wasm_trap_t wasm_vm_enter_function(wasm_vm_t *vm, uint32_t func_idx,
                                   uint32_t param_count, uint32_t local_count,
                                   uint32_t return_count);

wasm_trap_t wasm_vm_exit_function(wasm_vm_t *vm, uint32_t return_count);

/* Variable opcodes against the TOP frame's locals. */
wasm_trap_t wasm_vm_local_get(wasm_vm_t *vm, uint32_t local_idx);
wasm_trap_t wasm_vm_local_set(wasm_vm_t *vm, uint32_t local_idx);
wasm_trap_t wasm_vm_local_tee(wasm_vm_t *vm, uint32_t local_idx);

/* Phase 4: memory binding + code window + execution. */
wasm_trap_t wasm_vm_bind_memory(wasm_vm_t *vm, wasm_memory_t *mem);
wasm_trap_t wasm_vm_attach_code(wasm_vm_t *vm, const uint8_t *code, size_t len);
wasm_trap_t wasm_vm_step(wasm_vm_t *vm);
wasm_trap_t wasm_vm_run(wasm_vm_t *vm);

/* Phase 5: host function registration for OP_CALL. */
wasm_trap_t wasm_vm_define_function(wasm_vm_t *vm, uint32_t func_idx,
                                    uint32_t param_count, uint32_t local_count,
                                    uint32_t return_count,
                                    const uint8_t *code, size_t code_len);

/* Phase 6: native-callback registration. OP_CALL on such an index pops
 * param_count values, invokes cb, and pushes return_count results. */
wasm_trap_t wasm_vm_define_host_function(wasm_vm_t *vm, uint32_t func_idx,
                                         uint32_t param_count,
                                         uint32_t return_count,
                                         wasm_host_func_t cb);

#ifdef __cplusplus
}
#endif

#endif /* WASM_VM_H */

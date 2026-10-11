#ifndef WASM_FRAME_H
#define WASM_FRAME_H

/*
 * wasm_frame.h — Phase 3 call frames + fixed-depth call stack.
 *
 * - Zero allocation: every frame and its locals array live inside
 *   wasm_call_stack_t (statically allocatable; ~33 KiB — declare `static`).
 * - Guards: push on full  -> WASM_TRAP_CALL_STACK_OVERFLOW
 *           pop/top on empty -> WASM_TRAP_CALL_STACK_UNDERFLOW
 * - C11 strict. Frames are copied by value (plain struct assignment).
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_control.h"
#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t func_idx;       /* callee index (informational) */
    uint32_t stack_base_ptr; /* operand-stack height at entry (isolation mark) */
    uint32_t locals_count;   /* param_count + local_count (<= MAX) */
    uint32_t return_count;   /* 0 or 1 under MVP */
    const uint8_t *code_ptr; /* next byte to execute (borrowed, nullable) */
    const uint8_t *code_end; /* one past the last code byte (borrowed) */
    wasm_control_stack_t ctrl; /* open block/loop/if constructs (Phase 5) */
    wasm_val_t locals[WASM_MAX_LOCALS_PER_FRAME];
} wasm_frame_t;

typedef struct {
    wasm_frame_t frames[WASM_MAX_CALL_STACK_DEPTH];
    uint32_t depth; /* live frames; next push writes frames[depth] */
} wasm_call_stack_t;

wasm_trap_t wasm_call_stack_init(wasm_call_stack_t *cs);
wasm_trap_t wasm_call_stack_push(wasm_call_stack_t *cs, const wasm_frame_t *frame);
wasm_trap_t wasm_call_stack_pop(wasm_call_stack_t *cs, wasm_frame_t *out);
wasm_trap_t wasm_call_stack_top(wasm_call_stack_t *cs, wasm_frame_t **out);

/* Introspection helpers (never trap; NULL-safe). */
uint32_t wasm_call_stack_depth(const wasm_call_stack_t *cs);
int wasm_call_stack_is_empty(const wasm_call_stack_t *cs);
int wasm_call_stack_is_full(const wasm_call_stack_t *cs);

#ifdef __cplusplus
}
#endif

#endif /* WASM_FRAME_H */

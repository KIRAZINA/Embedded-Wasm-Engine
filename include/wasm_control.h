#ifndef WASM_CONTROL_H
#define WASM_CONTROL_H

/*
 * wasm_control.h — Phase 5 structured-control block stack.
 *
 * Every live control frame of a VM lives in ONE fixed-capacity pool
 * (wasm_control_pool_t) that is embedded in wasm_vm_t. Each call frame
 * owns a contiguous window of that pool, described by a lightweight
 * view (wasm_control_stack_t: pool pointer, base offset, depth).
 *
 * Consequences:
 *
 * - Zero allocation: the pool is static storage inside wasm_vm_t.
 * - The nesting bound is global: the number of live control frames
 *   across the WHOLE call stack never exceeds WASM_MAX_CTRL_NESTING
 *   (compile-time constant, see wasm_types.h). The validator rejects
 *   deeper nesting up front; the interpreter traps instead of growing.
 * - push on full  -> WASM_TRAP_CONTROL_STACK_OVERFLOW
 *   pop/top empty -> WASM_TRAP_CONTROL_STACK_UNDERFLOW
 *
 * The pool is written only through the window API below, so the
 * "pool top == window base + window depth" invariant holds at all
 * times; a violated invariant is reported as WASM_TRAP_INVALID_ARGUMENT
 * instead of corrupting memory.
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    wasm_block_kind_t kind;   /* BLOCK / LOOP / IF */
    int32_t blocktype;        /* WASM_BLOCKTYPE_VOID (-64) or valtype (e.g. 0x7F) */
    uint32_t stack_height;    /* operand-stack height at block entry */
    const uint8_t *body_start;/* LOOP branch target (first body instr); else NULL */
} wasm_control_frame_t;

/* The shared pool: every live control frame of every live call frame. */
typedef struct {
    wasm_control_frame_t items[WASM_MAX_CTRL_NESTING];
    uint32_t depth; /* live control frames in the pool */
} wasm_control_pool_t;

/* One call frame's window into the pool. */
typedef struct {
    wasm_control_pool_t *pool; /* borrowed shared pool (never owned) */
    uint32_t base;   /* first pool slot owned by this frame */
    uint32_t depth;  /* control frames owned by this frame */
} wasm_control_stack_t;

wasm_trap_t wasm_control_pool_init(wasm_control_pool_t *pool);

/* Open a fresh window at the current pool top (call-frame entry). */
wasm_trap_t wasm_control_stack_init(wasm_control_stack_t *cs,
                                    wasm_control_pool_t *pool);
wasm_trap_t wasm_control_stack_push(wasm_control_stack_t *cs,
                                    const wasm_control_frame_t *frame);
wasm_trap_t wasm_control_stack_pop(wasm_control_stack_t *cs,
                                   wasm_control_frame_t *out);
wasm_trap_t wasm_control_stack_top(wasm_control_stack_t *cs,
                                   wasm_control_frame_t **out);
/* Drop `count` frames from the top of this window (branch unwinding). */
wasm_trap_t wasm_control_stack_truncate(wasm_control_stack_t *cs,
                                        uint32_t count);

/* Introspection helpers (never trap; NULL-safe). */
uint32_t wasm_control_stack_depth(const wasm_control_stack_t *cs);
int wasm_control_stack_is_empty(const wasm_control_stack_t *cs);
int wasm_control_stack_is_full(const wasm_control_stack_t *cs);

#ifdef __cplusplus
}
#endif

#endif /* WASM_CONTROL_H */

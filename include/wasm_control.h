#ifndef WASM_CONTROL_H
#define WASM_CONTROL_H

/*
 * wasm_control.h — Phase 5 structured-control block stack.
 *
 * Each call frame owns one control stack tracking its open
 * block/loop/if constructs. Entries are plain values (no pointers into
 * mutable state except the borrowed LOOP body-start address).
 *
 * - Zero allocation: storage embedded (32 entries x 24 B + depth).
 * - push on full  -> WASM_TRAP_CONTROL_STACK_OVERFLOW
 *   pop/top empty -> WASM_TRAP_CONTROL_STACK_UNDERFLOW
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

typedef struct {
    wasm_control_frame_t items[WASM_MAX_CONTROL_STACK_DEPTH];
    uint32_t depth;
} wasm_control_stack_t;

wasm_trap_t wasm_control_stack_init(wasm_control_stack_t *cs);
wasm_trap_t wasm_control_stack_push(wasm_control_stack_t *cs,
                                    const wasm_control_frame_t *frame);
wasm_trap_t wasm_control_stack_pop(wasm_control_stack_t *cs,
                                   wasm_control_frame_t *out);
wasm_trap_t wasm_control_stack_top(wasm_control_stack_t *cs,
                                   wasm_control_frame_t **out);

/* Introspection helpers (never trap; NULL-safe). */
uint32_t wasm_control_stack_depth(const wasm_control_stack_t *cs);
int wasm_control_stack_is_empty(const wasm_control_stack_t *cs);
int wasm_control_stack_is_full(const wasm_control_stack_t *cs);

#ifdef __cplusplus
}
#endif

#endif /* WASM_CONTROL_H */

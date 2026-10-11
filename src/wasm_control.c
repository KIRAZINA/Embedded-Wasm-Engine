/*
 * wasm_control.c — fixed-depth structured-control block stack.
 * C11, -Wall -Wextra -Werror clean. No malloc.
 */

#include "wasm_control.h"

wasm_trap_t wasm_control_stack_init(wasm_control_stack_t *cs)
{
    if (cs == NULL) {
        return WASM_TRAP_NULL;
    }
    cs->depth = 0u;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_control_stack_push(wasm_control_stack_t *cs,
                                    const wasm_control_frame_t *frame)
{
    if (cs == NULL || frame == NULL) {
        return WASM_TRAP_NULL;
    }
    if (cs->depth >= (uint32_t)WASM_MAX_CONTROL_STACK_DEPTH) {
        return WASM_TRAP_CONTROL_STACK_OVERFLOW;
    }
    cs->items[cs->depth] = *frame;
    cs->depth++;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_control_stack_pop(wasm_control_stack_t *cs,
                                   wasm_control_frame_t *out)
{
    if (cs == NULL) {
        return WASM_TRAP_NULL;
    }
    if (cs->depth == 0u) {
        return WASM_TRAP_CONTROL_STACK_UNDERFLOW;
    }
    cs->depth--;
    if (out != NULL) {
        *out = cs->items[cs->depth];
    }
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_control_stack_top(wasm_control_stack_t *cs,
                                   wasm_control_frame_t **out)
{
    if (cs == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (cs->depth == 0u) {
        return WASM_TRAP_CONTROL_STACK_UNDERFLOW;
    }
    *out = &cs->items[cs->depth - 1u];
    return WASM_TRAP_OK;
}

uint32_t wasm_control_stack_depth(const wasm_control_stack_t *cs)
{
    if (cs == NULL) {
        return 0u;
    }
    return cs->depth;
}

int wasm_control_stack_is_empty(const wasm_control_stack_t *cs)
{
    if (cs == NULL) {
        return 1;
    }
    return cs->depth == 0u ? 1 : 0;
}

int wasm_control_stack_is_full(const wasm_control_stack_t *cs)
{
    if (cs == NULL) {
        return 0;
    }
    return cs->depth >= (uint32_t)WASM_MAX_CONTROL_STACK_DEPTH ? 1 : 0;
}

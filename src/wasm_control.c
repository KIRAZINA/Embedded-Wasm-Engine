/*
 * wasm_control.c — fixed-capacity structured-control block stack.
 * C11, -Wall -Wextra -Werror clean. No malloc.
 *
 * One pool per VM; one window per call frame. The pool top always
 * equals (window base + window depth) of the innermost live window.
 */

#include "wasm_control.h"

wasm_trap_t wasm_control_pool_init(wasm_control_pool_t *pool)
{
    if (pool == NULL) {
        return WASM_TRAP_NULL;
    }
    pool->depth = 0u;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_control_stack_init(wasm_control_stack_t *cs,
                                    wasm_control_pool_t *pool)
{
    if (cs == NULL || pool == NULL) {
        return WASM_TRAP_NULL;
    }
    cs->pool = pool;
    cs->base = pool->depth;
    cs->depth = 0u;
    return WASM_TRAP_OK;
}

/* True when this window is the innermost one (it owns the pool top). */
static int window_is_innermost(const wasm_control_stack_t *cs)
{
    if (cs == NULL || cs->pool == NULL) {
        return 0;
    }
    return cs->pool->depth == (cs->base + cs->depth);
}

wasm_trap_t wasm_control_stack_push(wasm_control_stack_t *cs,
                                    const wasm_control_frame_t *frame)
{
    if (cs == NULL || frame == NULL || cs->pool == NULL) {
        return WASM_TRAP_NULL;
    }
    if (!window_is_innermost(cs)) {
        return WASM_TRAP_INVALID_ARGUMENT; /* pool/window disagreement */
    }
    if (cs->pool->depth >= (uint32_t)WASM_MAX_CTRL_NESTING) {
        return WASM_TRAP_CONTROL_STACK_OVERFLOW;
    }
    cs->pool->items[cs->pool->depth] = *frame;
    cs->pool->depth++;
    cs->depth++;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_control_stack_pop(wasm_control_stack_t *cs,
                                   wasm_control_frame_t *out)
{
    if (cs == NULL || cs->pool == NULL) {
        return WASM_TRAP_NULL;
    }
    if (cs->depth == 0u) {
        return WASM_TRAP_CONTROL_STACK_UNDERFLOW;
    }
    if (!window_is_innermost(cs)) {
        return WASM_TRAP_INVALID_ARGUMENT; /* pool/window disagreement */
    }
    cs->depth--;
    cs->pool->depth--;
    if (out != NULL) {
        *out = cs->pool->items[cs->pool->depth];
    }
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_control_stack_top(wasm_control_stack_t *cs,
                                   wasm_control_frame_t **out)
{
    if (cs == NULL || cs->pool == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (cs->depth == 0u) {
        return WASM_TRAP_CONTROL_STACK_UNDERFLOW;
    }
    *out = &cs->pool->items[cs->base + cs->depth - 1u];
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_control_stack_truncate(wasm_control_stack_t *cs,
                                        uint32_t count)
{
    if (cs == NULL || cs->pool == NULL) {
        return WASM_TRAP_NULL;
    }
    if (count > cs->depth) {
        return WASM_TRAP_CONTROL_STACK_UNDERFLOW;
    }
    if (!window_is_innermost(cs)) {
        return WASM_TRAP_INVALID_ARGUMENT; /* pool/window disagreement */
    }
    cs->depth -= count;
    cs->pool->depth = cs->base + cs->depth;
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
    if (cs == NULL || cs->pool == NULL) {
        return 0;
    }
    return cs->pool->depth >= (uint32_t)WASM_MAX_CTRL_NESTING ? 1 : 0;
}

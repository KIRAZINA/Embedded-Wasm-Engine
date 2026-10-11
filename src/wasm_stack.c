/*
 * wasm_stack.c — fixed-capacity operand stack with explicit trap guards.
 * C11, -Wall -Wextra -Werror clean. No malloc.
 */

#include "wasm_stack.h"

wasm_trap_t wasm_stack_init(wasm_stack_t *s)
{
    if (s == NULL) {
        return WASM_TRAP_NULL;
    }
    s->top = 0u;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_stack_push(wasm_stack_t *s, wasm_val_t v)
{
    if (s == NULL) {
        return WASM_TRAP_NULL;
    }
    if (s->top >= (uint32_t)WASM_STACK_MAX_DEPTH) {
        return WASM_TRAP_STACK_OVERFLOW;
    }
    s->data[s->top] = v;
    s->top++;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_stack_pop(wasm_stack_t *s, wasm_val_t *out)
{
    if (s == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (s->top == 0u) {
        return WASM_TRAP_STACK_UNDERFLOW;
    }
    s->top--;
    *out = s->data[s->top];
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_stack_pop_typed(wasm_stack_t *s, wasm_valtype_t want, wasm_val_t *out)
{
    wasm_val_t v;
    wasm_trap_t tr;
    if (s == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (s->top == 0u) {
        return WASM_TRAP_STACK_UNDERFLOW;
    }
    /* Peek first: on type mismatch the stack is left untouched. */
    v = s->data[s->top - 1u];
    if (v.type != want) {
        return WASM_TRAP_TYPE_MISMATCH;
    }
    tr = wasm_stack_pop(s, out);
    return tr;
}

wasm_trap_t wasm_stack_peek(const wasm_stack_t *s, wasm_val_t *out)
{
    if (s == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (s->top == 0u) {
        return WASM_TRAP_STACK_UNDERFLOW;
    }
    *out = s->data[s->top - 1u];
    return WASM_TRAP_OK;
}

uint32_t wasm_stack_depth(const wasm_stack_t *s)
{
    if (s == NULL) {
        return 0u;
    }
    return s->top;
}

int wasm_stack_is_empty(const wasm_stack_t *s)
{
    if (s == NULL) {
        return 1;
    }
    return s->top == 0u ? 1 : 0;
}

int wasm_stack_is_full(const wasm_stack_t *s)
{
    if (s == NULL) {
        return 0;
    }
    return s->top >= (uint32_t)WASM_STACK_MAX_DEPTH ? 1 : 0;
}

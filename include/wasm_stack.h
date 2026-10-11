#ifndef WASM_STACK_H
#define WASM_STACK_H

/*
 * wasm_stack.h — Phase 2 fixed-capacity operand stack.
 *
 * - Zero allocation: storage is embedded in wasm_stack_t (statically
 *   allocatable; declare it `static` — it is ~16 KiB with the default depth).
 * - Every mutating/accessing op is guard-checked and returns a wasm_trap_t:
 *     push full   -> WASM_TRAP_STACK_OVERFLOW
 *     pop/peek empty -> WASM_TRAP_STACK_UNDERFLOW
 *     pop_typed wrong tag -> WASM_TRAP_TYPE_MISMATCH
 * - C11, no float aliasing issues (values carried as tagged union, never
 *   reinterpreted through pointers).
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    wasm_val_t data[WASM_STACK_MAX_DEPTH];
    uint32_t top; /* number of live entries; next push writes data[top] */
} wasm_stack_t;

wasm_trap_t wasm_stack_init(wasm_stack_t *s);
wasm_trap_t wasm_stack_push(wasm_stack_t *s, wasm_val_t v);
wasm_trap_t wasm_stack_pop(wasm_stack_t *s, wasm_val_t *out);
wasm_trap_t wasm_stack_pop_typed(wasm_stack_t *s, wasm_valtype_t want, wasm_val_t *out);
wasm_trap_t wasm_stack_peek(const wasm_stack_t *s, wasm_val_t *out);

/* Introspection helpers (never trap; NULL-safe where noted). */
uint32_t wasm_stack_depth(const wasm_stack_t *s);
int wasm_stack_is_empty(const wasm_stack_t *s);
int wasm_stack_is_full(const wasm_stack_t *s);

#ifdef __cplusplus
}
#endif

#endif /* WASM_STACK_H */

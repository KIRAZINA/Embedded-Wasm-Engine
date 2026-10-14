#ifndef WASM_MEMORY_H
#define WASM_MEMORY_H

/*
 * wasm_memory.h — Phase 2 WASM linear memory arena.
 *
 * - Zero allocation: backing storage is BORROWED from the host caller
 *   (static/stack array). init/grow never call malloc; grow only enlarges
 *   the *visible* window inside the pre-supplied backing and zero-fills it.
 * - All loads/stores are bounds-checked with subtraction-form guards
 *       access_size > total  -> OOB
 *       offset > total - access_size -> OOB
 *   so offset+size wraparound can never slip through (uint64 math).
 * - Unaligned-safe + endian-exact: memory is touched one byte at a time and
 *   multi-byte values are reconstructed with shifts (LE). Floats/doubles are
 *   transferred with memcpy (no aliasing UB, no unaligned word casts), so
 *   this is safe on strict-alignment ARM (Cortex-M) hosts.
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *bytes;          /* borrowed backing (never owned/freed) */
    uint32_t num_pages;      /* currently visible pages */
    uint32_t max_pages;      /* grow ceiling (pages) */
    uint32_t capacity_bytes; /* backing size in bytes */
} wasm_memory_t;

wasm_trap_t wasm_memory_init(wasm_memory_t *m, uint8_t *backing,
                             uint32_t backing_size, uint32_t initial_pages,
                             uint32_t max_pages);

/*
 * Grow visible memory by delta_pages. On success *prev_pages_out (if non-NULL)
 * receives the pre-grow count and the new tail is zero-filled. On failure the
 * memory is untouched. Failure -> WASM_TRAP_MEMORY_GROW_FAILED.
 */
wasm_trap_t wasm_mem_grow(wasm_memory_t *m, uint32_t delta_pages,
                          uint32_t *prev_pages_out);

uint32_t wasm_memory_pages(const wasm_memory_t *m);
uint64_t wasm_memory_size_bytes(const wasm_memory_t *m);

/* Bounds-checked accessors (all LE, all unaligned-safe). */
wasm_trap_t wasm_memory_load_u8(const wasm_memory_t *m, uint32_t offset, uint8_t *out);
wasm_trap_t wasm_memory_store_u8(wasm_memory_t *m, uint32_t offset, uint8_t v);

wasm_trap_t wasm_memory_load_u16(const wasm_memory_t *m, uint32_t offset, uint16_t *out);
wasm_trap_t wasm_memory_store_u16(wasm_memory_t *m, uint32_t offset, uint16_t v);

wasm_trap_t wasm_memory_load_i32(const wasm_memory_t *m, uint32_t offset, int32_t *out);
wasm_trap_t wasm_memory_store_i32(wasm_memory_t *m, uint32_t offset, int32_t v);

wasm_trap_t wasm_memory_load_i64(const wasm_memory_t *m, uint32_t offset, int64_t *out);
wasm_trap_t wasm_memory_store_i64(wasm_memory_t *m, uint32_t offset, int64_t v);

wasm_trap_t wasm_memory_load_f32(const wasm_memory_t *m, uint32_t offset, float *out);
wasm_trap_t wasm_memory_store_f32(wasm_memory_t *m, uint32_t offset, float v);

wasm_trap_t wasm_memory_load_f64(const wasm_memory_t *m, uint32_t offset, double *out);
wasm_trap_t wasm_memory_store_f64(wasm_memory_t *m, uint32_t offset, double v);

/* Alias names used by the execution core (identical bounds-checked LE semantics). */
wasm_trap_t wasm_mem_read_i32(const wasm_memory_t *m, uint32_t offset, int32_t *out);
wasm_trap_t wasm_mem_write_i32(wasm_memory_t *m, uint32_t offset, int32_t v);

#ifdef __cplusplus
}
#endif

#endif /* WASM_MEMORY_H */

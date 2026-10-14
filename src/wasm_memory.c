/*
 * wasm_memory.c — bounds-checked LE linear memory over host-borrowed arena.
 * C11, -Wall -Wextra -Werror clean. No malloc. No unaligned word access.
 */

#include "wasm_memory.h"

#include <string.h>

/* Total visible bytes as 64-bit (num_pages * 64KiB can be 2^32, overflows u32). */
static uint64_t visible_bytes(const wasm_memory_t *m)
{
    return (uint64_t)m->num_pages * (uint64_t)WASM_PAGE_SIZE;
}

/*
 * Subtraction-form bounds check (wrap-safe):
 *   access > total        -> OOB (also covers total < access underflow)
 *   offset > total-access -> OOB
 * Uses uint64_t so it is correct even at the 4 GiB edge.
 */
static wasm_trap_t check_bounds(const wasm_memory_t *m, uint32_t offset,
                                uint64_t access_size)
{
    uint64_t total = visible_bytes(m);
    if (access_size > total) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    if ((uint64_t)offset > total - access_size) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_memory_init(wasm_memory_t *m, uint8_t *backing,
                             uint32_t backing_size, uint32_t initial_pages,
                             uint32_t max_pages)
{
    uint64_t need;
    uint32_t i;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (backing_size > 0u && backing == NULL) {
        return WASM_TRAP_NULL;
    }
    if (max_pages > WASM_MEMORY_MAX_PAGES_ABSOLUTE) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (initial_pages > max_pages) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    need = (uint64_t)initial_pages * (uint64_t)WASM_PAGE_SIZE;
    if (need > (uint64_t)backing_size) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    m->bytes = backing;
    m->num_pages = initial_pages;
    m->max_pages = max_pages;
    m->capacity_bytes = backing_size;
    /* Zero the visible window so hosts with dirty static arrays start clean. */
    for (i = 0u; (uint64_t)i < need; i++) {
        backing[i] = 0u;
    }
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_mem_grow(wasm_memory_t *m, uint32_t delta_pages,
                          uint32_t *prev_pages_out)
{
    uint64_t old_bytes;
    uint64_t new_bytes;
    uint64_t new_pages64;
    uint64_t i;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL && (m->capacity_bytes > 0u || m->num_pages > 0u)) {
        return WASM_TRAP_NULL;
    }
    if (delta_pages == 0u) {
        if (prev_pages_out != NULL) {
            *prev_pages_out = m->num_pages;
        }
        return WASM_TRAP_OK;
    }
    new_pages64 = (uint64_t)m->num_pages + (uint64_t)delta_pages;
    if (new_pages64 > (uint64_t)m->max_pages) {
        return WASM_TRAP_MEMORY_GROW_FAILED;
    }
    if (new_pages64 > (uint64_t)WASM_MEMORY_MAX_PAGES_ABSOLUTE) {
        return WASM_TRAP_MEMORY_GROW_FAILED;
    }
    new_bytes = new_pages64 * (uint64_t)WASM_PAGE_SIZE;
    if (new_bytes > (uint64_t)m->capacity_bytes) {
        return WASM_TRAP_MEMORY_GROW_FAILED;
    }
    old_bytes = visible_bytes(m);
    /* Zero-fill the newly exposed tail BEFORE publishing the new size. */
    for (i = old_bytes; i < new_bytes; i++) {
        m->bytes[i] = 0u;
    }
    if (prev_pages_out != NULL) {
        *prev_pages_out = m->num_pages;
    }
    m->num_pages = (uint32_t)new_pages64;
    return WASM_TRAP_OK;
}

uint32_t wasm_memory_pages(const wasm_memory_t *m)
{
    if (m == NULL) {
        return 0u;
    }
    return m->num_pages;
}

uint64_t wasm_memory_size_bytes(const wasm_memory_t *m)
{
    if (m == NULL) {
        return 0u;
    }
    return visible_bytes(m);
}

/* ---- u8 ---- */

wasm_trap_t wasm_memory_load_u8(const wasm_memory_t *m, uint32_t offset, uint8_t *out)
{
    wasm_trap_t tr;
    if (m == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 1u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    *out = m->bytes[offset];
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_memory_store_u8(wasm_memory_t *m, uint32_t offset, uint8_t v)
{
    wasm_trap_t tr;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 1u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    m->bytes[offset] = v;
    return WASM_TRAP_OK;
}

/* ---- u16 (LE, byte-wise; bounds-checked as one 2-byte access so a
 * trapped half can never leave a partial read/write) ---- */

wasm_trap_t wasm_memory_load_u16(const wasm_memory_t *m, uint32_t offset, uint16_t *out)
{
    wasm_trap_t tr;
    uint16_t u;
    if (m == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 2u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    u = (uint16_t)m->bytes[offset] |
        (uint16_t)((uint16_t)m->bytes[offset + 1u] << 8u);
    *out = u;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_memory_store_u16(wasm_memory_t *m, uint32_t offset, uint16_t v)
{
    wasm_trap_t tr;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 2u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    m->bytes[offset]      = (uint8_t)(v & 0xFFu);
    m->bytes[offset + 1u] = (uint8_t)((v >> 8u) & 0xFFu);
    return WASM_TRAP_OK;
}

/* ---- i32 (LE, byte-wise) ---- */

wasm_trap_t wasm_memory_load_i32(const wasm_memory_t *m, uint32_t offset, int32_t *out)
{
    wasm_trap_t tr;
    uint32_t u;
    if (m == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 4u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    u = (uint32_t)m->bytes[offset] |
        ((uint32_t)m->bytes[offset + 1u] << 8u) |
        ((uint32_t)m->bytes[offset + 2u] << 16u) |
        ((uint32_t)m->bytes[offset + 3u] << 24u);
    memcpy(out, &u, sizeof(u));
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_memory_store_i32(wasm_memory_t *m, uint32_t offset, int32_t v)
{
    wasm_trap_t tr;
    uint32_t u;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 4u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    memcpy(&u, &v, sizeof(u));
    m->bytes[offset]      = (uint8_t)(u & 0xFFu);
    m->bytes[offset + 1u] = (uint8_t)((u >> 8u) & 0xFFu);
    m->bytes[offset + 2u] = (uint8_t)((u >> 16u) & 0xFFu);
    m->bytes[offset + 3u] = (uint8_t)((u >> 24u) & 0xFFu);
    return WASM_TRAP_OK;
}

/* ---- i64 (LE, byte-wise) ---- */

wasm_trap_t wasm_memory_load_i64(const wasm_memory_t *m, uint32_t offset, int64_t *out)
{
    wasm_trap_t tr;
    uint64_t u;
    unsigned k;
    if (m == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 8u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    u = 0u;
    for (k = 0u; k < 8u; k++) {
        u |= ((uint64_t)m->bytes[offset + k]) << (8u * k);
    }
    memcpy(out, &u, sizeof(u));
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_memory_store_i64(wasm_memory_t *m, uint32_t offset, int64_t v)
{
    wasm_trap_t tr;
    uint64_t u;
    unsigned k;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 8u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    memcpy(&u, &v, sizeof(u));
    for (k = 0u; k < 8u; k++) {
        m->bytes[offset + k] = (uint8_t)((u >> (8u * k)) & 0xFFu);
    }
    return WASM_TRAP_OK;
}

/* ---- f32 (LE bytes <-> uint32 <-> float via memcpy; no aliasing UB) ---- */

wasm_trap_t wasm_memory_load_f32(const wasm_memory_t *m, uint32_t offset, float *out)
{
    wasm_trap_t tr;
    uint32_t u;
    if (m == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 4u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    u = (uint32_t)m->bytes[offset] |
        ((uint32_t)m->bytes[offset + 1u] << 8u) |
        ((uint32_t)m->bytes[offset + 2u] << 16u) |
        ((uint32_t)m->bytes[offset + 3u] << 24u);
    memcpy(out, &u, sizeof(u));
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_memory_store_f32(wasm_memory_t *m, uint32_t offset, float v)
{
    wasm_trap_t tr;
    uint32_t u;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 4u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    memcpy(&u, &v, sizeof(u));
    m->bytes[offset]      = (uint8_t)(u & 0xFFu);
    m->bytes[offset + 1u] = (uint8_t)((u >> 8u) & 0xFFu);
    m->bytes[offset + 2u] = (uint8_t)((u >> 16u) & 0xFFu);
    m->bytes[offset + 3u] = (uint8_t)((u >> 24u) & 0xFFu);
    return WASM_TRAP_OK;
}

/* ---- f64 ---- */

wasm_trap_t wasm_memory_load_f64(const wasm_memory_t *m, uint32_t offset, double *out)
{
    wasm_trap_t tr;
    uint64_t u = 0u;
    unsigned k;
    if (m == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 8u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    for (k = 0u; k < 8u; k++) {
        u |= ((uint64_t)m->bytes[offset + k]) << (8u * k);
    }
    memcpy(out, &u, sizeof(u));
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_memory_store_f64(wasm_memory_t *m, uint32_t offset, double v)
{
    wasm_trap_t tr;
    uint64_t u;
    unsigned k;
    if (m == NULL) {
        return WASM_TRAP_NULL;
    }
    if (m->bytes == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    tr = check_bounds(m, offset, 8u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    memcpy(&u, &v, sizeof(u));
    for (k = 0u; k < 8u; k++) {
        m->bytes[offset + k] = (uint8_t)((u >> (8u * k)) & 0xFFu);
    }
    return WASM_TRAP_OK;
}

/* ---- execution-core aliases (same LE bounds-checked semantics) ---- */

wasm_trap_t wasm_mem_read_i32(const wasm_memory_t *m, uint32_t offset, int32_t *out)
{
    if (out == NULL) {
        return WASM_TRAP_NULL;
    }
    return wasm_memory_load_i32(m, offset, out);
}

wasm_trap_t wasm_mem_write_i32(wasm_memory_t *m, uint32_t offset, int32_t v)
{
    return wasm_memory_store_i32(m, offset, v);
}

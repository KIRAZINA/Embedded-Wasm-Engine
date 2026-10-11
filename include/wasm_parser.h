#ifndef WASM_PARSER_H
#define WASM_PARSER_H

/*
 * wasm_parser.h — Phase 1 WASM binary header parser + section traverser.
 *
 * Zero-copy: the parser never allocates. It validates the 8-byte header
 * (magic `\0asm` + LE version 1) and indexes sections as pointer offsets
 * into the caller's buffer. The caller must keep the buffer alive.
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque-ish module view over a borrowed buffer.
 *  base            -> start of the .wasm bytes (borrowed, not owned)
 *  total_len       -> total buffer length
 *  sections_begin  -> first byte after the 8-byte header (valid after OK)
 *  sections_end    -> base + total_len (one past the end)
 */
typedef struct {
    const uint8_t *base;
    size_t total_len;
    const uint8_t *sections_begin;
    const uint8_t *sections_end;
} wasm_module_t;

/*
 * Validate magic + version. On WASM_OK, fills `out` with zero-copy cursors.
 * Errors: WASM_ERR_NULL / WASM_ERR_TOO_SHORT / WASM_ERR_MAGIC / WASM_ERR_VERSION.
 */
wasm_err_t wasm_parse_header(const uint8_t *data, size_t len, wasm_module_t *out);

/*
 * Sequential section iterator (zero-copy).
 *  - `cursor` is advanced past [id][size LEB][payload] on success only.
 *  - `end` is the module's sections_end (exclusive bound, checked on every read).
 *  - `out->payload` points into the original buffer; len == size.
 * Errors: WASM_ERR_NULL / WASM_ERR_TRUNCATED (header cut off) /
 *         WASM_ERR_LEB_TRUNCATED / WASM_ERR_LEB_OVERFLOW (bad size LEB) /
 *         WASM_ERR_INVALID_SECTION (id > WASM_SECT_MAX) /
 *         WASM_ERR_SECTION_OVERFLOW (payload runs past `end`).
 * Returns WASM_ERR_TRUNCATED when *cursor == end (no more sections)? No —
 * the caller must check `*cursor >= end` before calling. Calling at end
 * returns WASM_ERR_TRUNCATED to signal "nothing left".
 */
wasm_err_t wasm_get_section(const uint8_t **cursor, const uint8_t *end,
                            wasm_section_header_t *out);

/*
 * Convenience linear search for the first section with matching id.
 * Zero-copy; returns WASM_OK + fills `out` on hit, WASM_ERR_TRUNCATED on miss.
 */
wasm_err_t wasm_find_section(const wasm_module_t *mod, uint8_t id,
                             wasm_section_header_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WASM_PARSER_H */

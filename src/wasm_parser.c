/*
 * wasm_parser.c — WASM header validation + zero-copy section traversal.
 * C11, -Wall -Wextra -Werror clean. No malloc. LE byte-stream decoding only.
 */

#include "wasm_parser.h"

#include "leb128.h"

wasm_err_t wasm_parse_header(const uint8_t *data, size_t len, wasm_module_t *out)
{
    uint32_t version;

    if (data == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    if (len < WASM_HEADER_SIZE) {
        return WASM_ERR_TOO_SHORT;
    }

    /* Magic: 00 61 73 6D ("\0asm"). Byte-wise compare (endian-independent). */
    if (data[0] != WASM_MAGIC_0 ||
        data[1] != WASM_MAGIC_1 ||
        data[2] != WASM_MAGIC_2 ||
        data[3] != WASM_MAGIC_3) {
        return WASM_ERR_MAGIC;
    }

    /* Version: uint32 LE == 1, i.e. bytes 01 00 00 00. Rebuilt with shifts
     * so the check is correct on big- and little-endian hosts alike. */
    version = (uint32_t)data[4] |
              ((uint32_t)data[5] << 8u) |
              ((uint32_t)data[6] << 16u) |
              ((uint32_t)data[7] << 24u);
    if (version != (uint32_t)WASM_VERSION) {
        return WASM_ERR_VERSION;
    }

    out->base = data;
    out->total_len = len;
    out->sections_begin = data + WASM_HEADER_SIZE;
    out->sections_end = data + len;
    return WASM_OK;
}

wasm_err_t wasm_get_section(const uint8_t **cursor, const uint8_t *end,
                            wasm_section_header_t *out)
{
    const uint8_t *p;
    uint8_t id;
    uint32_t size = 0u;
    const uint8_t *payload = NULL;
    wasm_err_t rc;

    if (cursor == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    p = *cursor;
    if (p == NULL || end == NULL) {
        return WASM_ERR_NULL;
    }
    if (p >= end) {
        return WASM_ERR_TRUNCATED; /* no more sections */
    }
    /* Section id is a single byte; gap check above guarantees 1 readable byte. */
    id = *p++;
    if (id > (uint8_t)WASM_SECT_MAX) {
        return WASM_ERR_INVALID_SECTION;
    }
    /* Section size is ULEB128 u32; decoder enforces bounds + overflow rules. */
    rc = leb128_decode_u32(&p, end, &size);
    if (rc == WASM_ERR_LEB_TRUNCATED) {
        return WASM_ERR_LEB_TRUNCATED;
    }
    if (rc != WASM_OK) {
        return rc;
    }
    payload = p;
    /* Zero-copy bounds check: payload [p, p+size) must lie within [p, end].
     * Subtraction form avoids pointer-overflow UB on (p + size). */
    if ((uint64_t)size > (uint64_t)(size_t)(end - payload)) {
        return WASM_ERR_SECTION_OVERFLOW;
    }

    out->id = id;
    out->size = size;
    out->payload.data = payload;
    out->payload.len = (size_t)size;

    /* Advance caller's cursor past the payload (success path only). */
    *cursor = payload + size;
    return WASM_OK;
}

wasm_err_t wasm_find_section(const wasm_module_t *mod, uint8_t id,
                             wasm_section_header_t *out)
{
    const uint8_t *cursor;
    wasm_section_header_t cur;

    if (mod == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    if (mod->sections_begin == NULL || mod->sections_end == NULL) {
        return WASM_ERR_NULL;
    }
    if (id > (uint8_t)WASM_SECT_MAX) {
        return WASM_ERR_INVALID_SECTION;
    }
    cursor = mod->sections_begin;
    while (cursor < mod->sections_end) {
        wasm_err_t rc = wasm_get_section(&cursor, mod->sections_end, &cur);
        if (rc != WASM_OK) {
            return rc;
        }
        if (cur.id == id) {
            *out = cur;
            return WASM_OK;
        }
    }
    return WASM_ERR_TRUNCATED; /* not found */
}

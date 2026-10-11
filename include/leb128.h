#ifndef LEB128_H
#define LEB128_H

/*
 * leb128.h — strict LEB128 decoders (unsigned/signed, 32/64-bit).
 *
 * Guarantees:
 *  - No reads past `end` (buffer-overrun safe). Every byte load is guarded
 *    by `p < end`. Unterminated streams return WASM_ERR_LEB_TRUNCATED.
 *  - No integer overflow: accumulators are unsigned, shifts are bounded, and
 *    surplus high bits in the final byte are rejected with
 *    WASM_ERR_LEB_OVERFLOW (unsigned) or sign-consistency-checked (signed).
 *  - Endianness-independent: pure shift/or on byte values, no casts.
 *  - Zero allocation. `*pp` advances past the consumed bytes ONLY on success;
 *    on error it is left unmodified and `*out` is untouched.
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

wasm_err_t leb128_decode_u32(const uint8_t **pp, const uint8_t *end, uint32_t *out);
wasm_err_t leb128_decode_u64(const uint8_t **pp, const uint8_t *end, uint64_t *out);
wasm_err_t leb128_decode_s32(const uint8_t **pp, const uint8_t *end, int32_t *out);
wasm_err_t leb128_decode_s64(const uint8_t **pp, const uint8_t *end, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LEB128_H */

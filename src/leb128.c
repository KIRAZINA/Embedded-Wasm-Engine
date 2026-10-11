/*
 * leb128.c — strict LEB128 decoders.
 * C11, -Wall -Wextra -Werror clean. No malloc. Little-endian byte stream only.
 */

#include "leb128.h"

wasm_err_t leb128_decode_u32(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p;
    uint32_t result = 0u;
    unsigned shift = 0u;

    if (pp == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    p = *pp;
    if (p == NULL || end == NULL || p >= end) {
        return (p == NULL || end == NULL) ? WASM_ERR_NULL : WASM_ERR_LEB_TRUNCATED;
    }

    /* Unsigned 32-bit: at most 5 bytes (5*7 = 35 bits, top 3 must be zero). */
    for (unsigned i = 0u; i < 5u; i++) {
        uint8_t byte;
        uint32_t payload;
        if (p >= end) {
            return WASM_ERR_LEB_TRUNCATED;
        }
        byte = *p++;
        payload = (uint32_t)(byte & 0x7Fu);
        if (i == 4u) {
            /* Final byte: continuation must be clear and only low 4 bits set. */
            if ((byte & 0x80u) != 0u) {
                return WASM_ERR_LEB_OVERFLOW;
            }
            if ((payload >> 4u) != 0u) {
                return WASM_ERR_LEB_OVERFLOW;
            }
            result |= (payload << shift);
            *out = result;
            *pp = p;
            return WASM_OK;
        }
        result |= (payload << shift);
        if ((byte & 0x80u) == 0u) {
            *out = result;
            *pp = p;
            return WASM_OK;
        }
        shift += 7u;
    }
    return WASM_ERR_LEB_OVERFLOW; /* unreachable: 5th byte always decides */
}

wasm_err_t leb128_decode_u64(const uint8_t **pp, const uint8_t *end, uint64_t *out)
{
    const uint8_t *p;
    uint64_t result = 0u;
    unsigned shift = 0u;

    if (pp == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    p = *pp;
    if (p == NULL || end == NULL || p >= end) {
        return (p == NULL || end == NULL) ? WASM_ERR_NULL : WASM_ERR_LEB_TRUNCATED;
    }

    /* Unsigned 64-bit: at most 10 bytes (10*7 = 70 bits, top 6 must be zero). */
    for (unsigned i = 0u; i < 10u; i++) {
        uint8_t byte;
        uint64_t payload;
        if (p >= end) {
            return WASM_ERR_LEB_TRUNCATED;
        }
        byte = *p++;
        payload = (uint64_t)(byte & 0x7Fu);
        if (i == 9u) {
            if ((byte & 0x80u) != 0u) {
                return WASM_ERR_LEB_OVERFLOW;
            }
            if ((payload >> 1u) != 0u) {
                return WASM_ERR_LEB_OVERFLOW;
            }
            result |= (payload << shift);
            *out = result;
            *pp = p;
            return WASM_OK;
        }
        result |= (payload << shift);
        if ((byte & 0x80u) == 0u) {
            *out = result;
            *pp = p;
            return WASM_OK;
        }
        shift += 7u;
    }
    return WASM_ERR_LEB_OVERFLOW; /* unreachable */
}

wasm_err_t leb128_decode_s32(const uint8_t **pp, const uint8_t *end, int32_t *out)
{
    const uint8_t *p;
    uint32_t raw = 0u;
    unsigned shift = 0u;
    uint8_t byte = 0u;

    if (pp == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    p = *pp;
    if (p == NULL || end == NULL || p >= end) {
        return (p == NULL || end == NULL) ? WASM_ERR_NULL : WASM_ERR_LEB_TRUNCATED;
    }

    /* Signed 32-bit: at most 5 bytes. Last byte's high payload bits must
     * equal the sign bit (bit 3 of payload == bit 31 of value). */
    for (unsigned i = 0u; i < 5u; i++) {
        uint32_t payload;
        if (p >= end) {
            return WASM_ERR_LEB_TRUNCATED;
        }
        byte = *p++;
        payload = (uint32_t)(byte & 0x7Fu);
        if (i == 4u) {
            uint32_t sign;
            uint32_t high;
            if ((byte & 0x80u) != 0u) {
                return WASM_ERR_LEB_OVERFLOW; /* > 5 bytes */
            }
            /* shift == 28 here. Bit 3 of payload is the sign (value bit 31). */
            sign = (payload >> 3u) & 1u;
            /* Bits [6:4] must all equal sign for the value to fit in 32 bits. */
            high = (payload >> 4u) & 0x07u;
            if (sign == 0u) {
                if (high != 0u) {
                    return WASM_ERR_LEB_OVERFLOW;
                }
            } else {
                if (high != 0x07u) {
                    return WASM_ERR_LEB_OVERFLOW;
                }
            }
            raw |= (payload << shift);
            /* Sign-extend when the terminating group leaves bits above shift. */
            if (shift < 32u && ((byte & 0x40u) != 0u)) {
                /* shift == 28: set top bits above bit 31? none remain, but
                 * keep the generic form for clarity; raw already holds bit 31. */
                raw |= (shift + 7u < 32u) ? (0xFFFFFFFFu << (shift + 7u)) : 0u;
            }
            *out = (int32_t)raw;
            *pp = p;
            return WASM_OK;
        }
        raw |= (payload << shift);
        shift += 7u;
        if ((byte & 0x80u) == 0u) {
            /* Terminated early: sign-extend from current shift. */
            if (shift < 32u && ((byte & 0x40u) != 0u)) {
                raw |= (0xFFFFFFFFu << shift);
            }
            /* Overflow check for early termination: bits above `shift` must
             * match the sign (they do by construction), but values that could
             * have used fewer bytes are still accepted (non-canonical OK). */
            *out = (int32_t)raw;
            *pp = p;
            return WASM_OK;
        }
    }
    return WASM_ERR_LEB_OVERFLOW; /* unreachable */
}

wasm_err_t leb128_decode_s64(const uint8_t **pp, const uint8_t *end, int64_t *out)
{
    const uint8_t *p;
    uint64_t raw = 0u;
    unsigned shift = 0u;
    uint8_t byte = 0u;

    if (pp == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    p = *pp;
    if (p == NULL || end == NULL || p >= end) {
        return (p == NULL || end == NULL) ? WASM_ERR_NULL : WASM_ERR_LEB_TRUNCATED;
    }

    /* Signed 64-bit: at most 10 bytes. Last byte's payload bits [6:1] must
     * equal bit 0 (value bit 63, the sign). */
    for (unsigned i = 0u; i < 10u; i++) {
        uint64_t payload;
        if (p >= end) {
            return WASM_ERR_LEB_TRUNCATED;
        }
        byte = *p++;
        payload = (uint64_t)(byte & 0x7Fu);
        if (i == 9u) {
            uint64_t sign;
            uint64_t high;
            if ((byte & 0x80u) != 0u) {
                return WASM_ERR_LEB_OVERFLOW; /* > 10 bytes */
            }
            /* shift == 63. Bit 0 of payload is value bit 63 (sign). */
            sign = payload & 1u;
            high = (payload >> 1u) & 0x3Fu;
            if (sign == 0u) {
                if (high != 0u) {
                    return WASM_ERR_LEB_OVERFLOW;
                }
            } else {
                if (high != 0x3Fu) {
                    return WASM_ERR_LEB_OVERFLOW;
                }
            }
            raw |= (payload << shift);
            *out = (int64_t)raw;
            *pp = p;
            return WASM_OK;
        }
        raw |= (payload << shift);
        shift += 7u;
        if ((byte & 0x80u) == 0u) {
            if (shift < 64u && ((byte & 0x40u) != 0u)) {
                raw |= (0xFFFFFFFFFFFFFFFFu << shift);
            }
            *out = (int64_t)raw;
            *pp = p;
            return WASM_OK;
        }
    }
    return WASM_ERR_LEB_OVERFLOW; /* unreachable */
}

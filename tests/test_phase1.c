/*
 * test_phase1.c — standalone Phase 1 verification (C11, assert.h + stdio.h only).
 *
 * Covers (required):
 *  1. Valid dummy WASM header (+ multi-section traversal)
 *  2. Corrupt magic bytes
 *  3. Truncated LEB128 streams
 *  4. Out-of-bounds section payload length
 * Plus: bad version, short buffer, LEB overflow (u32/u64/s32/s64),
 *       signed LEB vectors, truncated section header, bad section id.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -Iinclude \
 *            src/leb128.c src/wasm_parser.c tests/test_phase1.c -o build/test_phase1
 */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include "leb128.h"
#include "wasm_parser.h"
#include "wasm_types.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

/* ---------- LEB128 unit tests ---------- */

static void test_leb_u32_valid(void)
{
    printf("[T] leb128_decode_u32 — valid vectors\n");
    {
        /* 624485 = E5 8E 26 (canonical example from DWARF/LEB spec). */
        const uint8_t buf[] = { 0xE5, 0x8E, 0x26 };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        uint32_t v = 0;
        CHECK(leb128_decode_u32(&p, end, &v) == WASM_OK && v == 624485u, "u32 624485");
        CHECK(p == end, "u32 cursor advanced to end");
    }
    {
        const uint8_t buf[] = { 0x00 };
        const uint8_t *p = buf, *end = buf + 1;
        uint32_t v = 0xDEADu;
        CHECK(leb128_decode_u32(&p, end, &v) == WASM_OK && v == 0u, "u32 zero");
    }
    {
        /* UINT32_MAX = FF FF FF FF 0F */
        const uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        uint32_t v = 0;
        CHECK(leb128_decode_u32(&p, end, &v) == WASM_OK && v == 0xFFFFFFFFu, "u32 UINT32_MAX");
    }
    {
        /* 127 = 0x7F single byte; 128 = 0x80 0x01 */
        const uint8_t b1[] = { 0x7F };
        const uint8_t b2[] = { 0x80, 0x01 };
        const uint8_t *p;
        uint32_t v = 0;
        p = b1; CHECK(leb128_decode_u32(&p, b1 + 1, &v) == WASM_OK && v == 127u, "u32 127");
        p = b2; CHECK(leb128_decode_u32(&p, b2 + 2, &v) == WASM_OK && v == 128u, "u32 128");
    }
}

static void test_leb_truncated(void)
{
    printf("[T] LEB128 — truncated streams (REQUIRED case)\n");
    {
        /* Continuation bit set but buffer ends: 0x80 alone. */
        const uint8_t buf[] = { 0x80 };
        const uint8_t *p = buf, *end = buf + 1;
        uint32_t v = 0;
        CHECK(leb128_decode_u32(&p, end, &v) == WASM_ERR_LEB_TRUNCATED, "u32 truncated 0x80");
        CHECK(p == buf, "cursor unmoved on truncation");
    }
    {
        const uint8_t buf[] = { 0xE5, 0x8E }; /* missing 0x26 */
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        uint32_t v = 0;
        CHECK(leb128_decode_u32(&p, end, &v) == WASM_ERR_LEB_TRUNCATED, "u32 truncated mid-value");
    }
    {
        /* Empty range: p == end. */
        const uint8_t dummy = 0;
        const uint8_t *p = &dummy, *end = &dummy;
        uint64_t v = 0;
        CHECK(leb128_decode_u64(&p, end, &v) == WASM_ERR_LEB_TRUNCATED, "u64 empty range truncated");
    }
    {
        const uint8_t buf[] = { 0xFF }; /* signed stream cut off */
        const uint8_t *p = buf, *end = buf + 1;
        int32_t v = 0;
        CHECK(leb128_decode_s32(&p, end, &v) == WASM_ERR_LEB_TRUNCATED, "s32 truncated 0xFF");
    }
    {
        const uint8_t buf[] = { 0x80, 0x80, 0x80 };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        int64_t v = 0;
        CHECK(leb128_decode_s64(&p, end, &v) == WASM_ERR_LEB_TRUNCATED, "s64 truncated chain");
    }
}

static void test_leb_overflow(void)
{
    printf("[T] LEB128 — overflow rejection\n");
    {
        /* 6-byte u32 (continuation on byte 5) -> overflow. */
        const uint8_t buf[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        uint32_t v = 0;
        CHECK(leb128_decode_u32(&p, end, &v) == WASM_ERR_LEB_OVERFLOW, "u32 6-byte overflow");
    }
    {
        /* 5th byte with high bits set: FF FF FF FF 7F (35-bit value). */
        const uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x7F };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        uint32_t v = 0;
        CHECK(leb128_decode_u32(&p, end, &v) == WASM_ERR_LEB_OVERFLOW, "u32 high-bits overflow");
    }
    {
        /* 11-byte u64 -> overflow. */
        const uint8_t buf[] = { 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x01 };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        uint64_t v = 0;
        CHECK(leb128_decode_u64(&p, end, &v) == WASM_ERR_LEB_OVERFLOW, "u64 11-byte overflow");
    }
    {
        /* u64 max is FF..01 (10 bytes); payload 0x7F in last byte overflows 64 bits. */
        const uint8_t buf[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x7F };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        uint64_t v = 0;
        CHECK(leb128_decode_u64(&p, end, &v) == WASM_ERR_LEB_OVERFLOW, "u64 top-bits overflow");
    }
    {
        /* s32 positive overflow: 5-byte encoding of 2^31 (needs 64-bit). */
        const uint8_t buf[] = { 0x80, 0x80, 0x80, 0x80, 0x08 };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        int32_t v = 0;
        CHECK(leb128_decode_s32(&p, end, &v) == WASM_ERR_LEB_OVERFLOW, "s32 +2^31 overflow");
    }
    {
        /* s64 11-byte stream -> overflow. */
        const uint8_t buf[] = { 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x01 };
        const uint8_t *p = buf, *end = buf + sizeof(buf);
        int64_t v = 0;
        CHECK(leb128_decode_s64(&p, end, &v) == WASM_ERR_LEB_OVERFLOW, "s64 11-byte overflow");
    }
}

static void test_leb_signed_vectors(void)
{
    printf("[T] leb128 signed — valid vectors incl. negatives\n");
    {
        /* -1 s32 = 0x7F ; -1 s64 = 0x7F */
        const uint8_t buf[] = { 0x7F };
        const uint8_t *p;
        int32_t v32 = 0; int64_t v64 = 0;
        p = buf; CHECK(leb128_decode_s32(&p, buf + 1, &v32) == WASM_OK && v32 == -1, "s32 -1");
        p = buf; CHECK(leb128_decode_s64(&p, buf + 1, &v64) == WASM_OK && v64 == -1, "s64 -1");
    }
    {
        /* -624485 s32 = 9B F1 E9 7E ... classic vector: 9B F1 59? use encode of -624485. */
        const uint8_t buf[] = { 0x9B, 0xF1, 0x59 };
        const uint8_t *p = buf;
        int32_t v = 0;
        CHECK(leb128_decode_s32(&p, buf + 3, &v) == WASM_OK && v == -624485, "s32 -624485");
    }
    {
        /* INT32_MIN = 80 80 80 80 78 ; INT32_MAX = FF FF FF FF 07 */
        const uint8_t mn[] = { 0x80, 0x80, 0x80, 0x80, 0x78 };
        const uint8_t mx[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x07 };
        const uint8_t *p; int32_t v = 0;
        p = mn; CHECK(leb128_decode_s32(&p, mn + 5, &v) == WASM_OK && v == INT32_MIN, "s32 INT32_MIN");
        p = mx; CHECK(leb128_decode_s32(&p, mx + 5, &v) == WASM_OK && v == INT32_MAX, "s32 INT32_MAX");
    }
    {
        /* INT64_MIN / INT64_MAX 10-byte vectors. */
        const uint8_t mn[] = { 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x7F };
        const uint8_t mx[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
        const uint8_t *p; int64_t v = 0;
        p = mn; CHECK(leb128_decode_s64(&p, mn + 10, &v) == WASM_OK && v == INT64_MIN, "s64 INT64_MIN");
        p = mx; CHECK(leb128_decode_s64(&p, mx + 10, &v) == WASM_OK && v == INT64_MAX, "s64 INT64_MAX");
    }
    {
        /* u64 max vector must decode. */
        const uint8_t mx[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x01 };
        const uint8_t *p = mx; uint64_t v = 0;
        CHECK(leb128_decode_u64(&p, mx + 10, &v) == WASM_OK && v == UINT64_MAX, "u64 UINT64_MAX");
    }
}

/* ---------- Parser tests ---------- */

static void test_valid_dummy_header(void)
{
    printf("[T] parser — valid dummy module (REQUIRED case)\n");
    /*
     * Module: header + type section (id=1, size=4, payload 01 60 00 00:
     *   1 type, func () -> ()) + empty custom section (id=0, size=0).
     */
    static const uint8_t mod[] = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x00, 0x00
    };
    wasm_module_t m;
    const uint8_t *cur;
    wasm_section_header_t s1, s2;
    wasm_err_t rc;

    rc = wasm_parse_header(mod, sizeof(mod), &m);
    CHECK(rc == WASM_OK, "valid header parses");
    CHECK(m.sections_begin == mod + 8 && m.sections_end == mod + sizeof(mod), "cursors = header+8..end");

    cur = m.sections_begin;
    CHECK(wasm_get_section(&cur, m.sections_end, &s1) == WASM_OK, "first section iterates");
    CHECK(s1.id == WASM_SECT_TYPE && s1.size == 4u, "first section id/size");
    CHECK(s1.payload.len == 4u && s1.payload.data == mod + 10, "first payload zero-copy ptr");
    CHECK(s1.payload.data[1] == 0x60, "type payload functype tag 0x60");

    CHECK(wasm_get_section(&cur, m.sections_end, &s2) == WASM_OK, "second section iterates");
    CHECK(s2.id == WASM_SECT_CUSTOM && s2.size == 0u, "empty custom section");
    CHECK(cur == m.sections_end, "cursor at end after 2 sections");

    /* find helper locates the type section again. */
    {
        wasm_section_header_t f;
        CHECK(wasm_find_section(&m, WASM_SECT_TYPE, &f) == WASM_OK && f.size == 4u, "find type section");
        CHECK(wasm_find_section(&m, WASM_SECT_CODE, &f) == WASM_ERR_TRUNCATED, "find missing section = miss");
    }
}

static void test_corrupt_magic(void)
{
    printf("[T] parser — corrupt magic bytes (REQUIRED case)\n");
    static const uint8_t good[] = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
    uint8_t bad[8];
    wasm_module_t m;
    size_t i;
    for (i = 0; i < 8; i++) { bad[i] = good[i]; }
    CHECK(wasm_parse_header(good, sizeof(good), &m) == WASM_OK, "control: good magic OK");
    bad[0] = 0xFF;
    CHECK(wasm_parse_header(bad, sizeof(bad), &m) == WASM_ERR_MAGIC, "bad magic[0] rejected");
    for (i = 0; i < 8; i++) { bad[i] = good[i]; }
    bad[1] = 'X';
    CHECK(wasm_parse_header(bad, sizeof(bad), &m) == WASM_ERR_MAGIC, "bad magic[1] rejected");
    for (i = 0; i < 8; i++) { bad[i] = good[i]; }
    bad[3] = 'x';
    CHECK(wasm_parse_header(bad, sizeof(bad), &m) == WASM_ERR_MAGIC, "bad magic[3] rejected");
}

static void test_bad_version_and_short(void)
{
    printf("[T] parser — bad version / short buffer\n");
    {
        static const uint8_t mod[] = { 0x00, 0x61, 0x73, 0x6D, 0x02, 0x00, 0x00, 0x00 };
        wasm_module_t m;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_ERR_VERSION, "version 2 rejected");
    }
    {
        static const uint8_t mod[] = { 0x00, 0x61, 0x73 };
        wasm_module_t m;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_ERR_TOO_SHORT, "3-byte buffer too short");
    }
    {
        wasm_module_t m;
        CHECK(wasm_parse_header(NULL, 8, &m) == WASM_ERR_NULL, "NULL data rejected");
        CHECK(wasm_parse_header((const uint8_t *)"", 0, &m) == WASM_ERR_TOO_SHORT, "empty buffer too short");
    }
}

static void test_section_payload_oob(void)
{
    printf("[T] parser — out-of-bounds section payload (REQUIRED case)\n");
    {
        /* Claims type section size=4 but only 1 payload byte present. */
        static const uint8_t mod[] = {
            0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x04, 0x01
        };
        wasm_module_t m;
        const uint8_t *cur;
        wasm_section_header_t s;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_OK, "header OK before OOB check");
        cur = m.sections_begin;
        CHECK(wasm_get_section(&cur, m.sections_end, &s) == WASM_ERR_SECTION_OVERFLOW, "short payload -> SECTION_OVERFLOW");
    }
    {
        /* Absurd size (ULEB 0xFF..0F = UINT32_MAX) with tiny buffer. */
        static const uint8_t mod[] = {
            0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
            0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F
        };
        wasm_module_t m;
        const uint8_t *cur;
        wasm_section_header_t s;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_OK, "header OK before huge-size check");
        cur = m.sections_begin;
        CHECK(wasm_get_section(&cur, m.sections_end, &s) == WASM_ERR_SECTION_OVERFLOW, "huge size -> SECTION_OVERFLOW");
    }
}

static void test_section_header_edge_cases(void)
{
    printf("[T] parser — truncated section headers / bad ids\n");
    {
        /* Ends right after section id: size LEB missing. */
        static const uint8_t mod[] = {
            0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x01
        };
        wasm_module_t m;
        const uint8_t *cur;
        wasm_section_header_t s;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_OK, "header OK");
        cur = m.sections_begin;
        CHECK(wasm_get_section(&cur, m.sections_end, &s) == WASM_ERR_LEB_TRUNCATED, "missing size LEB -> LEB_TRUNCATED");
    }
    {
        /* Size LEB itself truncated (0x80 with no terminator). */
        static const uint8_t mod[] = {
            0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x01, 0x80
        };
        wasm_module_t m;
        const uint8_t *cur;
        wasm_section_header_t s;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_OK, "header OK");
        cur = m.sections_begin;
        CHECK(wasm_get_section(&cur, m.sections_end, &s) == WASM_ERR_LEB_TRUNCATED, "cut size LEB -> LEB_TRUNCATED");
    }
    {
        /* Section id 13 is out of range (max 12). */
        static const uint8_t mod[] = {
            0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x0D, 0x00
        };
        wasm_module_t m;
        const uint8_t *cur;
        wasm_section_header_t s;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_OK, "header OK");
        cur = m.sections_begin;
        CHECK(wasm_get_section(&cur, m.sections_end, &s) == WASM_ERR_INVALID_SECTION, "id 13 -> INVALID_SECTION");
    }
    {
        /* Header-only module: zero sections is legal; iterator reports TRUNCATED. */
        static const uint8_t mod[] = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
        wasm_module_t m;
        const uint8_t *cur;
        wasm_section_header_t s;
        CHECK(wasm_parse_header(mod, sizeof(mod), &m) == WASM_OK, "bare header OK");
        cur = m.sections_begin;
        CHECK(wasm_get_section(&cur, m.sections_end, &s) == WASM_ERR_TRUNCATED, "no sections -> TRUNCATED");
    }
}

static void test_struct_sizes(void)
{
    printf("[T] footprint — struct sizes\n");
    printf("  sizeof(wasm_slice_t)=%u sizeof(wasm_section_header_t)=%u sizeof(wasm_module_t)=%u\n",
           (unsigned)sizeof(wasm_slice_t),
           (unsigned)sizeof(wasm_section_header_t),
           (unsigned)sizeof(wasm_module_t));
    CHECK(sizeof(wasm_slice_t) == sizeof(void *) + sizeof(size_t), "slice = ptr + len, no padding surprises");
}

int main(void)
{
    printf("=== Phase 1 tests: LEB128 + header parser ===\n");
    test_leb_u32_valid();
    test_leb_truncated();
    test_leb_overflow();
    test_leb_signed_vectors();
    test_valid_dummy_header();
    test_corrupt_magic();
    test_bad_version_and_short();
    test_section_payload_oob();
    test_section_header_edge_cases();
    test_struct_sizes();

    printf("--------------------------------------------\n");
    printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("STATUS: FAIL\n");
        return 1;
    }
    printf("STATUS: ALL TESTS PASSED\n");
    return 0;
}

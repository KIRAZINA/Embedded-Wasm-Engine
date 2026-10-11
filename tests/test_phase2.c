/*
 * test_phase2.c — Phase 2 verification: operand stack + linear memory + traps.
 * Standalone, C11, assert.h/stdio.h only (+ string.h for bit-exact float compare).
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -Iinclude src/leb128.c
 *        src/wasm_parser.c src/wasm_stack.c src/wasm_memory.c
 *        tests/test_phase2.c -o build/test_phase2
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasm_memory.h"
#include "wasm_stack.h"
#include "wasm_types.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

/* Statically allocated (never malloc): 16 KiB stack + arenas. */
static wasm_stack_t g_stack;
static uint8_t g_arena_1page[65536];
static uint8_t g_arena_3page[3u * 65536u];

static wasm_val_t mk_i32(int32_t v) { wasm_val_t x; x.type = WASM_TYPE_I32; x.value.i32 = v; return x; }
static wasm_val_t mk_i64(int64_t v) { wasm_val_t x; x.type = WASM_TYPE_I64; x.value.i64 = v; return x; }
static wasm_val_t mk_f32(float v)   { wasm_val_t x; x.type = WASM_TYPE_F32; x.value.f32 = v; return x; }
static wasm_val_t mk_f64(double v)  { wasm_val_t x; x.type = WASM_TYPE_F64; x.value.f64 = v; return x; }

/* ---------- a) push/pop/peek across all four types ---------- */

static void test_stack_basic_types(void)
{
    wasm_val_t out;
    printf("[T] stack push/pop/peek across i32/i64/f32/f64\n");
    CHECK(wasm_stack_init(&g_stack) == WASM_TRAP_OK, "stack init OK");
    CHECK(wasm_stack_is_empty(&g_stack) == 1, "stack starts empty");

    CHECK(wasm_stack_push(&g_stack, mk_i32(-12345)) == WASM_TRAP_OK, "push i32");
    CHECK(wasm_stack_push(&g_stack, mk_i64((int64_t)0x1122334455667788LL)) == WASM_TRAP_OK, "push i64");
    CHECK(wasm_stack_push(&g_stack, mk_f32(1.5f)) == WASM_TRAP_OK, "push f32");
    CHECK(wasm_stack_push(&g_stack, mk_f64(-2.25)) == WASM_TRAP_OK, "push f64");
    CHECK(wasm_stack_depth(&g_stack) == 4u, "depth == 4");

    /* Peek sees the top without consuming. */
    CHECK(wasm_stack_peek(&g_stack, &out) == WASM_TRAP_OK, "peek OK");
    CHECK(out.type == WASM_TYPE_F64 && out.value.f64 == -2.25, "peek sees f64 top");
    CHECK(wasm_stack_depth(&g_stack) == 4u, "peek does not consume");

    /* Typed pops in LIFO order, bit-exact. */
    CHECK(wasm_stack_pop_typed(&g_stack, WASM_TYPE_F64, &out) == WASM_TRAP_OK &&
          out.value.f64 == -2.25, "pop_typed f64");
    {
        uint32_t got, want = 0x3FC00000u; /* 1.5f LE bits */
        CHECK(wasm_stack_pop_typed(&g_stack, WASM_TYPE_F32, &out) == WASM_TRAP_OK, "pop_typed f32");
        memcpy(&got, &out.value.f32, 4u);
        CHECK(got == want, "f32 bit pattern 0x3FC00000 preserved");
    }
    CHECK(wasm_stack_pop_typed(&g_stack, WASM_TYPE_I64, &out) == WASM_TRAP_OK &&
          out.value.i64 == (int64_t)0x1122334455667788LL, "pop_typed i64");
    CHECK(wasm_stack_pop_typed(&g_stack, WASM_TYPE_I32, &out) == WASM_TRAP_OK &&
          out.value.i32 == -12345, "pop_typed i32");
    CHECK(wasm_stack_is_empty(&g_stack) == 1, "stack empty after 4 pops");

    /* Untyped pop works too. */
    CHECK(wasm_stack_push(&g_stack, mk_i32(42)) == WASM_TRAP_OK, "repush i32 42");
    CHECK(wasm_stack_pop(&g_stack, &out) == WASM_TRAP_OK &&
          out.type == WASM_TYPE_I32 && out.value.i32 == 42, "untyped pop i32 42");
}

/* ---------- b) overflow / underflow / mismatch traps ---------- */

static void test_stack_traps(void)
{
    wasm_val_t out;
    uint32_t i;
    printf("[T] stack overflow / underflow traps\n");
    CHECK(wasm_stack_init(&g_stack) == WASM_TRAP_OK, "re-init for trap test");

    for (i = 0u; i < (uint32_t)WASM_STACK_MAX_DEPTH; i++) {
        if (wasm_stack_push(&g_stack, mk_i32((int32_t)i)) != WASM_TRAP_OK) {
            break;
        }
    }
    CHECK(i == (uint32_t)WASM_STACK_MAX_DEPTH, "filled exactly to MAX_DEPTH");
    CHECK(wasm_stack_is_full(&g_stack) == 1, "is_full reports true");
    CHECK(wasm_stack_push(&g_stack, mk_i32(999)) == WASM_TRAP_STACK_OVERFLOW, "push past top -> OVERFLOW");
    CHECK(wasm_stack_depth(&g_stack) == (uint32_t)WASM_STACK_MAX_DEPTH, "depth pinned at max after overflow");

    /* Type mismatch must NOT consume the top element. */
    CHECK(wasm_stack_pop_typed(&g_stack, WASM_TYPE_F64, &out) == WASM_TRAP_TYPE_MISMATCH, "wrong-type pop -> MISMATCH");
    CHECK(wasm_stack_depth(&g_stack) == (uint32_t)WASM_STACK_MAX_DEPTH, "mismatch leaves stack untouched");

    /* Drain fully, then underflow. */
    for (i = 0u; i < (uint32_t)WASM_STACK_MAX_DEPTH; i++) {
        if (wasm_stack_pop(&g_stack, &out) != WASM_TRAP_OK) {
            break;
        }
    }
    CHECK(i == (uint32_t)WASM_STACK_MAX_DEPTH, "drained all entries");
    CHECK(wasm_stack_pop(&g_stack, &out) == WASM_TRAP_STACK_UNDERFLOW, "pop on empty -> UNDERFLOW");
    CHECK(wasm_stack_peek(&g_stack, &out) == WASM_TRAP_STACK_UNDERFLOW, "peek on empty -> UNDERFLOW");
    CHECK(wasm_stack_pop_typed(&g_stack, WASM_TYPE_I32, &out) == WASM_TRAP_STACK_UNDERFLOW, "typed pop on empty -> UNDERFLOW");

    /* NULL guards. */
    CHECK(wasm_stack_init(NULL) == WASM_TRAP_NULL, "init NULL -> NULL trap");
    CHECK(wasm_stack_push(NULL, mk_i32(1)) == WASM_TRAP_NULL, "push NULL -> NULL trap");
    CHECK(wasm_stack_pop(NULL, &out) == WASM_TRAP_NULL, "pop NULL stack -> NULL trap");
    CHECK(wasm_stack_pop(&g_stack, NULL) == WASM_TRAP_NULL, "pop NULL out -> NULL trap");
    CHECK(wasm_stack_peek(NULL, &out) == WASM_TRAP_NULL, "peek NULL -> NULL trap");
}

/* ---------- c) LE accuracy + unaligned-safe access ---------- */

static void test_memory_le_accuracy(void)
{
    wasm_memory_t m;
    int32_t ri32; int64_t ri64; float rf32; double rf64; uint8_t ru8;
    uint32_t ubits; uint64_t ubits64;
    printf("[T] memory read/write accuracy + LE verification (unaligned offsets)\n");
    memset(g_arena_1page, 0xAA, sizeof(g_arena_1page)); /* dirty fill: init must zero it */
    CHECK(wasm_memory_init(&m, g_arena_1page, (uint32_t)sizeof(g_arena_1page), 1u, 1u) == WASM_TRAP_OK, "init 1 page");
    CHECK(g_arena_1page[0] == 0u && g_arena_1page[65535] == 0u, "init zero-fills visible window");
    CHECK(wasm_memory_size_bytes(&m) == 65536u, "size == 1 page");

    /* i32 at odd (unaligned) offset 1: bytes must appear LE. */
    CHECK(wasm_memory_store_i32(&m, 1u, (int32_t)0x01020304) == WASM_TRAP_OK, "store i32 @1 (unaligned)");
    CHECK(g_arena_1page[1] == 0x04u && g_arena_1page[2] == 0x03u &&
          g_arena_1page[3] == 0x02u && g_arena_1page[4] == 0x01u, "i32 LE byte order 04 03 02 01");
    CHECK(wasm_memory_load_i32(&m, 1u, &ri32) == WASM_TRAP_OK && ri32 == (int32_t)0x01020304, "load i32 @1 roundtrip");

    /* i64 at odd offset 3. */
    CHECK(wasm_memory_store_i64(&m, 3u, (int64_t)0x0102030405060708LL) == WASM_TRAP_OK, "store i64 @3 (unaligned)");
    CHECK(g_arena_1page[3] == 0x08u && g_arena_1page[10] == 0x01u, "i64 LE endpoints 08..01");
    CHECK(wasm_memory_load_i64(&m, 3u, &ri64) == WASM_TRAP_OK &&
          ri64 == (int64_t)0x0102030405060708LL, "load i64 @3 roundtrip");

    /* f32 1.0f == 0x3F800000 -> LE bytes 00 00 80 3F at offset 5. */
    CHECK(wasm_memory_store_f32(&m, 5u, 1.0f) == WASM_TRAP_OK, "store f32 1.0 @5");
    CHECK(g_arena_1page[5] == 0x00u && g_arena_1page[6] == 0x00u &&
          g_arena_1page[7] == 0x80u && g_arena_1page[8] == 0x3Fu, "f32 LE bytes 00 00 80 3F");
    CHECK(wasm_memory_load_f32(&m, 5u, &rf32) == WASM_TRAP_OK, "load f32 @5");
    memcpy(&ubits, &rf32, 4u);
    CHECK(ubits == 0x3F800000u, "f32 bit-exact 0x3F800000 (memcpy, no aliasing)");

    /* f64 roundtrip incl. negative value at offset 7 (unaligned). */
    CHECK(wasm_memory_store_f64(&m, 7u, -123.456) == WASM_TRAP_OK, "store f64 @7");
    CHECK(wasm_memory_load_f64(&m, 7u, &rf64) == WASM_TRAP_OK && rf64 == -123.456, "load f64 @7 roundtrip");
    memcpy(&ubits64, &rf64, 8u);
    {
        double ref = -123.456; uint64_t want;
        memcpy(&want, &ref, 8u);
        CHECK(ubits64 == want, "f64 bit-exact via memcpy");
    }

    /* u8 smoke. */
    CHECK(wasm_memory_store_u8(&m, 100u, 0x5Au) == WASM_TRAP_OK, "store u8");
    CHECK(wasm_memory_load_u8(&m, 100u, &ru8) == WASM_TRAP_OK && ru8 == 0x5Au, "load u8");
}

/* ---------- d) boundary OOB ---------- */

static void test_memory_oob_boundaries(void)
{
    wasm_memory_t m;
    int32_t ri32; int64_t ri64; float rf32; double rf64; uint8_t ru8;
    const uint32_t cap = 65536u;
    printf("[T] memory out-of-bounds at exact boundaries\n");
    CHECK(wasm_memory_init(&m, g_arena_1page, (uint32_t)sizeof(g_arena_1page), 1u, 1u) == WASM_TRAP_OK, "init 1 page");

    /* Last-byte-legal accesses. */
    CHECK(wasm_memory_load_u8(&m, cap - 1u, &ru8) == WASM_TRAP_OK, "u8 @cap-1 legal");
    CHECK(wasm_memory_load_i32(&m, cap - 4u, &ri32) == WASM_TRAP_OK, "i32 @cap-4 legal (last full word)");
    CHECK(wasm_memory_load_i64(&m, cap - 8u, &ri64) == WASM_TRAP_OK, "i64 @cap-8 legal");
    CHECK(wasm_memory_load_f32(&m, cap - 4u, &rf32) == WASM_TRAP_OK, "f32 @cap-4 legal");
    CHECK(wasm_memory_load_f64(&m, cap - 8u, &rf64) == WASM_TRAP_OK, "f64 @cap-8 legal");

    /* One byte past -> trap (the REQUIRED offset=capacity-2 i32 case included). */
    CHECK(wasm_memory_load_u8(&m, cap, &ru8) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "u8 @cap -> OOB");
    CHECK(wasm_memory_load_i32(&m, cap - 2u, &ri32) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "i32 @cap-2 -> OOB (needs 4, has 2)");
    CHECK(wasm_memory_load_i32(&m, cap - 3u, &ri32) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "i32 @cap-3 -> OOB");
    CHECK(wasm_memory_store_i32(&m, cap - 2u, 1) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "store i32 @cap-2 -> OOB");
    CHECK(wasm_memory_load_i64(&m, cap - 7u, &ri64) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "i64 @cap-7 -> OOB");
    CHECK(wasm_memory_store_i64(&m, cap - 1u, 1) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "store i64 @cap-1 -> OOB");
    CHECK(wasm_memory_load_f32(&m, cap - 2u, &rf32) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "f32 @cap-2 -> OOB");
    CHECK(wasm_memory_load_f64(&m, cap - 7u, &rf64) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "f64 @cap-7 -> OOB");
    CHECK(wasm_memory_store_u8(&m, cap, 0xFFu) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "store u8 @cap -> OOB");
}

/* ---------- e) integer-wraparound attempts ---------- */

static void test_memory_wraparound(void)
{
    wasm_memory_t m;
    int32_t ri32; int64_t ri64; float rf32; double rf64; uint8_t ru8;
    printf("[T] wraparound offsets (UINT32_MAX edges) must trap, never wrap-accept\n");
    CHECK(wasm_memory_init(&m, g_arena_1page, (uint32_t)sizeof(g_arena_1page), 1u, 1u) == WASM_TRAP_OK, "init 1 page");

    /* Naive `offset + size <= total` would wrap these to small values; ours must trap. */
    CHECK(wasm_memory_load_u8(&m, 0xFFFFFFFFu, &ru8) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "u8 @UINT32_MAX -> OOB");
    CHECK(wasm_memory_load_i32(&m, 0xFFFFFFFEu, &ri32) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "i32 @MAX-1 -> OOB (wraps to 2 if naive)");
    CHECK(wasm_memory_load_i32(&m, 0xFFFFFFFCu, &ri32) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "i32 @MAX-3 -> OOB");
    CHECK(wasm_memory_store_i32(&m, 0xFFFFFFFFu, 1) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "store i32 @MAX -> OOB");
    CHECK(wasm_memory_load_i64(&m, 0xFFFFFFF8u, &ri64) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "i64 @MAX-7 -> OOB");
    CHECK(wasm_memory_load_f32(&m, 0xFFFFFFFEu, &rf32) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "f32 @MAX-1 -> OOB");
    CHECK(wasm_memory_load_f64(&m, 0xFFFFFFF8u, &rf64) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "f64 @MAX-7 -> OOB");
    CHECK(wasm_memory_store_f64(&m, 0xFFFFFFFFu, 1.0) == WASM_TRAP_MEMORY_OUT_OF_BOUNDS, "store f64 @MAX -> OOB");
}

/* ---------- f) grow + zero-init ---------- */

static void test_memory_grow(void)
{
    wasm_memory_t m;
    uint32_t prev = 0u;
    int32_t rv;
    uint32_t i;
    printf("[T] memory growth + zero-initialized pages\n");
    memset(g_arena_3page, 0xAA, sizeof(g_arena_3page)); /* hostile fill */
    CHECK(wasm_memory_init(&m, g_arena_3page, (uint32_t)sizeof(g_arena_3page), 1u, 3u) == WASM_TRAP_OK, "init 1/3 pages");
    CHECK(g_arena_3page[0] == 0u && g_arena_3page[65535] == 0u, "init zeroes page 0");

    /* Sentinel at end of page 0 must survive growth. */
    CHECK(wasm_memory_store_i32(&m, 65532u, (int32_t)0xDEADBEEF) == WASM_TRAP_OK, "sentinel store @cap-4");
    CHECK(wasm_mem_grow(&m, 1u, &prev) == WASM_TRAP_OK && prev == 1u, "grow +1 returns prev=1");
    CHECK(wasm_memory_pages(&m) == 2u, "pages == 2 after grow");
    CHECK(wasm_memory_size_bytes(&m) == 131072u, "size == 2 pages");
    CHECK(wasm_memory_load_i32(&m, 65532u, &rv) == WASM_TRAP_OK && rv == (int32_t)0xDEADBEEF, "sentinel intact after grow");
    /* New tail zero-filled: spot-check head/mid/tail of page 1. */
    CHECK(g_arena_3page[65536] == 0u && g_arena_3page[70000] == 0u && g_arena_3page[131071] == 0u, "new page zero-filled");
    for (i = 65536u; i < 65552u; i++) {
        if (g_arena_3page[i] != 0u) {
            break;
        }
    }
    CHECK(i == 65552u, "first 16 bytes of grown page are zero");

    /* New page is readable/writable. */
    CHECK(wasm_memory_store_i32(&m, 100000u, 777) == WASM_TRAP_OK, "store in grown page");
    CHECK(wasm_memory_load_i32(&m, 100000u, &rv) == WASM_TRAP_OK && rv == 777, "load from grown page");

    /* delta 0 is a no-op query. */
    CHECK(wasm_mem_grow(&m, 0u, &prev) == WASM_TRAP_OK && prev == 2u, "grow 0 returns prev=2");
    CHECK(wasm_memory_pages(&m) == 2u, "pages unchanged after grow 0");

    /* Past max -> GROW_FAILED, state untouched. */
    CHECK(wasm_mem_grow(&m, 2u, &prev) == WASM_TRAP_MEMORY_GROW_FAILED, "grow +2 past max=3 -> GROW_FAILED");
    CHECK(wasm_memory_pages(&m) == 2u, "pages unchanged after failed grow");

    /* Past backing capacity -> GROW_FAILED (2-page backing, max 3). */
    {
        static uint8_t small[2u * 65536u];
        wasm_memory_t m2;
        memset(small, 0xAA, sizeof(small));
        CHECK(wasm_memory_init(&m2, small, (uint32_t)sizeof(small), 2u, 3u) == WASM_TRAP_OK, "init 2/3 on 2-page backing");
        CHECK(wasm_mem_grow(&m2, 1u, NULL) == WASM_TRAP_MEMORY_GROW_FAILED, "grow past backing capacity -> GROW_FAILED");
        CHECK(wasm_memory_pages(&m2) == 2u, "pages unchanged after capacity failure");
    }

    /* Bad init args rejected. */
    {
        wasm_memory_t bad;
        CHECK(wasm_memory_init(&bad, g_arena_3page, (uint32_t)sizeof(g_arena_3page), 2u, 1u) ==
              WASM_TRAP_INVALID_ARGUMENT, "init initial>max rejected");
        CHECK(wasm_memory_init(&bad, g_arena_1page, (uint32_t)sizeof(g_arena_1page), 2u, 3u) ==
              WASM_TRAP_INVALID_ARGUMENT, "init need>backing rejected");
        CHECK(wasm_memory_init(NULL, g_arena_1page, 65536u, 1u, 1u) == WASM_TRAP_NULL, "init NULL mem rejected");
    }
}

static void test_footprint(void)
{
    printf("[T] footprint — struct sizes\n");
    printf("  WASM_PAGE_SIZE=%u WASM_STACK_MAX_DEPTH=%u\n",
           (unsigned)WASM_PAGE_SIZE, (unsigned)WASM_STACK_MAX_DEPTH);
    printf("  sizeof(wasm_val_t)=%u sizeof(wasm_stack_t)=%u sizeof(wasm_memory_t)=%u\n",
           (unsigned)sizeof(wasm_val_t),
           (unsigned)sizeof(wasm_stack_t),
           (unsigned)sizeof(wasm_memory_t));
    CHECK(sizeof(wasm_val_t) == 16u, "wasm_val_t == 16 (tag + 8B union, LP64)");
    CHECK(sizeof(wasm_stack_t) == 16u * (unsigned)WASM_STACK_MAX_DEPTH + 4u ||
          sizeof(wasm_stack_t) == 16u * (unsigned)WASM_STACK_MAX_DEPTH + 8u, "stack ~= 16B*depth + top");
    CHECK(sizeof(wasm_memory_t) <= 24u, "memory descriptor <= 24 (borrowed arena, no inline pages)");
}

int main(void)
{
    printf("=== Phase 2 tests: stack + linear memory + traps ===\n");
    test_stack_basic_types();
    test_stack_traps();
    test_memory_le_accuracy();
    test_memory_oob_boundaries();
    test_memory_wraparound();
    test_memory_grow();
    test_footprint();
    printf("--------------------------------------------\n");
    printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("STATUS: FAIL\n");
        return 1;
    }
    printf("STATUS: ALL TESTS PASSED\n");
    return 0;
}

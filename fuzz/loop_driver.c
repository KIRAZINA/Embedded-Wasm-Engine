/*
 * loop_driver.c — deterministic, seeded mutation driver (no malloc).
 *
 *   loop_driver SEED ITERATIONS [seed-module.wasm ...]
 *
 * Fallback backend for hosts without libFuzzer or AFL++: a reproducible
 * random mutation loop over a corpus of VALID modules. The seed is
 * printed so any run can be replayed exactly.
 *
 * Mutations (all in-memory, all bounded by static buffers):
 *   - bit flips            (1..8 random bits in the candidate)
 *   - truncation           (keep the 8-byte header, drop the tail)
 *   - section-size corruption (rewrite a section's size LEB in place)
 *   - random byte overwrite
 *   - header corruption    (magic/version bytes)
 *
 * Every mutant is fed to the shared pipeline
 * (parse -> section walk -> validate -> instantiate -> execute).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_pipeline.h"

#define MAX_SEEDS     32u
#define MUTATIONS_MAX 3u

static uint8_t g_seed_buf[MAX_SEEDS][FUZZ_MAX_MODULE_BYTES];
static size_t g_seed_len[MAX_SEEDS];
static uint32_t g_nseeds;
static uint8_t g_scratch[FUZZ_MAX_MODULE_BYTES];

/* Minimal valid module used when no seed file is supplied:
 * (func (export "main")) */
static const uint8_t k_builtin_seed[] = {
    0x00u, 0x61u, 0x73u, 0x6Du, 0x01u, 0x00u, 0x00u, 0x00u,
    0x01u, 0x04u, 0x01u, 0x60u, 0x00u, 0x00u,
    0x03u, 0x02u, 0x01u, 0x00u,
    0x07u, 0x08u, 0x01u, 0x04u, 0x6Du, 0x61u, 0x69u, 0x6Eu,
    0x00u, 0x00u,
    0x0Au, 0x04u, 0x01u, 0x02u, 0x00u, 0x0Bu
};

/* ---- xorshift64* PRNG (deterministic, seed printed by main) ---- */

static uint64_t g_state;

static uint64_t rnd(void)
{
    uint64_t x = g_state;
    x ^= x >> 12u;
    x ^= x << 25u;
    x ^= x >> 27u;
    g_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ---- mutations ---- */

static void mut_bitflip(uint8_t *b, size_t n)
{
    uint32_t k = 1u + (uint32_t)(rnd() % 8u);
    uint32_t i;
    if (n == 0u) {
        return;
    }
    for (i = 0u; i < k; i++) {
        size_t pos = (size_t)(rnd() % (uint64_t)n);
        b[pos] ^= (uint8_t)(1u << (rnd() & 7u));
    }
}

static size_t mut_truncate(uint8_t *b, size_t n)
{
    size_t keep;
    (void)b;
    if (n <= 9u) {
        return n;
    }
    keep = 8u + (size_t)(rnd() % (uint64_t)(n - 8u));
    return keep;
}

/* Rewrite the size LEB of one section in place, keeping the encoded
 * length so the module layout stays walkable (the size itself lies). */
static void mut_section_size(uint8_t *b, size_t n)
{
    size_t cursor = 8u;
    uint32_t found = 0u;

    while (cursor < n) {
        size_t size_off, i, len = 0u;
        uint64_t value = 0u;
        uint32_t shift = 0u;
        uint8_t byte;

        (void)b[cursor]; /* section id */
        cursor++;
        size_off = cursor;
        while (cursor < n && len < 5u) {
            byte = b[cursor++];
            value |= (uint64_t)(byte & 0x7Fu) << shift;
            shift += 7u;
            len++;
            if ((byte & 0x80u) == 0u) {
                break;
            }
        }
        if (len == 0u) {
            return;
        }
        if (found == 0u || (rnd() & 1u) == 0u) {
            /* corrupt this section's size */
            uint64_t mask = (len >= 5u) ? 0xFFFFFFFFULL
                                         : ((1ULL << (7u * len)) - 1ULL);
            uint64_t newv = rnd() & mask;
            for (i = 0u; i < len; i++) {
                uint8_t out = (uint8_t)(newv & 0x7Fu);
                newv >>= 7u;
                if (i + 1u < len) {
                    out |= 0x80u;
                }
                b[size_off + i] = out;
            }
            found = 1u;
            if ((rnd() & 3u) == 0u) {
                return; /* sometimes stop after one section */
            }
        }
        /* advance past the payload (best effort) */
        cursor = size_off + len + (size_t)value;
        if (cursor <= size_off + len) {
            return; /* overflow / malformed: stop */
        }
    }
}

static void mut_random_byte(uint8_t *b, size_t n)
{
    if (n == 0u) {
        return;
    }
    b[rnd() % (uint64_t)n] = (uint8_t)rnd();
}

static void mut_header(uint8_t *b, size_t n)
{
    static const size_t offs[4] = { 0u, 1u, 2u, 3u };
    size_t i = offs[rnd() & 3u];
    if (n > i) {
        b[i] = (uint8_t)rnd();
    }
}

/* ---- driver ---- */

static int load_seed(const char *path)
{
    FILE *fp;
    size_t n;

    if (g_nseeds >= MAX_SEEDS) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    n = fread(g_seed_buf[g_nseeds], 1u, (size_t)FUZZ_MAX_MODULE_BYTES, fp);
    fclose(fp);
    if (n == 0u) {
        return 0;
    }
    g_seed_len[g_nseeds] = n;
    g_nseeds++;
    return 1;
}

int main(int argc, char **argv)
{
    unsigned long long seed = 0ull;
    unsigned long iters = 0ull, i;
    unsigned long executed = 0ull, staged = 0ull;
    int argi;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s SEED ITERATIONS [seed-module.wasm ...]\n",
                argv[0]);
        return 2;
    }
    seed = strtoull(argv[1], NULL, 0);
    iters = strtoul(argv[2], NULL, 0);
    g_state = (seed != 0ull) ? (uint64_t)seed : 0x9E3779B97F4A7C15ULL;

    for (argi = 3; argi < argc; argi++) {
        (void)load_seed(argv[argi]);
    }
    if (g_nseeds == 0u) {
        memcpy(g_seed_buf[0], k_builtin_seed, sizeof(k_builtin_seed));
        g_seed_len[0] = sizeof(k_builtin_seed);
        g_nseeds = 1u;
    }

    for (i = 0ull; i < iters; i++) {
        uint32_t which = (uint32_t)(rnd() % (uint64_t)g_nseeds);
        size_t len = g_seed_len[which];
        uint32_t nm, k;

        memcpy(g_scratch, g_seed_buf[which], len);
        nm = 1u + (uint32_t)(rnd() % MUTATIONS_MAX);
        for (k = 0u; k < nm; k++) {
            switch (rnd() % 5u) {
            case 0u:
                mut_bitflip(g_scratch, len);
                break;
            case 1u:
                len = mut_truncate(g_scratch, len);
                break;
            case 2u:
                mut_section_size(g_scratch, len);
                break;
            case 3u:
                mut_random_byte(g_scratch, len);
                break;
            default:
                mut_header(g_scratch, len);
                break;
            }
        }
        staged += (unsigned long)wasm_fuzz_pipeline(g_scratch, len);
        executed++;
    }

    printf("FUZZ-LOOP seed=0x%016llx iterations=%lu executed=%lu stages=%lu\n",
           seed, iters, executed, staged);
    return 0;
}

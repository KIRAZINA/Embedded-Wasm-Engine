/*
 * test_cli.c — CLI runner smoke tests (C11, assert-style + stdio only).
 *
 * Covers: end-to-end `wasm_runner` execution of hand-assembled () -> (i64)
 * modules, asserting the exact decimal rendering of i64 results, including
 * INT64_MIN (10-byte SLEB128 immediate, negation-overflow edge in the
 * decimal renderer) and -1 (sign path).
 *
 * Module layout mirrors demo/add.wasm conventions, in particular: the code
 * body carries NO trailing 0x0B end byte, matching what the engine executes
 * (a top-level OP_END would pop an empty control stack and trap).
 *
 * Layout per module:
 *   header(8) | type(01 05 01 60 00 01 7E) | func(03 02 01 00)
 *   | export(07 08 01 04 "main" 00 00) | code(0A .. 01 <body_size> 00 42 <sleb>)
 *
 * Build: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude \
 *            tests/test_cli.c -o build/test_cli
 * (No library sources needed: the runner binary under test is spawned via
 * system(), exactly like test_phase6.c test_cli_dump.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

#ifdef _WIN32
#define RUNNER "build\\wasm_runner.exe"
#define SEP "\\"
#else
#define RUNNER "./build/wasm_runner"
#define SEP "/"
#endif

/* () -> (i64) returning 42. Body: 00 (no locals) 42 2A (i64.const 42). */
static const unsigned char kMod42[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7E,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x05, 0x01, 0x03, 0x00, 0x42, 0x2A
};

/* () -> (i64) returning -1. Body: 00 42 7F (i64.const -1). */
static const unsigned char kModNeg1[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7E,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x05, 0x01, 0x03, 0x00, 0x42, 0x7F
};

/* () -> (i64) with an undecodable opcode body: validator must reject it,
 * while --no-validate proceeds into a runtime UNKNOWN_OPCODE trap. */
static const unsigned char kModBadOp[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7E,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x04, 0x01, 0x02, 0x00, 0xFF
};

/* () -> (i64) returning INT64_MIN.
 * SLEB128(-2^63) = 9 x 0x80 followed by 0x7F (10 bytes). */
static const unsigned char kModMin[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7E,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x0E, 0x01, 0x0C, 0x00, 0x42,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7F
};

static void run_case(const char *tag, const unsigned char *mod, size_t mod_len,
                     const char *want)
{
    char wasm_path[128];
    char out_path[128];
    char cmd[320];
    char out[1024];
    FILE *fp = NULL;
    size_t n = 0u;
    size_t w = 0u;

    printf("[T] CLI i64 result — %s\n", tag);
    sprintf(wasm_path, "build%scli_i64_%s.wasm", SEP, tag);
    sprintf(out_path, "build%scli_i64_%s.txt", SEP, tag);
    fp = fopen(wasm_path, "wb");
    CHECK(fp != NULL, "wasm file opened for writing");
    if (fp == NULL) {
        return;
    }
    w = fwrite(mod, 1u, mod_len, fp);
    fclose(fp);
    CHECK(w == mod_len, "wasm bytes fully written");

    sprintf(cmd, "%s %s main > %s", RUNNER, wasm_path, out_path);
    CHECK(system(cmd) == 0, "runner exits 0");
    fp = fopen(out_path, "rb");
    CHECK(fp != NULL, "runner output captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    CHECK(strstr(out, want) != NULL, "exact i64 result line printed");
}

static void test_validation(void)
{
    char wasm_path[128];
    char out_path[128];
    char cmd[384];
    char out[1024];
    FILE *fp = NULL;
    size_t n = 0u;
    size_t w = 0u;
    int rc;

    printf("[T] CLI validates by default, --no-validate bypasses\n");
    sprintf(wasm_path, "build%scli_badop.wasm", SEP);
    sprintf(out_path, "build%scli_badop.txt", SEP);
    fp = fopen(wasm_path, "wb");
    CHECK(fp != NULL, "bad-op wasm file opened for writing");
    if (fp == NULL) {
        return;
    }
    w = fwrite(kModBadOp, 1u, sizeof(kModBadOp), fp);
    fclose(fp);
    CHECK(w == sizeof(kModBadOp), "bad-op wasm bytes fully written");

    /* Default path: static validation rejects before linking/running. */
    sprintf(cmd, "%s %s main > %s 2>&1", RUNNER, wasm_path, out_path);
    rc = system(cmd);
    CHECK(rc != 0, "invalid module rejected by default (nonzero exit)");
    fp = fopen(out_path, "rb");
    CHECK(fp != NULL, "rejection output captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    CHECK(strstr(out, "validation:") != NULL, "stderr names validation");
    CHECK(strstr(out, "WASM_ERR_INVALID_SECTION") != NULL,
          "validator error string printed");

    /* Bypass path: validation skipped, the bad opcode traps at runtime. */
    sprintf(cmd, "%s --no-validate %s main > %s 2>&1", RUNNER, wasm_path,
            out_path);
    rc = system(cmd);
    CHECK(rc != 0, "--no-validate still fails, but at runtime");
    fp = fopen(out_path, "rb");
    CHECK(fp != NULL, "trap output captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    CHECK(strstr(out, "validation:") == NULL, "no validation message");
    CHECK(strstr(out, "WASM_TRAP_UNKNOWN_OPCODE") != NULL,
          "runtime trap reported instead");
}

int main(void)
{
    printf("=== CLI tests: runner i64 result printing ===\n");
    run_case("pos42", kMod42, sizeof(kMod42), "result: 42 (i64)");
    run_case("neg1", kModNeg1, sizeof(kModNeg1), "result: -1 (i64)");
    run_case("min", kModMin, sizeof(kModMin),
             "result: -9223372036854775808 (i64)");
    test_validation();
    printf("--------------------------------------------\n");
    printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("STATUS: FAIL\n");
        return 1;
    }
    printf("STATUS: ALL TESTS PASSED\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned pass_count;
static unsigned fail_count;

#define CHECK(c, n) do { if (c) pass_count++; else { printf("FAIL: %s\n", n); fail_count++; } } while (0)

#ifdef _WIN32
#define RUNNER "build\\wasm_runner.exe"
#define REDIR " > build\\phase12_cli.txt 2>&1"
#else
#define RUNNER "./build/wasm_runner"
#define REDIR " > build/phase12_cli.txt 2>&1"
#endif

static int output_contains(const char *want, int exact_one_line)
{
    FILE *fp = fopen("build/phase12_cli.txt", "rb");
    char buf[2048];
    size_t n;
    unsigned lines = 0u;
    if (fp == NULL) return 0;
    n = fread(buf, 1u, sizeof(buf) - 1u, fp);
    fclose(fp);
    buf[n] = '\0';
    for (size_t i = 0u; i < n; i++) if (buf[i] == '\n') lines++;
    return strstr(buf, want) != NULL && (!exact_one_line || lines == 1u);
}

int main(void)
{
    int rc;
    rc = system(RUNNER " --version" REDIR);
    CHECK(rc == 0 && output_contains("1.0.0", 1), "version output");
    rc = system(RUNNER " --quiet --result-line demo/add.wasm main --arg i32:2 --arg i32:3" REDIR);
    CHECK(rc == 0 && output_contains("RESULT i32 5", 1), "typed i32 result line");
    rc = system(RUNNER " --quiet --result-line demo/fadd.wasm main" REDIR);
    CHECK(rc == 0 && output_contains("RESULT f32 0x40800000", 1), "f32 bit result line");
    rc = system(RUNNER " --quiet --result-line demo/hello.wasm main" REDIR);
    CHECK(rc == 0 && output_contains("RESULT void", 1) == 1, "quiet void result line");
    rc = system(RUNNER " --quiet demo/add.wasm main --arg i64:-1 --arg i64:2" REDIR);
    CHECK(rc != 0 && output_contains("type does not match", 0), "typed mismatch rejected");
    rc = system(RUNNER " --quiet demo/add.wasm main --arg f32:0x3f800000 --arg f32:1.0" REDIR);
    CHECK(rc != 0 && output_contains("type does not match", 0), "f32 typed argument parsed");
    rc = system(RUNNER " --quiet demo/add.wasm main --arg f64:0x3ff0000000000000 --arg f64:2e10" REDIR);
    CHECK(rc != 0 && output_contains("type does not match", 0), "f64 typed argument parsed");
    printf("RESULT: %u passed, %u failed\n", pass_count, fail_count);
    printf("STATUS: %s\n", fail_count == 0u ? "ALL TESTS PASSED" : "FAIL");
    return fail_count == 0u ? 0 : 1;
}

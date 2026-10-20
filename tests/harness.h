#ifndef TESTS_HARNESS_H
#define TESTS_HARNESS_H

/*
 * tests/harness.h — the shared test harness.
 *
 * A small harness with no dependencies beyond stdio and no dynamic
 * allocation for the engine's test suites:
 *
 *   - test registration: a table of named test functions,
 *   - pass/fail counters for tests and assertions,
 *   - assert macros that report file:line,
 *   - a non-zero exit status when anything failed.
 *
 * Usage:
 *
 *   #include "harness.h"
 *
 *   static void test_something(void)
 *   {
 *       CHECK(1 + 1 == 2, "arithmetic works");
 *   }
 *
 *   int main(void)
 *   {
 *       static const harness_test_t tests[] = {
 *           { "something", test_something },
 *       };
 *       return harness_run("my_suite", tests,
 *                          sizeof(tests) / sizeof(tests[0]));
 *   }
 *
 * Output: one line per test plus a summary. Failing assertions are
 * printed immediately with their file:line; passing assertions are only
 * printed when HARNESS_VERBOSE=1 is set, so `make test` stays readable.
 * The summary keeps the "RESULT: <n> passed, <n> failed" line the
 * Makefile parses.
 */

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*harness_test_fn)(void);

typedef struct {
    const char *name;
    harness_test_fn fn;
} harness_test_t;

typedef struct {
    const char *suite;
    unsigned tests;
    unsigned assertions;
    unsigned passed;
    unsigned failed;
    unsigned current_failed;
} harness_state_t;

static harness_state_t g_harness;

static inline int harness_verbose(void)
{
    const char *v = getenv("HARNESS_VERBOSE");
    return (v != NULL && v[0] != '\0' && v[0] != '0');
}

/* Record one assertion. `ok` is the truth value, `name` the label. */
static inline void harness_record(int ok, const char *name,
                                   const char *file, int line)
{
    g_harness.assertions++;
    if (ok) {
        g_harness.passed++;
        if (harness_verbose()) {
            printf("  [PASS] %s\n", name);
        }
    } else {
        g_harness.failed++;
        g_harness.current_failed++;
        printf("  [FAIL] %s (%s:%d)\n", name, file, line);
    }
}

/* CHECK(cond, "name"): record the assertion, keep going. */
#define CHECK(cond, name) \
    harness_record((cond) ? 1 : 0, (name), __FILE__, __LINE__)

/* REQUIRE(cond, "name"): record the assertion and leave the current
 * test when it fails (the remaining checks of that test are skipped). */
#define REQUIRE(cond, name) do { \
    harness_record((cond) ? 1 : 0, (name), __FILE__, __LINE__); \
    if (!(cond)) { \
        return; \
    } \
} while (0)

/* CHECK_EQ(a, b, "name"): compare two integers. */
#define CHECK_EQ(actual, expected, name) \
    harness_record((long)(actual) == (long)(expected) ? 1 : 0, (name), \
                    __FILE__, __LINE__)

/* Run the registered tests and print the summary.
 * Returns 0 when every assertion passed, 1 otherwise. */
static inline int harness_run(const char *suite, const harness_test_t *tests,
                               size_t count)
{
    size_t i;
    g_harness.suite = suite;
    g_harness.tests = 0u;
    g_harness.assertions = 0u;
    g_harness.passed = 0u;
    g_harness.failed = 0u;
    for (i = 0u; i < count; i++) {
        g_harness.current_failed = 0u;
        g_harness.tests++;
        tests[i].fn();
        if (g_harness.current_failed == 0u) {
            printf("  [ ok ] %s\n", tests[i].name);
        } else {
            printf("  [FAIL] %s (%u assertion(s))\n", tests[i].name,
                   g_harness.current_failed);
        }
    }
    printf("--------------------------------------------\n");
    printf("%s: %u tests, %u assertions, %u passed, %u failed\n", suite,
           g_harness.tests, g_harness.assertions, g_harness.passed,
           g_harness.failed);
    printf("RESULT: %u passed, %u failed\n", g_harness.passed,
           g_harness.failed);
    printf("STATUS: %s\n", g_harness.failed == 0u ? "ALL TESTS PASSED"
                                                : "FAIL");
    return g_harness.failed == 0u ? 0 : 1;
}

#ifdef __cplusplus
}
#endif

#endif /* TESTS_HARNESS_H */

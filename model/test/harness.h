/* harness.h - a minimal in-tree test harness for the host model (plan §8.1).
 *
 * A test is a function `static void name(void)` registered with JT_TEST(name).
 * JT_CHECK records a failure and continues; JT_REQUIRE stops the test.
 * Registration uses a constructor so test files need no central list. */
#ifndef JOIST_TEST_HARNESS_H
#define JOIST_TEST_HARNESS_H

#include <stdio.h>

typedef void (*jt_fn)(void);

void jt_register(const char *name, jt_fn fn);
void jt_fail(const char *file, int line, const char *expr, int fatal);

#define JT_TEST(name)                                                                              \
    static void name(void);                                                                        \
    __attribute__((constructor)) static void jt_reg_##name(void)                                   \
    {                                                                                              \
        jt_register(#name, name);                                                                  \
    }                                                                                              \
    static void name(void)

#define JT_CHECK(expr)                                                                             \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            jt_fail(__FILE__, __LINE__, #expr, 0);                                                 \
        }                                                                                          \
    } while (0)

#define JT_REQUIRE(expr)                                                                           \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            jt_fail(__FILE__, __LINE__, #expr, 1);                                                 \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#endif /* JOIST_TEST_HARNESS_H */

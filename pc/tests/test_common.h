// Tiny assertion harness shared by the test executables.
// This file is part of Wii-Sec-U (GPL-3.0-or-later).
#pragma once

#include <cstdio>
#include <cstdlib>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        g_checks++;                                                          \
        if (!((a) == (b))) {                                                 \
            g_failures++;                                                    \
            std::fprintf(stderr,                                             \
                         "FAIL %s:%d: %s == %s (lhs=%lld rhs=%lld)\n",       \
                         __FILE__, __LINE__, #a, #b,                         \
                         static_cast<long long>(a),                          \
                         static_cast<long long>(b));                         \
        }                                                                    \
    } while (0)

static int testSummary(const char *name) {
    if (g_failures == 0) {
        std::printf("%s: OK (%d checks)\n", name, g_checks);
        return 0;
    }
    std::fprintf(stderr, "%s: %d/%d checks FAILED\n", name, g_failures,
                 g_checks);
    return 1;
}

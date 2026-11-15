#pragma once
// bin2d 测试用的极简断言（风格与上层项目的 tests 一致：PASS/FAIL 行 + 失败非 0 退出）
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_bin2d_failures = 0;
static int g_bin2d_checks = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        ++g_bin2d_checks;                                                         \
        if (!(cond)) {                                                            \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);            \
            ++g_bin2d_failures;                                                   \
        } else {                                                                  \
            std::printf("PASS: %s\n", msg);                                       \
        }                                                                         \
    } while (0)

#define CHECK_NEAR(a, b, tol, msg)                                                \
    do {                                                                          \
        ++g_bin2d_checks;                                                         \
        const double va = static_cast<double>(a);                                  \
        const double vb = static_cast<double>(b);                                  \
        if (std::fabs(va - vb) > static_cast<double>(tol)) {                      \
            std::printf("FAIL: %s (%.6f vs %.6f, tol %.6f) (%s:%d)\n", msg,        \
                        va, vb, static_cast<double>(tol), __FILE__, __LINE__);     \
            ++g_bin2d_failures;                                                   \
        } else {                                                                  \
            std::printf("PASS: %s\n", msg);                                       \
        }                                                                         \
    } while (0)

inline int Bin2dTestSummary(const char* suite) {
    if (g_bin2d_failures == 0) {
        std::printf("=== %s: 全部通过 (%d 项) ===\n", suite, g_bin2d_checks);
        return 0;
    }
    std::printf("=== %s: %d/%d 项失败 ===\n", suite, g_bin2d_failures, g_bin2d_checks);
    return 1;
}

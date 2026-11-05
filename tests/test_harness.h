// A deliberately dependency-free test harness. T19 replaces this with whatever the tier-1
// suite needs; until then a few dozen lines is less machinery than a framework.

#pragma once

#include <cstdio>
#include <string>

namespace testing {

inline int& Failures()
{
    static int failures = 0;
    return failures;
}

inline void Check(bool condition, const char* what, const char* file, int line)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", what, file, line);
        ++Failures();
    }
}

inline void CheckEqual(const std::string& actual, const std::string& expected, const char* what,
                       const char* file, int line)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n  expected: \"%s\"\n  actual:   \"%s\"\n", what,
                     file, line, expected.c_str(), actual.c_str());
        ++Failures();
    }
}

inline void CheckEqualInt(long long actual, long long expected, const char* what, const char* file,
                          int line)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n  expected: %lld\n  actual:   %lld\n", what, file,
                     line, expected, actual);
        ++Failures();
    }
}

inline int Report()
{
    if (Failures() == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d check(s) failed\n", Failures());
    return 1;
}

} // namespace testing

#define CHECK(cond) ::testing::Check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
    ::testing::CheckEqual((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define CHECK_EQ_INT(actual, expected) \
    ::testing::CheckEqualInt((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

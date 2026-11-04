// A deliberately dependency-free test runner. T8 has one thing to assert; pulling in a
// test framework before there is anything to test would be a decision made too early.
// T19 replaces this with whatever the tier-1 suite needs.

#include <lootlocker/version.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

} // namespace

int main()
{
    const lootlocker::Version v = lootlocker::GetVersion();
    Check(v.major == LOOTLOCKER_VERSION_MAJOR, "major matches the header");
    Check(v.minor == LOOTLOCKER_VERSION_MINOR, "minor matches the header");
    Check(v.patch == LOOTLOCKER_VERSION_PATCH, "patch matches the header");

    Check(std::strcmp(lootlocker::GetVersionString(), "0.1.0") == 0,
          "version string is 0.1.0");

    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}

// The version must come from one place: the `project(LootLocker VERSION ...)` line.
//
// The previous version of this test compared the header's macros against GetVersion(),
// which both derive from the same header — so it could never catch the header drifting
// from CMake. This one compares against the version CMake was configured with, passed in
// as a compile definition, so a stale generated header fails here.

#include "test_harness.h"

#include <lootlocker/version.h>

#include <cstdio>
#include <cstring>

namespace {

void TestVersionMatchesCmake()
{
    const lootlocker::Version v = lootlocker::GetVersion();

    CHECK_EQ_INT(v.major, LOOTLOCKER_EXPECTED_MAJOR);
    CHECK_EQ_INT(v.minor, LOOTLOCKER_EXPECTED_MINOR);
    CHECK_EQ_INT(v.patch, LOOTLOCKER_EXPECTED_PATCH);
}

void TestVersionStringMatchesCmake()
{
    // The string form must agree with the numeric form, and both with CMake.
    char expected[64];
    std::snprintf(expected, sizeof(expected), "%d.%d.%d", LOOTLOCKER_EXPECTED_MAJOR,
                  LOOTLOCKER_EXPECTED_MINOR, LOOTLOCKER_EXPECTED_PATCH);

    CHECK_EQ(lootlocker::GetVersionString(), std::string(expected));
}

void TestVersionStringIsStaticStorage()
{
    // Documented as returning static storage, not an allocation. Two calls must return the
    // same pointer, and the pointer must be stable.
    const char* a = lootlocker::GetVersionString();
    const char* b = lootlocker::GetVersionString();

    CHECK(a == b);
    CHECK(a != nullptr);
    CHECK(std::strlen(a) > 0);
}

} // namespace

int main()
{
    TestVersionMatchesCmake();
    TestVersionStringMatchesCmake();
    TestVersionStringIsStaticStorage();

    return testing::Report();
}

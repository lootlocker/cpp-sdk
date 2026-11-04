#pragma once

// The SDK's version, as a compile-time constant.
//
// The single source of truth is `project(LootLocker VERSION x.y.z)` in the top-level
// CMakeLists.txt; this header is generated from it at configure time (D15). A consumer
// that drops the sources in without CMake gets the fallback below, which is why it is a
// valid version rather than an error.

#include <cstdint>

#define LOOTLOCKER_VERSION_MAJOR 0
#define LOOTLOCKER_VERSION_MINOR 1
#define LOOTLOCKER_VERSION_PATCH 0

namespace lootlocker {

struct Version {
    int major;
    int minor;
    int patch;
};

// The version this build was compiled from.
Version GetVersion();

// "0.1.0", for logs and support tickets. Allocates, so not on a hot path.
const char* GetVersionString();

} // namespace lootlocker

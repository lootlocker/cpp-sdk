#include <lootlocker/version.h>

namespace lootlocker {

Version GetVersion()
{
    return Version{LOOTLOCKER_VERSION_MAJOR, LOOTLOCKER_VERSION_MINOR, LOOTLOCKER_VERSION_PATCH};
}

const char* GetVersionString()
{
    // A string literal, so static storage with static lifetime. No allocation.
    return LOOTLOCKER_VERSION_STRING;
}

} // namespace lootlocker

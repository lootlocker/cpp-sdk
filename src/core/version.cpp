#include <lootlocker/version.h>

namespace lootlocker {

Version GetVersion()
{
    return Version{LOOTLOCKER_VERSION_MAJOR, LOOTLOCKER_VERSION_MINOR, LOOTLOCKER_VERSION_PATCH};
}

const char* GetVersionString()
{
    return "0.1.0";
}

} // namespace lootlocker

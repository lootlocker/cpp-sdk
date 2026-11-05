// The smallest possible program: link core, print the version.
//
// The real "smallest possible program" — a guest session — lands with T14. This one
// exists so the examples directory has something that builds from day one.

#include <lootlocker/version.h>

#include <cstdio>

int main()
{
    const lootlocker::Version v = lootlocker::GetVersion();
    std::printf("LootLocker C++ SDK %d.%d.%d\n", v.major, v.minor, v.patch);
    return 0;
}

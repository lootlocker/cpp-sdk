// Tier 2 (D14): a consumer that links only LootLocker::auth.
//
// This file is the target-graph assertion. It includes only the auth header and links
// only the auth target, so if auth ever grows a dependency on broadcasts — or if the
// include-path boundary (D18) leaks — this stops compiling or linking.

#include <lootlocker/auth.h>
#include <lootlocker/version.h>

#include <cstdio>

int main()
{
    std::printf("consumer_auth built against LootLocker %s\n",
                lootlocker::GetVersionString());
    return 0;
}

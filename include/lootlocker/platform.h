#pragma once

// The auth platform (D16, T14 review).
//
// A wire value, not a free string. The API accepts a fixed set of platform names and rejects
// anything else, so a typo in a string literal is a runtime 400 that a type would have caught
// at compile time. The names match the other SDKs' `LL_AuthPlatforms` exactly, because the
// backend compares them literally.

#include <lootlocker/json_value.h>
#include <lootlocker/result.h>

#include <string>

namespace lootlocker {

enum class Platform {
    None = 0,
    Guest,
    WhiteLabel,
    Steam,
    PlayStationNetwork,
    XboxOne,
    NintendoSwitch,
    AmazonLuna,
    AppleSignIn,
    AppleGameCenter,
    Android,
    GoogleSignIn,
    GooglePlayGames,
    EpicGames,
    Meta,
    Remote,
    Discord,
};

// The wire name, e.g. `"guest"`. Never null.
const char* ToString(Platform platform);

// Parses a wire name. Returns false and leaves `out` untouched when the name is unknown,
// rather than defaulting to `None` - an unknown platform is a contract change, not a
// missing value.
bool TryParsePlatform(const std::string& name, Platform& out);

// The ADL deserialiser, so a platform round-trips through the JSON layer like any other
// field (D27). There is no `to_json`: a `JsonBuilder` writes into an object, so a bare enum
// has no key to write under. A caller serialises with `b.Set("platform", ToString(p))`.
Result<Empty> from_json(const JsonValue& j, Platform& out);

} // namespace lootlocker

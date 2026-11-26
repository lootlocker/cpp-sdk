#include <lootlocker/platform.h>

#include <lootlocker/json_builder.h>

namespace lootlocker {

namespace {

struct PlatformName {
    Platform    platform;
    const char* name;
};

// The wire names, matching the other SDKs' `LL_AuthPlatforms` exactly. The backend compares
// them literally, so a "nicer" spelling here is a 400 at runtime.
const PlatformName kPlatformNames[] = {
    {Platform::None,             ""},
    {Platform::Guest,            "guest"},
    {Platform::WhiteLabel,       "white_label_login"},
    {Platform::Steam,            "steam"},
    {Platform::PlayStationNetwork, "psn"},
    {Platform::XboxOne,          "xbox_one"},
    {Platform::NintendoSwitch,   "nintendo_switch"},
    {Platform::AmazonLuna,       "amazon_luna"},
    {Platform::AppleSignIn,      "apple_sign_in"},
    {Platform::AppleGameCenter,  "apple_game_center"},
    {Platform::Android,          "android"},
    {Platform::GoogleSignIn,     "google_sign_in"},
    {Platform::GooglePlayGames,  "google_play_games"},
    {Platform::EpicGames,        "epic_games"},
    {Platform::Meta,             "meta"},
    {Platform::Remote,           "remote"},
    {Platform::Discord,          "discord"},
};

} // namespace

const char* ToString(Platform platform)
{
    for (const PlatformName& entry : kPlatformNames) {
        if (entry.platform == platform) {
            return entry.name;
        }
    }
    return "";
}

bool TryParsePlatform(const std::string& name, Platform& out)
{
    for (const PlatformName& entry : kPlatformNames) {
        if (name == entry.name) {
            out = entry.platform;
            return true;
        }
    }
    return false;
}

Result<Empty> from_json(const JsonValue& j, Platform& out)
{
    if (j.GetKind() != JsonValue::Kind::String) {
        Error e;
        e.code    = ErrorCode::MalformedResponse;
        e.message = "the platform is missing or not a string";
        return Result<Empty>::Fail(e);
    }

    Platform parsed = Platform::None;
    if (!TryParsePlatform(j.AsString(), parsed)) {
        Error e;
        e.code    = ErrorCode::MalformedResponse;
        e.message = "the platform is not one the SDK knows: " + j.AsString();
        return Result<Empty>::Fail(e);
    }
    out = parsed;
    return Result<Empty>::Ok(Empty());
}

} // namespace lootlocker

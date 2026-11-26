#pragma once

// The session (D8, D12). A wire mirror: fields are `snake_case` and match the JSON exactly.
//
// This is the one type that is both a wire mirror and a persisted record, which is why its
// field set is exactly what D8 says to store and nothing more.

#include <lootlocker/json_value.h>
#include <lootlocker/result.h>

#include <cstdint>
#include <string>
#include <type_traits>

namespace lootlocker {

struct Session {
    std::string   session_token;
    std::string   player_identifier;
    std::string   player_ulid;
    std::string   public_uid;
    std::string   player_name;
    std::uint64_t player_id = 0;      // uint64 on the wire; see D4
    std::string   player_created_at;
    bool          seen_before = false;
    bool          check_grant_notifications = false;
    bool          check_deactivation_notifications = false;
};

// `player_id` must stay an unsigned 64-bit integer.
//
// **This is R9's documented mitigation, not a guard against someone editing the struct.**
// The D4 spike showed both candidate parsers are exact by default, so the hazard is not the
// parser but a `double` field in our own structs - and the failure is silent, because a
// value above 2^53 loses precision without any error. The assert is what makes adding such
// a field a compile error rather than a support ticket.
static_assert(sizeof(Session::player_id) == 8 && !std::is_floating_point<decltype(Session::player_id)>::value,
              "player_id must be an unsigned 64-bit integer (D4, R9)");

// Decodes a session response (D27).
//
// **Which fields are required, and why that is not a silent default.** D23 says a missing
// field is a decode error rather than a zero, and that rule exists to stop a field the SDK
// depends on being silently defaulted. It does not mean every field is mandatory:
//
//   - `session_token`, `player_identifier`, `player_ulid`, `public_uid` and `player_id` are
//     required. Without them the session is unusable or the player cannot be identified,
//     and a default would be a lie.
//   - `player_name` is optional, because an absent name genuinely means "this player has
//     not set one". Defaulting it to empty is the correct reading, not a silent default.
//   - `player_created_at` and the three booleans are optional for the same reason: they are
//     display and notification hints, and the API omits them on some auth paths.
Result<Empty> from_json(const JsonValue& j, Session& out);

} // namespace lootlocker

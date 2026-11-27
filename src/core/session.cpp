#include <lootlocker/session.h>

#include "json.h"

namespace lootlocker {

Result<Empty> from_json(const JsonValue& j, Session& out)
{
    if (j.GetKind() != JsonValue::Kind::Object) {
        Error e;
        e.code    = ErrorCode::MalformedResponse;
        e.message = "the session response is not an object";
        return Result<Empty>::Fail(e);
    }

    Error error;

    // Required: without these the session is unusable or the player cannot be identified.
    if (!core::ReadRequiredString(j, "session_token", out.session_token, error) ||
        !core::ReadRequiredString(j, "player_identifier", out.player_identifier, error) ||
        !core::ReadRequiredString(j, "player_ulid", out.player_ulid, error) ||
        !core::ReadRequiredString(j, "public_uid", out.public_uid, error) ||
        !core::ReadRequiredUInt64(j, "player_id", out.player_id, error)) {
        return Result<Empty>::Fail(error);
    }

    // Optional: an absent name genuinely means "not set", and the API omits the rest on
    // some auth paths.
    if (!core::ReadOptionalString(j, "player_name", out.player_name, error) ||
        !core::ReadOptionalString(j, "player_created_at", out.player_created_at, error) ||
        !core::ReadOptionalBool(j, "seen_before", out.seen_before, error) ||
        !core::ReadOptionalBool(j, "check_grant_notifications", out.check_grant_notifications,
                                error)) {
        return Result<Empty>::Fail(error);
    }

    return Result<Empty>::Ok(Empty());
}

} // namespace lootlocker

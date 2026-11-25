#include <lootlocker/session.h>

namespace lootlocker {

namespace {

Error Missing(const char* field)
{
    Error e;
    e.code    = ErrorCode::MalformedResponse;
    e.message = std::string("the session response is missing ") + field;
    return e;
}

// A required string. Absent, null and non-string are all failures, because a session field
// the SDK depends on must not be silently defaulted (D23).
bool ReadRequiredString(const JsonValue& j, const char* key, std::string& out, Error& error)
{
    const JsonValue value = j[key];
    if (value.GetKind() != JsonValue::Kind::String) {
        error = Missing(key);
        return false;
    }
    out = value.AsString();
    return true;
}

// An optional string. Absent or null leaves `out` untouched; a present non-string is still
// an error, because that is a shape the API does not send and silently ignoring it would
// hide a contract change.
bool ReadOptionalString(const JsonValue& j, const char* key, std::string& out, Error& error)
{
    const JsonValue value = j[key];
    if (value.GetKind() == JsonValue::Kind::Invalid || value.GetKind() == JsonValue::Kind::Null) {
        return true;
    }
    if (value.GetKind() != JsonValue::Kind::String) {
        error = Missing(key);
        return false;
    }
    out = value.AsString();
    return true;
}

bool ReadOptionalBool(const JsonValue& j, const char* key, bool& out, Error& error)
{
    const JsonValue value = j[key];
    if (value.GetKind() == JsonValue::Kind::Invalid || value.GetKind() == JsonValue::Kind::Null) {
        return true;
    }
    if (value.GetKind() != JsonValue::Kind::Bool) {
        error = Missing(key);
        return false;
    }
    out = value.AsBool();
    return true;
}

} // namespace

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
    if (!ReadRequiredString(j, "session_token", out.session_token, error) ||
        !ReadRequiredString(j, "player_identifier", out.player_identifier, error) ||
        !ReadRequiredString(j, "player_ulid", out.player_ulid, error) ||
        !ReadRequiredString(j, "public_uid", out.public_uid, error)) {
        return Result<Empty>::Fail(error);
    }

    // `player_id` is required and must be an unsigned integer. Reading it through
    // `AsDouble()` would lose precision above 2^53, which is the hazard D4 exists for.
    const JsonValue player_id = j["player_id"];
    if (!player_id.IsUnsigned()) {
        return Result<Empty>::Fail(Missing("player_id"));
    }
    out.player_id = player_id.AsUInt64();

    // Optional: an absent name genuinely means "not set", and the API omits the rest on
    // some auth paths.
    if (!ReadOptionalString(j, "player_name", out.player_name, error) ||
        !ReadOptionalString(j, "player_created_at", out.player_created_at, error) ||
        !ReadOptionalBool(j, "seen_before", out.seen_before, error) ||
        !ReadOptionalBool(j, "check_grant_notifications", out.check_grant_notifications,
                          error) ||
        !ReadOptionalBool(j, "check_deactivation_notifications",
                          out.check_deactivation_notifications, error)) {
        return Result<Empty>::Fail(error);
    }

    return Result<Empty>::Ok(Empty());
}

} // namespace lootlocker

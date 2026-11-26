#include <lootlocker/auth.h>

#include <lootlocker/json_builder.h>
#include <lootlocker/state_writer.h>
#include <lootlocker/timezone.h>

#include "core/definition.h"
#include "core/query_parameters.h"

#include <cstdio>
#include <random>
#include <string>
#include <utility>

namespace lootlocker {

namespace {

// The key the SDK persists. Named here rather than inline so a typo is a compile error
// rather than a silently missing key.
//
// **`guest_identifier`, not `player_identifier`.** The wire field is `player_identifier` on
// every platform, but only on guest is it a client-generated value; on the others the server
// owns it. Naming the persisted key after the concept rather than the wire field keeps the
// two from being confused.
const char* const kGuestIdentifierKey = "guest_identifier";

// A guest identifier: a UUID-shaped random string.
//
// Not a security token - the server treats it as an opaque account handle - so
// `std::random_device` is the right tool rather than a cryptographic generator. It only has
// to be unlikely to collide, and it must be stable once persisted.
std::string GenerateGuestIdentifier()
{
    std::random_device device;
    std::uint64_t high = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    std::uint64_t low  = (static_cast<std::uint64_t>(device()) << 32) ^ device();

    high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    low  = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((high >> 32) & 0xFFFFFFFFULL),
                  static_cast<unsigned>((high >> 16) & 0xFFFFULL),
                  static_cast<unsigned>(high & 0xFFFFULL),
                  static_cast<unsigned>((low >> 48) & 0xFFFFULL),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
    return std::string(buffer);
}

// The identifier to send: the caller's, else the persisted one, else a new one.
//
// **Whatever is chosen is persisted, including the caller's.** A game that supplies an
// identifier on the first launch and then relies on the SDK's persisted value would
// otherwise get a different account on the next launch - which is the failure this whole
// mechanism exists to prevent.
//
// The value is written *before* the request is sent. A crash between sending and receiving
// would otherwise lose the identifier and orphan the account the server just created.
std::string ResolveGuestIdentifier(Client& ll, const GuestSessionInputs& inputs)
{
    std::shared_ptr<IStateWriter> writer = ll.GetStateWriter().lock();

    if (inputs.guest_identifier.has_value() && !inputs.guest_identifier->empty()) {
        if (writer) {
            writer->SetString(kGuestIdentifierKey, *inputs.guest_identifier);
        }
        return *inputs.guest_identifier;
    }

    if (writer) {
        std::string persisted;
        if (writer->GetString(kGuestIdentifierKey, persisted) && !persisted.empty()) {
            return persisted;
        }
    }

    const std::string generated = GenerateGuestIdentifier();
    if (writer) {
        writer->SetString(kGuestIdentifierKey, generated);
    }
    return generated;
}

// The timezone to send, or empty for "send none".
//
// Resolution order, per D26: the caller's option, then `Config::timezone`, then the injected
// `TimezoneProvider`, then the OS, then `Etc/UTC`. `Config::send_timezone == false` means
// send nothing at all, which is a different statement from "I have no opinion".
//
// The result is normalised to an IANA name, because a Windows name sent as-is gets UTC
// server-side and puts every local-day boundary on the wrong day (R27).
std::string ResolveTimezone(Client& ll, const SessionOptionals& optionals)
{
    const Config& config = ll.GetConfig();
    if (!config.send_timezone) {
        return std::string();
    }

    std::string candidate = optionals.timezone;
    if (candidate.empty()) {
        candidate = config.timezone;
    }
    if (candidate.empty()) {
        if (std::shared_ptr<TimezoneProvider> provider = config.timezone_provider.lock()) {
            candidate = provider->GetTimezone();
        }
    }

    std::string iana;
    if (TryNormalizeToIana(candidate, iana)) {
        return iana;
    }
    // `TryNormalizeToIana` sets the default on failure, so this is the documented fallback
    // rather than an empty string the server would read as UTC.
    return std::string(kDefaultIanaTimezone);
}

// The backend's `optionals` object, serialised only when it carries something.
//
// The defaults are the backend's, so a field equal to its default is omitted rather than
// sent - which keeps the body small and makes "the caller did not care" visible on the wire.
struct SessionOptionalsBody {
    std::string timezone;
    std::string player_name;
    bool        auto_create_profile = true;
    bool        check_dlc           = false;
    std::string referral_code;
    std::string referral_program_key;
};

void to_json(JsonBuilder& b, const SessionOptionalsBody& in)
{
    if (!in.timezone.empty()) {
        b.Set("timezone", in.timezone);
    }
    if (!in.player_name.empty()) {
        b.Set("player_name", in.player_name);
    }
    if (!in.auto_create_profile) {
        b.Set("auto_create_profile", false);
    }
    if (in.check_dlc) {
        b.Set("check_dlc", true);
    }
    if (!in.referral_code.empty()) {
        b.Set("referral_code", in.referral_code);
    }
    if (!in.referral_program_key.empty()) {
        b.Set("referral_program_key", in.referral_program_key);
    }
}

// The request body.
//
// **`optionals` is a nested object, not root fields.** The backend's `GuestInput` embeds
// `BaseInput` and carries `Optionals Optionals` tagged `json:"optionals"`, so a `timezone`
// at the root is silently ignored and the player gets UTC. Verified against
// `services/gamesession/models.go` rather than inferred from the Unity SDK.
struct GuestSessionRequest {
    std::string           game_key;
    std::string           game_version;
    std::string           player_identifier;
    Platform              platform = Platform::Guest;
    SessionOptionalsBody  optionals;
    bool                  has_optionals = false;
};

void to_json(JsonBuilder& b, const GuestSessionRequest& in)
{
    b.Set("game_key", in.game_key);
    b.Set("game_version", in.game_version);
    b.Set("player_identifier", in.player_identifier);
    b.Set("platform", ToString(in.platform));

    // Omitted entirely when nothing is set: the backend defaults an absent `optionals` to
    // UTC, and an empty object is noise.
    if (in.has_optionals) {
        b.Set("optionals", in.optionals);
    }
}

} // namespace

namespace core {

struct GuestSessionDef {
    static constexpr const char* kMethod = "POST";
    static constexpr const char* kPath   = "/game/v2/session/guest";

    // A session response carries required fields, so an empty body is a malformed response
    // rather than a success. Without this a 200 with no body would become an all-default
    // `Session` and `HasSession()` would report true with no token.
    static constexpr bool kAllowEmptyResponse = false;

    // An authentication request is not an authenticated request, so it must not carry a
    // session token. A stale token from a previous session would otherwise be sent to an
    // endpoint that has no use for it.
    static constexpr bool kRequiresSession = false;

    using Request  = GuestSessionRequest;
    using Response = Session;

    static void ConstructQueryParameters(const Request&, QueryParameters&) {}

    static void ConstructHeaders(const Request&, HeaderMap&) {}
};

} // namespace core

namespace auth {

RequestId StartGuestSession(Client& ll, SessionCallback on_done)
{
    return StartGuestSession(ll, GuestSessionInputs(), std::move(on_done));
}

RequestId StartGuestSession(Client& ll, const GuestSessionInputs& inputs,
                            SessionCallback on_done)
{
    const Config& config = ll.GetConfig();

    GuestSessionRequest request;
    request.game_key          = config.game_key;
    request.game_version      = config.game_version;
    request.platform          = Platform::Guest;
    request.player_identifier = ResolveGuestIdentifier(ll, inputs);

    if (inputs.optionals.has_value()) {
        const SessionOptionals& in = *inputs.optionals;

        request.optionals.timezone             = ResolveTimezone(ll, in);
        request.optionals.player_name          = in.player_name;
        request.optionals.auto_create_profile  = in.auto_create_profile;
        request.optionals.check_dlc            = in.check_dlc;
        request.optionals.referral_code        = in.referral_code;
        request.optionals.referral_program_key = in.referral_program_key;

        request.has_optionals = !request.optionals.timezone.empty() ||
                                !request.optionals.player_name.empty() ||
                                !request.optionals.auto_create_profile ||
                                request.optionals.check_dlc ||
                                !request.optionals.referral_code.empty() ||
                                !request.optionals.referral_program_key.empty();
    } else {
        // No optionals supplied, but the timezone is still resolved: it is a config-level
        // concern rather than a per-call one, and a game that set `Config::timezone` expects
        // it to be sent.
        request.optionals.timezone = ResolveTimezone(ll, SessionOptionals());
        request.has_optionals      = !request.optionals.timezone.empty();
    }

    return core::Send<core::GuestSessionDef>(
        ll, request, [&ll, on_done](Result<Session> result) {
            if (result.Ok()) {
                // Stored on the client so subsequent requests carry `x-session-token` and
                // `GetSession()` returns it. Done here rather than by the caller, because a
                // caller that forgot would get a 401 on the next call with no clue why.
                ll.SetSession(result.Value());
            }
            on_done(std::move(result));
        });
}

} // namespace auth
} // namespace lootlocker

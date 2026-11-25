#include <lootlocker/auth.h>

#include <lootlocker/json_builder.h>
#include <lootlocker/state_writer.h>

#include "core/definition.h"
#include "core/query_parameters.h"

#include <cstdio>
#include <random>
#include <string>
#include <utility>

namespace lootlocker {

namespace {

// The keys the SDK persists. Named here rather than inline so a typo is a compile error
// rather than a silently missing key.
const char* const kPlayerIdentifierKey = "player_identifier";

// A guest identifier: a UUID-shaped random string.
//
// Not a security token - the server treats it as an opaque account handle - so
// `std::random_device` is the right tool rather than a cryptographic generator. It only has
// to be unlikely to collide, and it must be stable once persisted.
std::string GeneratePlayerIdentifier()
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
// The resolved value is persisted *before* the request is sent. A crash between sending and
// receiving would otherwise lose the identifier and orphan the account the server just
// created - which is the single most damaging thing to lose (D8).
std::string ResolvePlayerIdentifier(Client& ll, const GuestSessionOptions& opts)
{
    if (!opts.player_identifier.empty()) {
        return opts.player_identifier;
    }

    std::shared_ptr<IStateWriter> writer = ll.GetStateWriter().lock();
    if (writer) {
        std::string persisted;
        if (writer->GetString(kPlayerIdentifierKey, persisted) && !persisted.empty()) {
            return persisted;
        }
    }

    const std::string generated = GeneratePlayerIdentifier();
    if (writer) {
        writer->SetString(kPlayerIdentifierKey, generated);
    }
    return generated;
}

} // namespace

// The request type lives in `lootlocker`, not the anonymous namespace, because the
// definition in `core` names it and `to_json` is found by ADL.
struct GuestSessionRequest {
    std::string game_key;
    std::string game_version;
    std::string player_identifier;
    std::string platform;
    std::string timezone;
    std::string locale;
    std::string player_name;
    std::string character_name;
};

void to_json(JsonBuilder& b, const GuestSessionRequest& in)
{
    b.Set("game_key", in.game_key);
    b.Set("game_version", in.game_version);
    b.Set("player_identifier", in.player_identifier);
    b.Set("platform", in.platform);

    if (!in.timezone.empty()) {
        b.Set("timezone", in.timezone);
    }
    if (!in.locale.empty()) {
        b.Set("locale", in.locale);
    }
    if (!in.player_name.empty()) {
        b.Set("player_name", in.player_name);
    }
    if (!in.character_name.empty()) {
        b.Set("character_name", in.character_name);
    }
}

namespace core {

struct GuestSessionDef {
    static constexpr const char* kMethod = "POST";
    static constexpr const char* kPath   = "/game/v2/session/guest";

    using Request  = GuestSessionRequest;
    using Response = Session;

    static void ConstructQueryParameters(const Request&, QueryParameters&) {}

    static void ConstructHeaders(const Request&, HeaderMap&) {}
};

} // namespace core

namespace auth {

RequestId StartGuestSession(Client& ll, SessionCallback on_done)
{
    return StartGuestSession(ll, GuestSessionOptions(), std::move(on_done));
}

RequestId StartGuestSession(Client& ll, const GuestSessionOptions& opts,
                            SessionCallback on_done)
{
    const Config& config = ll.GetConfig();

    GuestSessionRequest request;
    request.game_key          = config.game_key;
    request.game_version      = config.game_version;
    request.platform          = "guest";
    request.player_identifier = ResolvePlayerIdentifier(ll, opts);
    request.timezone          = opts.timezone;
    request.locale            = opts.locale;
    request.player_name       = opts.player_name;
    request.character_name    = opts.character_name;

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

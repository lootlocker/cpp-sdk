#pragma once

// Guest authentication (D16, T14).
//
// A guest session is the MVP's primary auth path: no credentials, no platform, just an
// identifier the SDK generates once and reuses. **The reuse is the whole point** - a guest
// who loses their `player_identifier` silently becomes a new account with no inventory, so
// the SDK persists it (D8) and sends it back on the next launch.
//
// The identifier is generated here rather than by the caller, because a caller that has to
// invent one will invent a different one on the next launch.

#include <lootlocker/client.h>
#include <lootlocker/request_id.h>
#include <lootlocker/result.h>
#include <lootlocker/session.h>

#include <functional>
#include <string>

namespace lootlocker {

// Everything the API calls `optionals`. All of it is optional: a session start needs only
// `game_key` and `game_version`, which live in `Config`.
struct GuestSessionOptions {
    // Reuse it to keep the same guest across launches. Empty means "use the persisted one,
    // or generate a new one" - which is what almost every caller wants.
    std::string player_identifier;

    // Empty means "resolve it": the injected provider, then the OS, then `Etc/UTC` (D26).
    std::string timezone;

    std::string locale;
    std::string player_name;
    std::string character_name;
};

using SessionCallback = std::function<void(Result<Session>)>;

namespace auth {

// POST /game/v2/session/guest
//
// Starts a guest session and stores it on the client, so subsequent requests carry
// `x-session-token` and `Client::GetSession()` returns it.
//
// **The identifier is resolved in this order:** `opts.player_identifier` if set, then the
// one persisted by a previous session, then a newly generated one. The resolved value is
// persisted before the request is sent, so a crash between sending and receiving does not
// lose the identifier and orphan the account.
//
// Returns the id the executor assigned. The callback fires on the thread that calls
// `Client::Update()` (D5).
RequestId StartGuestSession(Client& ll, SessionCallback on_done);

RequestId StartGuestSession(Client& ll, const GuestSessionOptions& opts,
                            SessionCallback on_done);

} // namespace auth
} // namespace lootlocker

#pragma once

// Guest authentication (D16, T14).
//
// A guest session is the MVP's primary auth path: no credentials, no platform, just an
// identifier the SDK generates once and reuses. **The reuse is the whole point** - a guest
// who loses their identifier silently becomes a new account with no inventory, so the SDK
// persists it (D8) and sends it back on the next launch.
//
// The identifier is generated here rather than by the caller, because a caller that has to
// invent one will invent a different one on the next launch.

#include <lootlocker/client.h>
#include <lootlocker/platform.h>
#include <lootlocker/request_id.h>
#include <lootlocker/result.h>
#include <lootlocker/session.h>

#include <functional>
#include <optional>
#include <string>

namespace lootlocker {

// The backend's `optionals` object, verbatim.
//
// **Named `optionals`, not `options`.** It is the backend's term for "a set of optional
// values", and `options` invokes a different meaning - configuration rather than
// per-request optional data. The other SDKs use the backend term for the same reason.
//
// Every field is optional, and the defaults are the backend's: an absent `timezone` means
// UTC, an absent `auto_create_profile` means true.
struct SessionOptionals {
    // IANA format. Empty means "resolve it": the injected provider, then the OS, then
    // `Etc/UTC` (D26).
    std::string timezone;

    // The player's display name. Empty means "leave it as it is".
    std::string player_name;

    // Whether to create a profile for a player who does not have one. Defaults to true;
    // false makes the session fail with a 404 for an unknown player.
    bool auto_create_profile = true;

    // Whether the client reports owning DLC, so the server reconciles ownership at session
    // start. Defaults to false.
    bool check_dlc = false;

    // The referrer's public UID, for attributing a referral on a first session. Ignored for
    // returning players.
    std::string referral_code;

    // The referral program the code redeems. Omit when the game has a single active program.
    std::string referral_program_key;
};

// Everything a guest session start takes.
//
// **An input struct rather than parameters plus overloads** (owner, 2026-11-26). Two
// optional parameters is four overloads and four is sixteen; a struct is one shape that
// gains fields without breaking callers. The zero-argument `StartGuestSession(ll, cb)` stays
// as the common call.
//
// Both fields are `Optional` because neither is required, and "unset" is a different
// statement from "set to empty" - the same distinction `Config::send_timezone` exists to
// preserve.
struct GuestSessionInputs {
    // The guest identifier to use. Unset means "use the persisted one, or generate a new
    // one", which is what almost every caller wants.
    //
    // **Not part of `optionals`.** The backend's `optionals` object does not contain it, and
    // muddling the two would make the SDK's shape disagree with the wire's.
    std::optional<std::string> guest_identifier;

    std::optional<SessionOptionals> optionals;
};

using SessionCallback = std::function<void(Result<Session>)>;

namespace auth {

// POST /game/v2/session/guest
//
// Starts a guest session and stores it on the client, so subsequent requests carry
// `x-session-token` and `Client::GetSession()` returns it.
//
// **The identifier is resolved in this order:** `inputs.guest_identifier` if set, then the
// one persisted by a previous session, then a newly generated one. Whatever is chosen is
// persisted before the request is sent, so a crash between sending and receiving does not
// lose it and orphan the account.
//
// **The session token is never sent on this request.** An authentication request is not an
// authenticated request, so attaching a token to it is wrong by definition - and a stale
// token from a previous session would be sent to an endpoint that has no use for it.
//
// Returns the id the executor assigned. The callback fires on the thread that calls
// `Client::Update()` (D5).
RequestId StartGuestSession(Client& ll, SessionCallback on_done);

RequestId StartGuestSession(Client& ll, const GuestSessionInputs& inputs,
                            SessionCallback on_done);

} // namespace auth
} // namespace lootlocker

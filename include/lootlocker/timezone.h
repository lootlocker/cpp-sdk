#pragma once

// Timezone handling (D26, R27).
//
// The problem this solves: Windows does not give you IANA names. `GetTimeZoneInformation`
// returns "W. Europe Standard Time", and the Game API wants "Europe/Berlin". A game that
// sends the Windows name gets UTC server-side, which puts every local-day boundary —
// daily logins, event windows, `once_per_day` buckets — on the wrong day.
//
// Two parts:
//
//   1. A Windows<->IANA table, generated from data/windows-to-iana.tsv.
//   2. A `TimezoneProvider` seam, so an engine that already knows the player's timezone
//      can supply it instead of the SDK guessing from the OS.
//
// The seam exists because timezone and locale are both potentially user-configurable in a
// game, and every engine has its own way of exposing them. The SDK's default provider
// reads the OS, which is right for a plain application and often wrong for a game whose
// player picked a timezone in a settings menu.

#include <lootlocker/result.h>

#include <string>

namespace lootlocker {

// The IANA name the SDK falls back to when nothing else is available.
//
// Correctly cased: the backend rejects "ETC/GMT" and similar with an "invalid JSON input"
// error, which is a confusing failure for a casing mistake.
extern const char* const kDefaultIanaTimezone;

// Convert a Windows timezone name to its IANA equivalent.
//
// Case-insensitive on the Windows name. Returns false and leaves `out` untouched when the
// name is not in the table — the caller decides what to do, rather than being handed a
// silent UTC.
bool TryConvertWindowsToIana(const std::string& windows_name, std::string& out);

// Convert an IANA name to a Windows name. Case-insensitive on the IANA name.
//
// Many-to-one: several IANA names share a Windows name, so this is not a strict inverse of
// the above. Only the IANA representatives present in the generated table are accepted —
// the table is a Windows mapping, not the IANA database, so it holds one name per Windows
// zone and does not carry aliases.
bool TryConvertIanaToWindows(const std::string& iana_name, std::string& out);

// True when `name` looks like an IANA name: a well-formed `Area/Location` path.
//
// This is a shape check, not a lookup. The table is a Windows mapping rather than the IANA
// database, so validating against it would reject real zones such as
// `America/Argentina/Buenos_Aires` and silently fall through to UTC. The server is the
// authority on whether a zone exists.
bool IsKnownIanaTimezone(const std::string& name);

// True when `name` is a known Windows name in the table.
bool IsKnownWindowsTimezone(const std::string& name);

// Normalise any of the accepted forms to an IANA name.
//
// Accepts, in order: a numeric UTC offset ("2", "-5"), an IANA name (passed through), or a
// Windows name (converted). Returns false and sets `out` to `kDefaultIanaTimezone` when the
// input is empty or unrecognised, so a caller that ignores the return value still sends
// something valid rather than an empty string.
bool TryNormalizeToIana(const std::string& timezone, std::string& out);

// Convert a UTC offset in hours to an IANA `Etc/GMT` name.
//
// Note the reversed sign, which is an IANA convention and not a bug: `Etc/GMT+5` is UTC-5.
// Offsets outside [-12, 14] are clamped.
std::string UtcOffsetToIana(int utc_offset_hours);

// Supplies the player's timezone.
//
// The SDK ships a default that reads the OS. An engine with its own notion of the player's
// timezone — a settings menu, a profile field, a platform API — should inject its own,
// because the OS value is frequently not what the player chose.
//
// Implementations must not throw (D7) and must be cheap: this is called once per session
// start, not per frame.
class TimezoneProvider {
public:
    virtual ~TimezoneProvider() = default;

    // The player's timezone, in any form `TryNormalizeToIana` accepts.
    //
    // Return an empty string when the provider has nothing to offer; the SDK then falls
    // back to its own detection and finally to `kDefaultIanaTimezone`.
    virtual std::string GetTimezone() = 0;
};

// The default provider: reads the OS timezone.
//
// - Windows: `GetDynamicTimeZoneInformation`, converted through the table.
// - Linux/macOS: the `TZ` environment variable, then `/etc/localtime`, then UTC.
// - Anything else: UTC.
//
// **This is the one place the SDK reads the environment, and it is deliberate.** D9 says
// the SDK reads no file and no environment variable on its own, because a host with its
// own config system must not have a second one imposed on it. The OS timezone is the
// exception that proves the rule: it is not configuration, it is the platform's answer to
// "where is this machine", and an adopter who wants to control it injects a provider
// instead of setting `TZ`. Nothing else in the SDK reads the environment.
//
// **This may not reflect what the player chose.** A game with a timezone setting should
// inject its own provider. The SDK documents this rather than pretending the OS value is
// authoritative.
class SystemTimezoneProvider : public TimezoneProvider {
public:
    std::string GetTimezone() override;
};

// Resolve the timezone to send on a session start.
//
// Order: the injected provider, then the OS, then `kDefaultIanaTimezone`. `override_value`
// is the `Config::timezone` field: when non-empty it wins outright, because an adopter who
// set it explicitly meant it.
//
// Returns an empty string when `send` is false, which is how an adopter says "send no
// timezone at all". That is a separate flag rather than an empty `override_value`, because
// empty already means "resolve it".
std::string ResolveTimezone(TimezoneProvider* provider, const std::string& override_value,
                            bool send = true);

} // namespace lootlocker

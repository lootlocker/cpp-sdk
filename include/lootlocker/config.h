#pragma once

// Configuration (D9).
//
// The `Config` struct is the single runtime source of truth. Nothing is hard-coded and
// nothing is read implicitly: the SDK reads no file and no environment variable on its
// own. The file loader and the CLI parser are thin, optional helpers that *populate* the
// struct; the adopter calls them, or fills the struct by hand, or both in either order.
// Precedence is the adopter's business, not the SDK's.

#include <lootlocker/result.h>

#include <cstdint>
#include <string>

namespace lootlocker {

class Logger;
class StateWriter;
class IHttpTransport;
class TimezoneProvider;

// The execution queue's policy (D25). Ported from the Unreal SDK's behaviour, not its
// shape: the values are hard-won, the mechanism is ours.
struct QueueConfig {
    int  max_retries             = 5;
    int  backoff_factor          = 2;      // 50 ms, 100, 200, 400, ...
    int  initial_backoff_ms      = 50;
    int  max_in_flight           = 50;
    int  max_queue_size          = 5000;
    int  choke_warning_threshold = 500;
    int  request_timeout_seconds = 300;
    bool deny_when_backed_up     = true;
};

// The rate limiter's policy (D25).
//
// The moving-average threshold is *derived*, not configured separately, so the fraction
// and the effective per-bucket limit cannot disagree:
//
//   per_bucket_limit = floor(max_requests_per_tripwire * moving_average_fraction
//                            * seconds_per_bucket / tripwire_window_seconds)
//                    = floor(280 * 0.8 * 5 / 60) = 18
//
// `floor`, not round: the soft threshold must never be more permissive than the fraction
// says, or a client could sit above the intended rate indefinitely.
struct RateLimitConfig {
    bool   enabled                    = true;
    int    tripwire_window_seconds    = 60;
    int    max_requests_per_tripwire  = 280;
    int    seconds_per_bucket         = 5;
    double moving_average_fraction    = 0.8;   // of the tripwire rate
    int    moving_average_time_frames = 3;     // 3 x 60 s

    // The derived per-bucket limit. Not a field: computed, so it cannot drift.
    int PerBucketLimit() const;
};

// The only runtime input.
//
// A plain aggregate, so a game fills it directly. The injection pointers are not owned;
// null means "use the SDK default".
struct Config {
    // Required. There is no host to talk to without a domain key: the base URL is
    // https://<domain_key>.api.lootlocker.com.
    std::string domain_key;

    // The Game API key. Guest and white-label sessions take `game_key`; Steam takes
    // `game_api_key`. The naming difference is real and is in the OAS.
    std::string game_key;
    std::string game_api_key;

    // Sent on every session start. Must be semver.
    std::string game_version;

    // `is_development` is a header, separate from the URL — a second, independent
    // environment switch.
    bool is_development = false;

    // Empty means derive from `domain_key`.
    std::string base_url;

    // Optional overrides. Not owned. Null means "use the SDK default".
    Logger*         logger       = nullptr;   // default writes to stderr
    StateWriter*    state_writer = nullptr;   // default is FileStateWriter
    IHttpTransport* transport    = nullptr;   // default is the libcurl one

    // Empty means resolve it: the injected provider, then the OS, then Etc/UTC (D26).
    //
    // Set this when the game has its own timezone setting — a settings menu, a profile
    // field, a platform API. The OS value is frequently not what the player chose.
    std::string timezone;

    // Supplies the player's timezone. Not owned. Null means "use the OS".
    //
    // This is a seam for the same reason `state_writer` is: timezone and locale are both
    // potentially user-configurable in a game, and every engine exposes them differently.
    // The SDK's default reads the OS, which is right for a plain application and often
    // wrong for a game whose player picked a timezone in a menu.
    TimezoneProvider* timezone_provider = nullptr;

    QueueConfig     queue;
    RateLimitConfig rate_limit;

    // The base URL to use, derived from `domain_key` when `base_url` is empty.
    std::string ResolveBaseUrl() const;
};

// Validate a config before any request is made.
//
// Returns an `InvalidConfig` error naming the first problem found, or an empty error when
// the config is usable. Called by the client's constructor, so a bad config fails at
// construction rather than on the first request.
Error ValidateConfig(const Config& config);

} // namespace lootlocker

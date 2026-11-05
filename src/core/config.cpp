#include <lootlocker/config.h>
#include <lootlocker/result.h>

#include <cmath>
#include <cstdio>

namespace lootlocker {

namespace {

// A numeric identifier: non-empty, all digits, no leading zeros beyond "0" itself.
bool IsNumericIdentifier(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return s.size() == 1 || s[0] != '0';
}

// A dot-separated list of numeric identifiers, at least one.
bool IsNumericIdentifierList(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    std::size_t i = 0;
    while (i <= s.size()) {
        const std::size_t dot = s.find('.', i);
        const std::string part =
            s.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (!IsNumericIdentifier(part)) {
            return false;
        }
        if (dot == std::string::npos) {
            break;
        }
        i = dot + 1;
    }
    return true;
}

// A dot-separated list of identifiers: alphanumerics and hyphens, non-empty, no leading
// zero on a purely numeric identifier.
bool IsIdentifierList(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    std::size_t i = 0;
    while (i <= s.size()) {
        const std::size_t dot = s.find('.', i);
        const std::string part =
            s.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (part.empty()) {
            return false;
        }
        bool all_digits = true;
        for (const char c : part) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                            || (c >= 'A' && c <= 'Z') || c == '-';
            if (!ok) {
                return false;
            }
            if (c < '0' || c > '9') {
                all_digits = false;
            }
        }
        if (all_digits && !IsNumericIdentifier(part)) {
            return false;
        }
        if (dot == std::string::npos) {
            break;
        }
        i = dot + 1;
    }
    return true;
}

// Strict SemVer 2.0.0: MAJOR.MINOR.PATCH, optionally -PRERELEASE and +BUILD.
//
// The pre-release and build portions are validated rather than stripped. Stripping them
// accepted "1.2.3-", "1.2.3+" and "1.2.3-rc..1", which are not valid SemVer.
bool IsSemver(const std::string& v)
{
    if (v.empty()) {
        return false;
    }

    std::string rest = v;

    // Build metadata: everything after the first '+'. Must be a non-empty identifier list.
    const std::size_t plus = rest.find('+');
    if (plus != std::string::npos) {
        const std::string build = rest.substr(plus + 1);
        if (!IsIdentifierList(build)) {
            return false;
        }
        rest = rest.substr(0, plus);
    }

    // Pre-release: everything after the first '-'. Must be a non-empty identifier list.
    const std::size_t dash = rest.find('-');
    if (dash != std::string::npos) {
        const std::string pre = rest.substr(dash + 1);
        if (!IsIdentifierList(pre)) {
            return false;
        }
        rest = rest.substr(0, dash);
    }

    // The core must be exactly three numeric identifiers.
    int parts = 0;
    std::size_t i = 0;
    while (i <= rest.size()) {
        const std::size_t dot = rest.find('.', i);
        const std::string part =
            rest.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (!IsNumericIdentifier(part)) {
            return false;
        }
        ++parts;
        if (dot == std::string::npos) {
            break;
        }
        i = dot + 1;
    }

    return parts == 3;
}

} // namespace

int RateLimitConfig::PerBucketLimit() const
{
    if (tripwire_window_seconds <= 0) {
        return 0;
    }
    const double raw = static_cast<double>(max_requests_per_tripwire) * moving_average_fraction
                       * static_cast<double>(seconds_per_bucket)
                       / static_cast<double>(tripwire_window_seconds);
    // floor, not round: the soft threshold must never be more permissive than the
    // fraction says.
    return static_cast<int>(std::floor(raw));
}

std::string Config::ResolveBaseUrl() const
{
    if (!base_url.empty()) {
        return base_url;
    }
    if (domain_key.empty()) {
        return std::string();
    }
    return "https://" + domain_key + ".api.lootlocker.com";
}

namespace {

Error Invalid(const std::string& message)
{
    Error e;
    e.code = ErrorCode::InvalidConfig;
    e.message = message;
    return e;
}

} // namespace

Error ValidateConfig(const Config& config)
{
    // --- Required identity -------------------------------------------------------------

    if (config.domain_key.empty()) {
        return Invalid("domain_key is required: the base URL is derived from it");
    }

    if (config.game_key.empty() && config.game_api_key.empty()) {
        return Invalid("one of game_key (guest/white label) or game_api_key (Steam) is required");
    }

    if (config.game_version.empty()) {
        return Invalid("game_version is required and must be semver");
    }

    if (!IsSemver(config.game_version)) {
        return Invalid("game_version must be semver (MAJOR.MINOR.PATCH), got \""
                       + config.game_version + "\"");
    }

    // --- Queue policy ------------------------------------------------------------------
    //
    // Every field is checked, not just the two that were obvious. A negative retry count
    // or timeout is as unusable as a zero queue size, and the contract says bad policy is
    // rejected before dispatch.

    if (config.queue.max_retries < 0) {
        return Invalid("queue.max_retries must not be negative");
    }
    if (config.queue.backoff_factor < 1) {
        return Invalid("queue.backoff_factor must be at least 1");
    }
    if (config.queue.initial_backoff_ms < 0) {
        return Invalid("queue.initial_backoff_ms must not be negative");
    }
    if (config.queue.max_in_flight <= 0) {
        return Invalid("queue.max_in_flight must be positive");
    }
    if (config.queue.max_queue_size <= 0) {
        return Invalid("queue.max_queue_size must be positive");
    }
    if (config.queue.choke_warning_threshold < 0) {
        return Invalid("queue.choke_warning_threshold must not be negative");
    }
    if (config.queue.choke_warning_threshold > config.queue.max_queue_size) {
        return Invalid("queue.choke_warning_threshold must not exceed queue.max_queue_size");
    }
    if (config.queue.request_timeout_seconds <= 0) {
        return Invalid("queue.request_timeout_seconds must be positive");
    }

    // --- Rate limiter policy -----------------------------------------------------------

    if (config.rate_limit.enabled) {
        if (config.rate_limit.tripwire_window_seconds <= 0) {
            return Invalid("rate_limit.tripwire_window_seconds must be positive");
        }
        if (config.rate_limit.max_requests_per_tripwire <= 0) {
            return Invalid("rate_limit.max_requests_per_tripwire must be positive");
        }
        if (config.rate_limit.seconds_per_bucket <= 0) {
            return Invalid("rate_limit.seconds_per_bucket must be positive");
        }
        if (config.rate_limit.seconds_per_bucket > config.rate_limit.tripwire_window_seconds) {
            return Invalid(
                "rate_limit.seconds_per_bucket must not exceed tripwire_window_seconds");
        }
        if (config.rate_limit.moving_average_time_frames <= 0) {
            return Invalid("rate_limit.moving_average_time_frames must be positive");
        }

        // The fraction must be finite and in (0, 1]. A NaN or an out-of-range value would
        // make PerBucketLimit() produce a nonsense threshold.
        const double f = config.rate_limit.moving_average_fraction;
        if (!std::isfinite(f) || f <= 0.0 || f > 1.0) {
            return Invalid("rate_limit.moving_average_fraction must be finite and in (0, 1]");
        }

        // The derived threshold must be usable. A fraction small enough to floor to 0 would
        // throttle every request forever.
        if (config.rate_limit.PerBucketLimit() <= 0) {
            return Invalid("rate_limit settings derive a per-bucket limit of 0; raise "
                           "max_requests_per_tripwire or moving_average_fraction");
        }
    }

    return Error{};
}

} // namespace lootlocker



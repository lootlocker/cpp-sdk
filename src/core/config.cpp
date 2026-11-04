#include <lootlocker/config.h>
#include <lootlocker/result.h>

#include <cmath>
#include <cstdio>

namespace lootlocker {

namespace {

// A semver check that is deliberately strict about the shape and lenient about the
// numbers: MAJOR.MINOR.PATCH, each a non-empty run of digits, no leading zeros beyond a
// single "0". Pre-release and build metadata are accepted because the API accepts them.
bool IsSemver(const std::string& v)
{
    if (v.empty()) {
        return false;
    }

    // Split off build metadata and pre-release, which we do not validate beyond presence.
    std::string core = v;
    const std::size_t plus = core.find('+');
    if (plus != std::string::npos) {
        core = core.substr(0, plus);
    }
    const std::size_t dash = core.find('-');
    if (dash != std::string::npos) {
        core = core.substr(0, dash);
    }

    int parts = 0;
    std::size_t i = 0;
    while (i <= core.size()) {
        const std::size_t dot = core.find('.', i);
        const std::string part =
            core.substr(i, dot == std::string::npos ? std::string::npos : dot - i);

        if (part.empty()) {
            return false;
        }
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        // No leading zeros, except "0" itself.
        if (part.size() > 1 && part[0] == '0') {
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

Error ValidateConfig(const Config& config)
{
    Error e;

    if (config.domain_key.empty()) {
        e.code = ErrorCode::InvalidConfig;
        e.message = "domain_key is required: the base URL is derived from it";
        return e;
    }

    if (config.game_key.empty() && config.game_api_key.empty()) {
        e.code = ErrorCode::InvalidConfig;
        e.message = "one of game_key (guest/white label) or game_api_key (Steam) is required";
        return e;
    }

    if (config.game_version.empty()) {
        e.code = ErrorCode::InvalidConfig;
        e.message = "game_version is required and must be semver";
        return e;
    }

    if (!IsSemver(config.game_version)) {
        e.code = ErrorCode::InvalidConfig;
        e.message = "game_version must be semver (MAJOR.MINOR.PATCH), got \"" + config.game_version
                    + "\"";
        return e;
    }

    if (config.queue.max_in_flight <= 0) {
        e.code = ErrorCode::InvalidConfig;
        e.message = "queue.max_in_flight must be positive";
        return e;
    }

    if (config.queue.max_queue_size <= 0) {
        e.code = ErrorCode::InvalidConfig;
        e.message = "queue.max_queue_size must be positive";
        return e;
    }

    if (config.rate_limit.enabled && config.rate_limit.tripwire_window_seconds <= 0) {
        e.code = ErrorCode::InvalidConfig;
        e.message = "rate_limit.tripwire_window_seconds must be positive when the limiter is on";
        return e;
    }

    return e;
}

} // namespace lootlocker

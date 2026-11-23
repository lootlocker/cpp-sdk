#include "rate_limiter.h"

#include <algorithm>

namespace lootlocker {
namespace core {

namespace {

// The bucket count and the per-bucket limit are derived from the config rather than stored,
// so the fraction and the effective limit cannot disagree. `floor`, not round: the soft
// threshold must never be more permissive than the fraction says, or a client could sit
// above the intended rate indefinitely.
//
// Returns 0 for a configuration that cannot produce a usable window. That includes one whose
// product would overflow `int`: a large `moving_average_time_frames` with a large bucket
// count would otherwise wrap negative, and a negative bucket count takes the "disabled" fast
// path - silently turning the limiter off for a config that looked valid.
int DeriveBucketCount(const RateLimitConfig& config)
{
    if (config.seconds_per_bucket <= 0 || config.tripwire_window_seconds <= 0
        || config.moving_average_time_frames <= 0) {
        return 0;
    }

    // Reject a non-divisible policy rather than silently shortening the window. 60 s with
    // 7 s buckets would otherwise become an effective 56 s tripwire and a 168 s moving
    // average, while the config still advertises 60/180 - a different rate limit than the
    // adopter asked for, with nothing to say so.
    if (config.tripwire_window_seconds % config.seconds_per_bucket != 0) {
        return 0;
    }

    const int buckets_per_tripwire = config.tripwire_window_seconds / config.seconds_per_bucket;
    if (buckets_per_tripwire <= 0) {
        return 0;
    }

    // Checked before multiplying, not after: the multiply itself is the undefined behaviour.
    const int max_int = 2147483647;
    if (buckets_per_tripwire > max_int / config.moving_average_time_frames) {
        return 0;
    }

    return buckets_per_tripwire * config.moving_average_time_frames;
}

} // namespace

RateLimiter::RateLimiter(const RateLimitConfig& config, Clock& clock)
    : m_config(config), m_clock(clock)
{
    m_bucket_count = DeriveBucketCount(config);
    if (m_bucket_count > 0) {
        m_buckets_per_tripwire = config.tripwire_window_seconds / config.seconds_per_bucket;
        m_per_bucket_limit = config.PerBucketLimit();
        m_buckets.assign(static_cast<std::size_t>(m_bucket_count), 0);
    }
}

void RateLimiter::Reset()
{
    std::fill(m_buckets.begin(), m_buckets.end(), 0);
    m_last_bucket = -1;
    m_last_bucket_change_ms = 0;
    m_started = false;
    m_total = 0;
    m_total_in_tripwire = 0;
    m_limited = false;
    m_resolves_at_ms = 0;
}

void RateLimiter::Advance(std::int64_t now_ms)
{
    if (m_bucket_count <= 0) {
        return;
    }

    const std::int64_t bucket_ms = static_cast<std::int64_t>(m_config.seconds_per_bucket) * 1000;

    // The first call initialises bucket 0 rather than trying to measure elapsed time from a
    // sentinel.
    if (!m_started) {
        m_started = true;
        m_last_bucket = 0;
        m_last_bucket_change_ms = now_ms;
        return;
    }

    const std::int64_t elapsed = now_ms - m_last_bucket_change_ms;
    if (elapsed < bucket_ms) {
        return;
    }

    const std::int64_t moved = elapsed / bucket_ms;

    // Snap to the bucket boundary rather than the raw detection time, so sub-bucket
    // remainder cannot accumulate as drift over a long run.
    m_last_bucket_change_ms += moved * bucket_ms;

    // More buckets elapsed than exist: everything has aged out, so clear rather than
    // looping over a window that is entirely stale.
    if (moved >= m_bucket_count) {
        std::fill(m_buckets.begin(), m_buckets.end(), 0);
        m_total = 0;
        m_total_in_tripwire = 0;
        m_last_bucket = static_cast<int>(moved % m_bucket_count);
        return;
    }

    for (std::int64_t step = 1; step <= moved; ++step) {
        const int bucket = static_cast<int>((m_last_bucket + step) % m_bucket_count);

        // The bucket exactly one tripwire window behind this one is sliding out of the
        // tripwire window, so its count leaves that total.
        int leaving_tripwire = bucket - m_buckets_per_tripwire;
        if (leaving_tripwire < 0) {
            leaving_tripwire += m_bucket_count;
        }

        m_total_in_tripwire -= m_buckets[static_cast<std::size_t>(leaving_tripwire)];
        m_total -= m_buckets[static_cast<std::size_t>(bucket)];
        m_buckets[static_cast<std::size_t>(bucket)] = 0;
    }

    m_last_bucket = static_cast<int>((m_last_bucket + moved) % m_bucket_count);
}

bool RateLimiter::WouldTrip() const
{
    // Tripwire: this request would take the window to the cap.
    if (m_total_in_tripwire + 1 > m_config.max_requests_per_tripwire) {
        return true;
    }

    // Moving average: the per-bucket average across the whole window would exceed the
    // derived limit. Integer division, matching the reference implementations.
    const int projected = m_total + 1;
    return (projected / m_bucket_count) > m_per_bucket_limit;
}

bool RateLimiter::CanAcquire()
{
    if (!m_config.enabled || m_bucket_count <= 0) {
        return true;
    }

    const std::int64_t now = m_clock.NowMs();
    Advance(now);

    // While limited, nothing is counted. A held request has not been sent, so counting it
    // would keep the limiter tripped by its own backlog - the window would never drain.
    if (m_limited) {
        if (m_total <= 0) {
            m_limited = false;
            m_resolves_at_ms = 0;
        } else {
            return false;
        }
    }

    if (WouldTrip()) {
        m_limited = true;

        // The limit clears once everything currently counted has aged out of the window.
        // Derived from the current bucket boundary plus the full window, so the estimate
        // does not drift with sub-bucket remainder.
        const std::int64_t window_ms =
            static_cast<std::int64_t>(m_bucket_count) * m_config.seconds_per_bucket * 1000;
        m_resolves_at_ms = m_last_bucket_change_ms + window_ms;
        return false;
    }

    return true;
}

void RateLimiter::Commit()
{
    if (!m_config.enabled || m_bucket_count <= 0) {
        return;
    }
    m_buckets[static_cast<std::size_t>(m_last_bucket)]++;
    m_total++;
    m_total_in_tripwire++;
}

bool RateLimiter::TryAcquire()
{
    if (!CanAcquire()) {
        return false;
    }
    Commit();
    return true;
}

int RateLimiter::SecondsUntilClear() const
{
    if (!m_limited) {
        return 0;
    }
    const std::int64_t remaining = m_resolves_at_ms - m_clock.NowMs();
    if (remaining <= 0) {
        return 0;
    }
    // Round up, so a caller that waits this long is not still limited by a fraction of a
    // second.
    return static_cast<int>((remaining + 999) / 1000);
}

} // namespace core
} // namespace lootlocker

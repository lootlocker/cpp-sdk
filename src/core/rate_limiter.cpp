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

        // A fixed-size array, allocated once. `std::atomic` is neither copyable nor movable,
        // so a vector cannot hold it.
        m_buckets.reset(new std::atomic<int>[static_cast<std::size_t>(m_bucket_count)]);
        for (int i = 0; i < m_bucket_count; ++i) {
            m_buckets[static_cast<std::size_t>(i)].store(0, std::memory_order_relaxed);
        }
    }
}

void RateLimiter::Reset()
{
        for (int i = 0; i < m_bucket_count; ++i) {
            m_buckets[static_cast<std::size_t>(i)].store(0, std::memory_order_relaxed);
        }
    m_last_bucket.store(-1, std::memory_order_relaxed);
    m_ms_into_bucket.store(0, std::memory_order_relaxed);
    m_started.store(false, std::memory_order_relaxed);
    m_total.store(0, std::memory_order_relaxed);
    m_total_in_tripwire.store(0, std::memory_order_relaxed);
    m_limited.store(false, std::memory_order_relaxed);
    m_ms_until_clear.store(0, std::memory_order_relaxed);
}

void RateLimiter::Tick(std::int64_t delta_ms)
{
    if (m_bucket_count <= 0) {
        return;
    }

    // The first tick initialises bucket 0, even with a zero delta. Without this the limiter
    // would stay unstarted until time actually passed, and `Commit` would write to bucket -1.
    if (!m_started.load(std::memory_order_relaxed)) {
        m_started.store(true, std::memory_order_relaxed);
        m_last_bucket.store(0, std::memory_order_relaxed);
        m_ms_into_bucket.store(0, std::memory_order_relaxed);
        return;
    }

    if (delta_ms <= 0) {
        return;
    }

    // The countdown to clearing is a duration, so it simply decreases. Clamped at zero so a
    // long idle period cannot make it negative and then take a whole window to climb back.
    if (m_limited.load(std::memory_order_relaxed)) {
        std::int64_t remaining = m_ms_until_clear.load(std::memory_order_relaxed) - delta_ms;
        if (remaining < 0) {
            remaining = 0;
        }
        m_ms_until_clear.store(remaining, std::memory_order_relaxed);
    }

    SlideWindow(delta_ms);
}

void RateLimiter::SlideWindow(std::int64_t delta_ms)
{
    const std::int64_t bucket_ms = static_cast<std::int64_t>(m_config.seconds_per_bucket) * 1000;

    const std::int64_t ms_into_bucket =
        m_ms_into_bucket.load(std::memory_order_relaxed) + delta_ms;

    const std::int64_t numberOfBucketsTraversed = ms_into_bucket / bucket_ms;
    if (numberOfBucketsTraversed == 0) {
        m_ms_into_bucket.store(ms_into_bucket, std::memory_order_relaxed);
        return;
    }

    // Keep only the remainder, so sub-bucket time carries forward and cannot accumulate as
    // drift over a long run.
    m_ms_into_bucket.store(ms_into_bucket - numberOfBucketsTraversed * bucket_ms,
                           std::memory_order_relaxed);

    const int last_bucket = m_last_bucket.load(std::memory_order_relaxed);

    // More buckets elapsed than exist: everything has aged out, so clear rather than looping
    // over a window that is entirely stale.
    if (numberOfBucketsTraversed >= m_bucket_count) {
        for (int i = 0; i < m_bucket_count; ++i) {
            m_buckets[static_cast<std::size_t>(i)].store(0, std::memory_order_relaxed);
        }
        m_total.store(0, std::memory_order_relaxed);
        m_total_in_tripwire.store(0, std::memory_order_relaxed);
        m_last_bucket.store(static_cast<int>(numberOfBucketsTraversed % m_bucket_count),
                            std::memory_order_relaxed);
        return;
    }

    int total = m_total.load(std::memory_order_relaxed);
    int total_in_tripwire = m_total_in_tripwire.load(std::memory_order_relaxed);

    for (std::int64_t step = 1; step <= numberOfBucketsTraversed; ++step) {
        const int bucket = static_cast<int>((last_bucket + step) % m_bucket_count);

        // The bucket exactly one tripwire window behind this one is sliding out of the
        // tripwire window, so its count leaves that total.
        int leaving_tripwire = bucket - m_buckets_per_tripwire;
        if (leaving_tripwire < 0) {
            leaving_tripwire += m_bucket_count;
        }

        total_in_tripwire -= m_buckets[static_cast<std::size_t>(leaving_tripwire)].load(
            std::memory_order_relaxed);
        total -= m_buckets[static_cast<std::size_t>(bucket)].load(std::memory_order_relaxed);
        m_buckets[static_cast<std::size_t>(bucket)].store(0, std::memory_order_relaxed);
    }

    m_total.store(total, std::memory_order_relaxed);
    m_total_in_tripwire.store(total_in_tripwire, std::memory_order_relaxed);
    m_last_bucket.store(static_cast<int>((last_bucket + numberOfBucketsTraversed) % m_bucket_count),
                        std::memory_order_relaxed);
}

bool RateLimiter::WouldTrip() const
{
    // Tripwire: this request would take the window to the cap.
    if (m_total_in_tripwire.load(std::memory_order_relaxed) + 1
        > m_config.max_requests_per_tripwire) {
        return true;
    }

    // Moving average: the per-bucket average across the whole window would exceed the
    // derived limit. Integer division, matching the reference implementations.
    const int projected = m_total.load(std::memory_order_relaxed) + 1;
    return (projected / m_bucket_count) > m_per_bucket_limit;
}

bool RateLimiter::CanAcquire() const
{
    if (!m_config.enabled || m_bucket_count <= 0) {
        return true;
    }

    // While limited, nothing is counted. A held request has not been sent, so counting it
    // would keep the limiter tripped by its own backlog - the window would never drain.
    if (m_limited.load(std::memory_order_relaxed)) {
        return m_total.load(std::memory_order_relaxed) <= 0;
    }

    return !WouldTrip();
}

void RateLimiter::Commit()
{
    if (!m_config.enabled || m_bucket_count <= 0) {
        return;
    }

    // Defensive: `Tick` initialises bucket 0, so this cannot happen through the executor.
    // But writing to `m_buckets[-1]` is a stack overrun rather than a wrong number, so the
    // guard is worth the branch.
    if (!m_started.load(std::memory_order_relaxed)) {
        return;
    }

    // The window has drained, so the limit is lifted. `IsLimited()` derives from `m_total`
    // as well, so this is belt-and-braces rather than the only place it clears.
    if (m_limited.load(std::memory_order_relaxed)
        && m_total.load(std::memory_order_relaxed) <= 0) {
        m_limited.store(false, std::memory_order_relaxed);
        m_ms_until_clear.store(0, std::memory_order_relaxed);
    }

    const int last_bucket = m_last_bucket.load(std::memory_order_relaxed);
    m_buckets[static_cast<std::size_t>(last_bucket)].fetch_add(1, std::memory_order_relaxed);
    m_total.fetch_add(1, std::memory_order_relaxed);
    m_total_in_tripwire.fetch_add(1, std::memory_order_relaxed);

    // `IsLimited()` reports whether the *next* request would be refused, which is what
    // `CanAcquire()` will say. Recording it here keeps the two in agreement: without this,
    // `CanAcquire()` would refuse while `IsLimited()` still said false.
    //
    // The request that *reaches* the cap is allowed - the cap is "more than 280", not "280" -
    // so this is `WouldTrip()` after the increment, not before it.
    if (WouldTrip()) {
        m_limited.store(true, std::memory_order_relaxed);

        // The limit clears once everything currently counted has aged out of the window,
        // plus whatever is left of the current bucket.
        const std::int64_t window_ms =
            static_cast<std::int64_t>(m_bucket_count) * m_config.seconds_per_bucket * 1000;
        const std::int64_t bucket_ms =
            static_cast<std::int64_t>(m_config.seconds_per_bucket) * 1000;
        m_ms_until_clear.store(
            window_ms + (bucket_ms - m_ms_into_bucket.load(std::memory_order_relaxed)),
            std::memory_order_relaxed);
    }
}

int RateLimiter::SecondsUntilClear() const
{
    if (!IsLimited()) {
        return 0;
    }
    const std::int64_t remaining = m_ms_until_clear.load(std::memory_order_relaxed);
    if (remaining <= 0) {
        return 0;
    }
    // Round up, so a caller that waits this long is not still limited by a fraction of a
    // second.
    return static_cast<int>((remaining + 999) / 1000);
}

} // namespace core
} // namespace lootlocker
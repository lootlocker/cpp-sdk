#pragma once

// The client-side rate limiter (D25). Internal: `lootlocker::core::` is not on an adopter's
// include path (D18).
//
// A dual-threshold sliding window, ported from the Unreal and Unity SDKs, which share the
// algorithm. Two thresholds, because one is not enough:
//
//   Tripwire      - hard cap: more than `max_requests_per_tripwire` in any rolling
//                   `tripwire_window_seconds` window.
//   Moving average - soft cap: the per-bucket average across the whole window exceeds
//                   `PerBucketLimit()`, i.e. sustained traffic above
//                   `moving_average_fraction` of the tripwire rate.
//
// The soft cap exists because a pure tripwire lets a client sit at 279 requests a minute
// for ever: abusive, but never tripping. The moving average catches sustained near-limit
// behaviour that the tripwire cannot see.
//
// **This is client-side self-restraint, not error handling.** A tripped request is held and
// sent later, never dropped and never reported as an error. A server-side 429 is a different
// thing entirely and is handled by the executor's retry policy - conflating the two is how
// the other SDKs ended up with a limiter that also tries to be a retry policy.

#include <lootlocker/config.h>

#include "clock.h"

#include <cstdint>
#include <vector>

namespace lootlocker {
namespace core {

class RateLimiter {
public:
    RateLimiter(const RateLimitConfig& config, Clock& clock);

    RateLimiter(const RateLimiter&)            = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    // Whether a request may be sent now. Does not record anything.
    //
    // Split from `Commit` so the executor can check *before* starting a transfer and record
    // only if the transfer actually started. `IHttpTransport::Start` may return 0, and a
    // start that failed sent nothing - counting it would burn quota for a request that never
    // left, and repeated failures could hold unrelated queued requests.
    bool CanAcquire();
    // Records one request against the window. Call only after `CanAcquire()` returned true
    // and the transfer actually started.
    void Commit();

    // Records one request and returns true when it may be sent now.
    //
    // Convenience for a caller that has nothing between the check and the send. The executor
    // does not use it, because it has a `Start` call in between.
    bool TryAcquire();

    // True while the limiter is holding requests back.
    bool IsLimited() const { return m_limited; }

    // Seconds until the limit is expected to clear. 0 when not limited.
    //
    // An estimate, not a promise: it is derived from when the oldest counted request ages
    // out of the window, and the executor uses it only to decide when to look again.
    int SecondsUntilClear() const;

    // Requests counted in the tripwire window, for tests and logs.
    int TripwireCount() const { return m_total_in_tripwire; }

    // Requests counted across the whole moving-average window, for tests and logs.
    int WindowCount() const { return m_total; }

    // Drops all state. For test isolation and for a client that is being reset.
    void Reset();

private:
    // Advances the bucket pointer to reflect elapsed time, zeroing the buckets that have
    // slid out of the window and removing their counts from the running totals.
    void Advance(std::int64_t now_ms);

    // Whether adding one more request would exceed either threshold.
    bool WouldTrip() const;

    RateLimitConfig m_config;
    Clock&          m_clock;

    // Derived from the config at construction, so the arithmetic cannot disagree with
    // itself later.
    int m_bucket_count = 0;
    int m_buckets_per_tripwire = 0;
    int m_per_bucket_limit = 0;

    std::vector<int> m_buckets;

    // Index of the most recently written bucket. -1 before the first request.
    int m_last_bucket = -1;

    // When the current bucket became active, snapped to a bucket boundary so sub-bucket
    // remainder cannot accumulate as drift.
    std::int64_t m_last_bucket_change_ms = 0;
    bool         m_started = false;

    // Requests across every bucket in the array.
    int m_total = 0;

    // Requests in the buckets that fall inside the current tripwire window.
    int m_total_in_tripwire = 0;

    bool         m_limited = false;
    std::int64_t m_resolves_at_ms = 0;
};

} // namespace core
} // namespace lootlocker

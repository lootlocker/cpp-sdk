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

#include <atomic>
#include <memory>
#include <cstdint>
#include <vector>

namespace lootlocker {
namespace core {

// The memory order used throughout the limiter.
//
// Relaxed is deliberate: the requirement is that each field is read and written atomically,
// not that other threads observe any particular order between them. A stronger order would
// buy nothing here and cost on the hot path.
//
// Named once so the intent is stated in one place rather than repeated at every access, and
// so a future change to the ordering is one edit rather than forty.
constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

class RateLimiter {
public:
    RateLimiter(const RateLimitConfig& config, Clock& clock);

    RateLimiter(const RateLimiter&)            = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    // Advances the window by `delta_ms`. Called by the executor's tick, once per `Update()`,
    // before anything is promoted.
    //
    // **Time is pushed in, not read here.** The limiter used to call the clock itself from
    // inside `CanAcquire()`, which made a `const`-looking predicate mutate the whole module -
    // and meant the window only advanced when someone happened to ask. A tickable component
    // should be told how much time passed and do nothing else; the executor owns the clock
    // and is the one place that reads it.
    void Tick(std::int64_t delta_ms);

    // Whether a request may be sent now. **Const**: it reads the window and reports, and
    // does not advance or record anything.
    //
    // Split from `Commit` so the executor can check *before* starting a transfer and record
    // only if the transfer actually started. `IHttpTransport::Start` may return 0, and a
    // start that failed sent nothing - counting it would burn quota for a request that never
    // left, and repeated failures could hold unrelated queued requests.
    bool CanAcquire() const;

    // Records one request against the window. Call only after `CanAcquire()` returned true
    // and the transfer actually started.
    void Commit();

    // True while the limiter is holding requests back.
    //
    // Derived rather than stored: a stored flag has to be cleared by something, and the only
    // method that runs after a successful check is `Commit` - so a caller that only ever
    // checked would see a stale `true` for ever. Deriving it means `IsLimited()` and
    // `CanAcquire()` cannot disagree.
    bool IsLimited() const { return m_limited && m_total > 0; }

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
    // Zeroes the buckets that have slid out of the window and removes their counts from the
    // running totals.
    void SlideWindow(std::int64_t delta_ms);

    // Whether adding one more request would exceed either threshold. Used by `CanAcquire()`
    // before anything is recorded, and by `Commit()` after, so the two agree.
    bool WouldTrip() const;

    RateLimitConfig m_config;
    Clock&          m_clock;

    // Derived from the config at construction, so the arithmetic cannot disagree with
    // itself later.
    int m_bucket_count = 0;
    int m_buckets_per_tripwire = 0;
    int m_per_bucket_limit = 0;

    // **Atomics, because the SDK is single-threaded by contract but not by enforcement.**
    // `Update()` is documented as the only entry point and callbacks fire on the calling
    // thread (D5), so in a correct program nothing here is contended. But a game that calls
    // `Update()` from two threads by mistake would otherwise corrupt the window silently,
    // and a torn read of `m_total` is a wrong rate limit rather than a crash - the kind of
    // bug that shows up as "the limiter randomly trips" months later.
    //
    // Relaxed ordering throughout: the requirement is that each field is read and written
    // atomically, not that other threads observe any particular order between them. A
    // stronger order would buy nothing here and cost on the hot path.
    //
    // Explicit locks are deliberately not used. There is no invariant that spans two fields
    // - a torn read of `m_total` against `m_total_in_tripwire` gives a slightly stale
    // decision, not a corrupt one - so a mutex would add contention to buy nothing.
    // A heap array rather than a `std::vector`: `std::atomic` is neither copyable nor
    // movable, so a vector cannot grow or reallocate. The size is fixed at construction, so
    // an array is the right shape anyway.
    std::unique_ptr<std::atomic<int>[]> m_buckets;

    // Index of the most recently written bucket. -1 before the first tick.
    std::atomic<int> m_last_bucket{-1};

    // Milliseconds accumulated since the current bucket became active. Kept as a remainder
    // rather than an absolute timestamp, because the limiter is told deltas and never reads
    // the clock: sub-bucket remainder carries forward, so it cannot accumulate as drift.
    std::atomic<std::int64_t> m_ms_into_bucket{0};
    std::atomic<bool>         m_started{false};

    // Requests across every bucket in the array.
    std::atomic<int> m_total{0};

    // Requests in the buckets that fall inside the current tripwire window.
    std::atomic<int> m_total_in_tripwire{0};

    std::atomic<bool> m_limited{false};

    // Milliseconds until the limit is expected to clear, counted down by `Tick`. A remaining
    // duration rather than a deadline, for the same reason: no clock reads.
    std::atomic<std::int64_t> m_ms_until_clear{0};
};

} // namespace core
} // namespace lootlocker

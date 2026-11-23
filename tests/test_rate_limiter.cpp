// The rate limiter (T11a, D25).
//
// Driven by a fake clock, so every timing assertion is exact and nothing sleeps. The
// algorithm is ported from the Unreal and Unity SDKs, so the tests are written against the
// behaviour those implementations have, not against a re-derivation of it.

#include <lootlocker/config.h>

#include "clock.h"
#include "rate_limiter.h"
#include "test_harness.h"

#include <cstdint>

using namespace lootlocker;
using namespace lootlocker::core;

namespace {

class FakeClock : public Clock {
public:
    std::int64_t NowMs() override { return m_now; }
    void Advance(std::int64_t ms) { m_now += ms; }
    void AdvanceSeconds(int seconds) { m_now += static_cast<std::int64_t>(seconds) * 1000; }

private:
    std::int64_t m_now = 1000000;   // non-zero, so "not set" is distinguishable
};

RateLimitConfig Defaults()
{
    RateLimitConfig config;
    config.enabled = true;
    config.tripwire_window_seconds = 60;
    config.max_requests_per_tripwire = 280;
    config.seconds_per_bucket = 5;
    config.moving_average_fraction = 0.8;
    config.moving_average_time_frames = 3;
    return config;
}

// The derived values the defaults must produce, asserted once so a change to the derivation
// is visible rather than silently shifting every threshold.
void TestDerivedConstants()
{
    const RateLimitConfig config = Defaults();

    // 280 * 0.8 * 5 / 60 = 18.67, floored to 18.
    CHECK_EQ_INT(config.PerBucketLimit(), 18);
}

// --- The tripwire ----------------------------------------------------------------------

void TestBurstTripsTheTripwire()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    // 280 requests in one bucket is allowed: the cap is "more than 280".
    int allowed = 0;
    for (int i = 0; i < 280; ++i) {
        if (limiter.TryAcquire()) {
            ++allowed;
        }
    }
    CHECK_EQ_INT(allowed, 280);
    CHECK(!limiter.IsLimited());

    // The 281st trips it.
    CHECK(!limiter.TryAcquire());
    CHECK(limiter.IsLimited());
    CHECK_EQ_INT(limiter.TripwireCount(), 280);
}

void TestTripwireClearsWhenTheWindowSlides()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    for (int i = 0; i < 280; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.TryAcquire());
    CHECK(limiter.IsLimited());

    // The tripwire window is 60 s, but the limiter does not clear when the tripwire window
    // slides - it clears when every counted request has aged out of the *moving-average*
    // window, which is 36 buckets of 5 s = 180 s. That is the reference behaviour in both
    // the Unreal and Unity SDKs, and it is deliberately conservative: a burst that tripped
    // the hard cap backs the client off for the full window rather than resuming the
    // instant the 60 s count drops.
    clock.AdvanceSeconds(59);
    CHECK(!limiter.TryAcquire());
    CHECK(limiter.IsLimited());

    clock.AdvanceSeconds(60);
    CHECK(!limiter.TryAcquire());
    CHECK(limiter.IsLimited());

    // Past the full window, the counted requests have aged out and the limiter clears.
    clock.AdvanceSeconds(62);
    CHECK(limiter.TryAcquire());
    CHECK(!limiter.IsLimited());
}

// --- The moving average ----------------------------------------------------------------

void TestSustainedTrafficTripsTheMovingAverage()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    // 18 requests per 5 s bucket is exactly the derived limit, so it must NOT trip: the
    // check is "average exceeds 18", not "reaches 18".
    //
    // 36 buckets of 5 s is the whole window, so this is 3 minutes of traffic at exactly the
    // soft cap.
    for (int bucket = 0; bucket < 36; ++bucket) {
        for (int i = 0; i < 18; ++i) {
            CHECK(limiter.TryAcquire());
        }
        clock.AdvanceSeconds(5);
    }
    CHECK(!limiter.IsLimited());

    // One more per bucket pushes the average over 18 and trips the soft cap, even though
    // the tripwire (280 per 60 s) is nowhere near.
    for (int bucket = 0; bucket < 36; ++bucket) {
        for (int i = 0; i < 19; ++i) {
            if (!limiter.TryAcquire()) {
                // Tripped partway through, which is the expected outcome.
                CHECK(limiter.IsLimited());
                return;
            }
        }
        clock.AdvanceSeconds(5);
    }

    // Reaching here means it never tripped, which is the failure.
    CHECK(limiter.IsLimited());
}

void TestMovingAverageTripsBelowTheTripwire()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    // 19 per bucket is 228 per 60 s, comfortably under the 280 tripwire. The soft cap is
    // what catches it, which is the whole reason the soft cap exists.
    bool tripped = false;
    for (int bucket = 0; bucket < 36 && !tripped; ++bucket) {
        for (int i = 0; i < 19; ++i) {
            if (!limiter.TryAcquire()) {
                tripped = true;
                break;
            }
        }
        clock.AdvanceSeconds(5);
    }

    CHECK(tripped);
    CHECK(limiter.IsLimited());

    // And the tripwire count never reached its cap, so it was the average that caught it.
    CHECK(limiter.TripwireCount() < 280);
}

// --- A tripped request is held, not dropped --------------------------------------------

void TestHeldRequestsAreNotCounted()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    for (int i = 0; i < 280; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.TryAcquire());
    CHECK_EQ_INT(limiter.TripwireCount(), 280);

    // A held request must not count against the window, or the limiter would keep itself
    // tripped by its own backlog and the window would never drain.
    for (int i = 0; i < 100; ++i) {
        CHECK(!limiter.TryAcquire());
    }
    CHECK_EQ_INT(limiter.TripwireCount(), 280);

    // So it still clears on schedule - the full moving-average window, not the tripwire
    // window.
    clock.AdvanceSeconds(181);
    CHECK(limiter.TryAcquire());
}

void TestSecondsUntilClearIsAnEstimateThatExpires()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    CHECK_EQ_INT(limiter.SecondsUntilClear(), 0);

    for (int i = 0; i < 280; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.TryAcquire());

    // The window is 3 minutes (36 buckets of 5 s), so the estimate is bounded by that.
    const int estimate = limiter.SecondsUntilClear();
    CHECK(estimate > 0);
    CHECK(estimate <= 180);

    // Waiting the estimate out clears it.
    clock.AdvanceSeconds(estimate);
    CHECK(limiter.TryAcquire());
    CHECK_EQ_INT(limiter.SecondsUntilClear(), 0);
}

// --- Disabled, and the config edge cases -----------------------------------------------

void TestDisabledLimiterAlwaysAllows()
{
    RateLimitConfig config = Defaults();
    config.enabled = false;

    FakeClock clock;
    RateLimiter limiter(config, clock);

    // Far past every threshold, and still allowed.
    for (int i = 0; i < 10000; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.IsLimited());
}

void TestResetClearsEverything()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    for (int i = 0; i < 280; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.TryAcquire());
    CHECK(limiter.IsLimited());

    limiter.Reset();

    CHECK(!limiter.IsLimited());
    CHECK_EQ_INT(limiter.TripwireCount(), 0);
    CHECK_EQ_INT(limiter.WindowCount(), 0);
    CHECK(limiter.TryAcquire());
}

void TestLongIdlePeriodClearsWithoutIterating()
{
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    for (int i = 0; i < 280; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.TryAcquire());

    // A day of idleness is far more buckets than exist. The limiter must clear rather than
    // loop over a window that is entirely stale.
    clock.AdvanceSeconds(86400);
    CHECK(limiter.TryAcquire());
    CHECK(!limiter.IsLimited());
    CHECK_EQ_INT(limiter.TripwireCount(), 1);
}

void TestCustomConstantsAreHonoured()
{
    // The constants are policy, not a constexpr in a header, so a different policy must
    // work. 10 per 10 s, one bucket per 10 s, one frame.
    RateLimitConfig config;
    config.enabled = true;
    config.tripwire_window_seconds = 10;
    config.max_requests_per_tripwire = 10;
    config.seconds_per_bucket = 10;
    config.moving_average_fraction = 1.0;
    config.moving_average_time_frames = 1;
    FakeClock clock;
    RateLimiter limiter(config, clock);

    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (limiter.TryAcquire()) {
            ++allowed;
        }
    }
    CHECK_EQ_INT(allowed, 10);
    CHECK(!limiter.TryAcquire());

    clock.AdvanceSeconds(11);
    CHECK(limiter.TryAcquire());
}

void TestNonDivisiblePolicyIsRejected()
{
    // 60 s with 7 s buckets does not divide. Silently shortening it would give an effective
    // 56 s tripwire and a 168 s moving average while the config still advertises 60/180 -
    // a different rate limit than the adopter asked for, with nothing to say so.
    RateLimitConfig config = Defaults();
    config.seconds_per_bucket = 7;

    FakeClock clock;
    RateLimiter limiter(config, clock);

    // Rejected means the limiter is inert rather than silently misconfigured. It allows
    // everything, which is the safe direction: a broken policy must not throttle a game.
    for (int i = 0; i < 1000; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.IsLimited());
}

void TestOverflowingBucketCountIsRejected()
{
    // A large frame count with a large bucket count would overflow `int` on the multiply and
    // wrap negative - and a negative bucket count takes the "disabled" fast path, silently
    // turning the limiter off for a config that looked valid.
    RateLimitConfig config = Defaults();
    config.tripwire_window_seconds = 2000000000;
    config.seconds_per_bucket = 1;
    config.moving_average_time_frames = 100;

    FakeClock clock;
    RateLimiter limiter(config, clock);

    // Inert rather than negative, and no huge allocation.
    for (int i = 0; i < 1000; ++i) {
        CHECK(limiter.TryAcquire());
    }
    CHECK(!limiter.IsLimited());
}
void TestCommitIsSeparateFromTheCheck()
{
    // The executor checks, starts the transfer, and only then commits - because a `Start`
    // that returned 0 sent nothing. This is the primitive that makes that possible.
    FakeClock clock;
    RateLimiter limiter(Defaults(), clock);

    CHECK(limiter.CanAcquire());
    CHECK_EQ_INT(limiter.TripwireCount(), 0);   // checking alone records nothing

    limiter.Commit();
    CHECK_EQ_INT(limiter.TripwireCount(), 1);

    // A check that is never committed leaves the window untouched, so a failed start cannot
    // burn quota.
    for (int i = 0; i < 500; ++i) {
        CHECK(limiter.CanAcquire());
    }
    CHECK_EQ_INT(limiter.TripwireCount(), 1);
}

} // namespace

int main()
{
    TestDerivedConstants();
    TestBurstTripsTheTripwire();
    TestTripwireClearsWhenTheWindowSlides();
    TestSustainedTrafficTripsTheMovingAverage();
    TestMovingAverageTripsBelowTheTripwire();
    TestHeldRequestsAreNotCounted();
    TestSecondsUntilClearIsAnEstimateThatExpires();
    TestDisabledLimiterAlwaysAllows();
    TestResetClearsEverything();
    TestLongIdlePeriodClearsWithoutIterating();
    TestCustomConstantsAreHonoured();
    TestNonDivisiblePolicyIsRejected();
    TestOverflowingBucketCountIsRejected();
    TestCommitIsSeparateFromTheCheck();

    return testing::Report();
}

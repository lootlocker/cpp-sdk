#pragma once

// The clock seam (D25, R26). Internal: `lootlocker::core::` is not on an adopter's include
// path (D18).
//
// The executor needs time for two things: back-off delays and request timeouts. Both are
// behaviour a test has to observe, and observing them against a wall clock means sleeping,
// which makes a suite slow and flaky. So time is read through this interface and a test
// advances a fake instead.
//
// This is deliberately NOT a `Config` seam. The four public seams exist because an adopter
// has to dock to their own ecosystem (challenge #5); nobody has asked to supply time, and
// `steady_clock` is monotonic on every platform we target. A seam with no consumer is a
// seam nobody maintains (D18). If a host with a fixed-timestep loop ever needs to drive
// time, that is a decision to take then.

#include <chrono>
#include <cstdint>

namespace lootlocker {
namespace core {

class Clock {
public:
    virtual ~Clock() = default;

    // Monotonic milliseconds. Never wall-clock: a system clock adjustment must not make a
    // back-off fire early or a timeout fire late.
    virtual std::int64_t NowMs() = 0;
};

class SteadyClock : public Clock {
public:
    std::int64_t NowMs() override
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

} // namespace core
} // namespace lootlocker

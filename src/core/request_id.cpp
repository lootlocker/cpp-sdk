#include <lootlocker/request_id.h>

#include <atomic>

namespace lootlocker {

namespace {

// Process-wide, so two clients in one process cannot mint the same id (D20).
//
// Relaxed ordering is enough: the only requirement is that each call gets a distinct value,
// not that other threads observe any particular order. `fetch_add` is atomic regardless of
// the memory order, so this is a single uncontended instruction on the hot path.
std::atomic<std::uint64_t> g_next_request_id{1};

} // namespace

RequestId RequestId::Next()
{
    // A CAS loop rather than `fetch_add` plus a fix-up, so that "never 0" is an invariant of
    // the loop rather than a special case bolted on afterwards.
    //
    // The `fetch_add` version was not actually racy: `fetch_add` hands out each value
    // exactly once, so only one thread could ever observe 0 and take the next value. But
    // that reasoning needs a paragraph to justify, and a loop that visibly skips 0 needs
    // none. The cost is one compare-exchange on an uncontended atomic, the same order as the
    // `fetch_add` it replaces.
    std::uint64_t current = g_next_request_id.load(std::memory_order_relaxed);
    for (;;) {
        // Skip 0, which is the invalid id, and wrap from the maximum back to 1 rather than
        // to 0. `current + 1` alone would store 0 at the wrap, and the *next* call would
        // then return `RequestId(0)` - which is indistinguishable from a rejected
        // submission and would collide with that map key.
        //
        // Unreachable in practice - a uint64 at a million requests a second lasts longer
        // than the hardware - but the loop makes the invariant true by construction rather
        // than by argument.
        const std::uint64_t next = (current == 0 || current == UINT64_MAX) ? 1 : current + 1;

        if (g_next_request_id.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
            return RequestId(current);
        }
        // `current` was reloaded by the failed exchange; loop and retry.
    }
}

std::string RequestId::ToString() const
{
    // Hand-rolled rather than std::to_string: this is called once per request for the
    // header and once per log line, and std::to_string goes through the locale-aware
    // formatting machinery for a value that is always plain decimal digits.
    if (m_value == 0) {
        return std::string("0");
    }

    char buffer[20];   // 2^64 - 1 is 20 digits
    std::size_t length = 0;
    std::uint64_t value = m_value;
    while (value != 0) {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    std::string out;
    out.reserve(length);
    while (length > 0) {
        out.push_back(buffer[--length]);
    }
    return out;
}

} // namespace lootlocker

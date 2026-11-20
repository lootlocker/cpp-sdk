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
    std::uint64_t value = g_next_request_id.fetch_add(1, std::memory_order_relaxed);

    // Skip 0, which is the invalid id. Unreachable in practice - a uint64 at a million
    // requests a second lasts longer than the hardware - but one comparison removes the
    // edge case entirely rather than leaving a wrap that would silently produce an invalid
    // id.
    if (value == 0) {
        value = g_next_request_id.fetch_add(1, std::memory_order_relaxed);
    }
    return RequestId(value);
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

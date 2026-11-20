#pragma once

// The id every async call returns (D25).
//
// A value type wrapping a `uint64`, not a string: a string id would allocate on every call,
// which is exactly the kind of thing a game SDK should not do on a hot path. It is the same
// id the queue uses internally and the same id sent as `LL-Request-Id`, so a support ticket
// can be matched to a server log line.
//
// It is also what `Cancel(RequestId)` will take when cancellation lands, so the return type
// does not have to change for that.
//
// **The counter is process-wide, not per-client.** Two clients in one process are legal
// (D20), and a per-client counter would let both mint id 5 - so a support ticket quoting
// "request 5" would be ambiguous. Minting from one static counter makes the id unique
// across the process, which is the whole point of a correlation id.

#include <cstdint>
#include <string>

namespace lootlocker {

class RequestId {
public:
    // Invalid. `IsValid()` is false, and it is what a rejected submission returns.
    RequestId() = default;

    explicit RequestId(std::uint64_t value) : m_value(value) {}

    // Mint the next id. Process-wide and monotonic, so ids are unique across every client
    // in the process and increasing in submission order.
    //
    // Never returns an invalid id: 0 is skipped, so a wrap cannot produce one. The wrap is
    // unreachable in practice - a `uint64` at a million requests a second lasts longer than
    // the hardware - but skipping 0 costs one comparison and removes the edge case
    // entirely.
    static RequestId Next();

    std::uint64_t Value() const { return m_value; }

    // False for the default-constructed id, which is what a rejected submission returns.
    // Ids start at 1, so 0 is unambiguous.
    bool IsValid() const { return m_value != 0; }

    // For logs and for the `LL-Request-Id` header. Allocates, so not on a hot path.
    std::string ToString() const;

    bool operator==(const RequestId& other) const { return m_value == other.m_value; }
    bool operator!=(const RequestId& other) const { return m_value != other.m_value; }

    // Ordering exists so the queue can keep FIFO order among requests that are equally
    // ready, and so an id can be a map key. Because ids are minted monotonically, this is
    // also submission order within one client.
    //
    // It is deliberately not a documented guarantee: the other SDKs mint a GUID and have no
    // ordering at all, and nothing in the public surface should come to depend on it.
    bool operator<(const RequestId& other) const { return m_value < other.m_value; }

private:
    std::uint64_t m_value = 0;
};

} // namespace lootlocker

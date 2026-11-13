#pragma once

// A scriptable transport for tests (T10).
//
// Public rather than test-only, because an adopter writing their own tests against the SDK
// needs the same thing, and because the executor's tests are not the only place a fake is
// useful. It is header-only and has no dependency beyond the interface.

#include <lootlocker/http_transport.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace lootlocker {

// A transport that never touches the network.
//
// Transfers complete only when the test says so, which is what makes the pollable contract
// testable without sleeping: `Start` records the request, `Poll` reports "still running"
// until `Complete` is called, and `Cancel` records that it happened.
class FakeHttpTransport : public IHttpTransport {
public:
    TransferId Start(const Request& request) override
    {
        Transfer transfer;
        transfer.id = ++m_next_id;
        transfer.request = request;
        m_transfers[transfer.id] = transfer;
        m_order.push_back(transfer.id);
        return transfer.id;
    }

    bool Poll(TransferId id, Response& out) override
    {
        ++m_poll_count;

        const auto it = m_transfers.find(id);
        if (it == m_transfers.end()) {
            return false;
        }
        if (it->second.cancelled || !it->second.completed) {
            return false;
        }

        out = it->second.response;
        return true;
    }

    void Cancel(TransferId id) override
    {
        const auto it = m_transfers.find(id);
        if (it != m_transfers.end()) {
            it->second.cancelled = true;
        }
    }

    // --- test control -------------------------------------------------------

    // Finish a transfer with a response. The next `Poll` for it returns true.
    void Complete(TransferId id, const Response& response)
    {
        const auto it = m_transfers.find(id);
        if (it != m_transfers.end()) {
            it->second.response = response;
            it->second.completed = true;
        }
    }

    // Finish a transfer with a status and body, which is the common case.
    void Complete(TransferId id, int status_code, const std::string& body)
    {
        Response response;
        response.status_code = status_code;
        response.body = body;
        Complete(id, response);
    }

    // Fail a transfer the way a transport failure looks: no response at all.
    void Fail(TransferId id) { Complete(id, Response()); }

    // The request a transfer was started with, or nullptr when the id is unknown.
    const Request* FindRequest(TransferId id) const
    {
        const auto it = m_transfers.find(id);
        return it == m_transfers.end() ? nullptr : &it->second.request;
    }

    bool WasCancelled(TransferId id) const
    {
        const auto it = m_transfers.find(id);
        return it != m_transfers.end() && it->second.cancelled;
    }

    bool WasCompleted(TransferId id) const
    {
        const auto it = m_transfers.find(id);
        return it != m_transfers.end() && it->second.completed;
    }

    std::size_t StartCount() const { return m_order.size(); }
    std::size_t PollCount() const { return m_poll_count; }

    // The requests in the order they were started, for asserting on what was sent.
    std::deque<TransferId> Order() const { return m_order; }

    void Reset()
    {
        m_transfers.clear();
        m_order.clear();
        m_poll_count = 0;
    }

private:
    struct Transfer {
        TransferId id = 0;
        Request    request;
        Response   response;
        bool       cancelled = false;
        bool       completed = false;
    };

    TransferId                     m_next_id = 0;
    std::map<TransferId, Transfer> m_transfers;
    std::deque<TransferId>         m_order;
    std::size_t                    m_poll_count = 0;
};

} // namespace lootlocker

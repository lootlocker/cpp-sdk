#pragma once

// The request executor (D25). Internal: `lootlocker::core::` is not on an adopter's include
// path (D18).
//
// This is the queue state machine. It owns three things the transport does not: retry with
// back-off, timeouts, and the concurrency cap. It is driven entirely by `Update()`, which
// does a frame's worth of work and never waits on the network (D5).
//
// The rate limiter (T11a) is deliberately NOT here yet. It is a separate concern with its
// own clock arithmetic, and folding it in now would mean writing the queue twice.

#include <lootlocker/config.h>
#include <lootlocker/header_map.h>
#include <lootlocker/http_transport.h>
#include <lootlocker/log.h>
#include <lootlocker/request_id.h>
#include <lootlocker/result.h>

#include "clock.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lootlocker {
namespace core {

// One request, as the executor sees it.
//
// Deliberately not `IHttpTransport::Request`: the transport's shape is the wire contract,
// and this carries the policy the transport has no business knowing about.
struct PendingRequest {
    std::string method;
    std::string url;
    HeaderMap   headers;
    std::string body;
};

// The outcome of one request, delivered exactly once.
struct Completion {
    // 0 when no response arrived, which is how a transport failure is told apart from a
    // 4xx (D23).
    int         status_code = 0;
    HeaderMap   headers;
    std::string body;

    // `code` is `None` on success. On failure it is the coarse kind, and `status_code`
    // carries the HTTP status when there was one.
    Error error;

    // How many attempts were made in total, 1-based. A caller that sees 3 knows the
    // request was retried twice.
    int attempts = 1;
};

using CompletionHandler = std::function<void(const Completion&)>;

class Executor {
public:
    Executor(const QueueConfig& config, std::weak_ptr<IHttpTransport> transport, Clock& clock,
             std::weak_ptr<Logger> logger);

    // The same, plus the config error the client found. A non-empty error makes every
    // submission reject with it, which is how an inert client still delivers its handlers.
    Executor(const QueueConfig& config, std::weak_ptr<IHttpTransport> transport, Clock& clock,
             std::weak_ptr<Logger> logger, Error config_error);
    ~Executor();

    Executor(const Executor&)            = delete;
    Executor& operator=(const Executor&) = delete;

    // Queues a request and returns its id. Never invokes the handler synchronously: the
    // contract is that callbacks fire on the thread that calls `Update()` (D5), so a
    // rejected request is delivered on the next `Update()` too.
    //
    // Returns an invalid `RequestId` when the request was rejected outright, which happens
    // when the queue is full and `deny_when_backed_up` is set, or when the config is
    // invalid. The handler still runs, with `QueueFull` or `InvalidConfig`.
    //
    // **Every submission is its own request. There is no deduplication or grouping**
    // (owner, 2026-11-19). Two identical submissions are two requests, and both are sent.
    // The reason is that the API is not guaranteed to be idempotent: "add 5 gold" sent
    // five times must yield 25, not 5, and the SDK cannot tell that case apart from a
    // harmless repeated read. The Unity SDK built this and then turned it off for the same
    // reason.
    RequestId Submit(PendingRequest request, CompletionHandler on_done);

    // A frame's worth of work. Returns false when nothing is queued and nothing is in
    // flight, which is what lets a loop-less program terminate.
    bool Update();

    // Replaces the transport. The executor holds a `weak_ptr` and locks it per use, so a
    // transport that dies mid-flight is detected rather than dereferenced.
    //
    // **Refused while anything is in flight**, and returns false when it refuses. An
    // in-flight entry holds only a transfer id, which is meaningful to the transport that
    // issued it and to no other: swapping the transport underneath would poll and cancel
    // that id on the new one, and the old transfer would never be cancelled. Draining
    // first is the caller's job, because only the caller knows whether waiting is
    // acceptable.
    bool SetTransport(std::weak_ptr<IHttpTransport> transport);

    // The transport in use. Empty when the client has none, which happens when curl was
    // not built in and nothing was injected.
    std::weak_ptr<IHttpTransport> GetTransport() const;

    std::size_t QueuedCount() const;
    std::size_t InFlightCount() const;

    // Cancels every in-flight transfer and drops the queue without invoking handlers.
    // Called from the destructor, where invoking a handler would be a callback into a
    // half-destroyed client.
    void Abandon();

private:
    struct Entry {
        RequestId         id;
        PendingRequest    request;

        // Every handler waiting on this request. More than one only when a submission was
        // deduplicated onto this entry, in which case all of them are called with the same
        // completion.
        std::vector<CompletionHandler> on_done;

        // Attempts started so far, 1-based once the first one begins.
        int attempts = 0;

        // Not before this time. Set on submit and on every retry.
        std::int64_t ready_at_ms = 0;

        std::int64_t                 started_at_ms = 0;
        IHttpTransport::TransferId   transfer = 0;
        bool                         in_flight = false;
    };

    // Decide whether a finished attempt is retried or delivered, and either requeue it or
    // move its handlers into the delivery lists. Shared by the poll path and the
    // start-failure path so the policy exists in exactly one place.
    void FinishOrRetry(RequestId id, Completion completion, std::int64_t now,
                       std::vector<CompletionHandler>& out_handlers,
                       std::vector<Completion>& out_completions);

    // Parse a `Retry-After` header into `error.retry_after_seconds`, clamping rather than
    // overflowing. Server-controlled input, so it is not trusted to be small.
    static void ParseRetryAfter(const std::string& value, Error& error);

    void Deliver(std::vector<CompletionHandler>& handlers, std::vector<Completion>& completions);

    void DrainRejected();

    void FailAll(const std::string& reason);

    void LogAttempt(const Entry& entry, const Completion& completion, std::int64_t duration_ms);

    std::int64_t TimeoutMs() const;
    std::int64_t BackoffMs(int attempts, const Completion& completion) const;

    // The longest delay a back-off may produce. A day is far beyond any sane back-off and
    // exists only to keep the arithmetic total.
    static constexpr std::int64_t kMaxBackoffMs = 86400000;

    QueueConfig                    m_config;
    Error                          m_config_error;
    std::weak_ptr<IHttpTransport>  m_transport;
    Clock&                         m_clock;
    std::weak_ptr<Logger>          m_logger;

    std::map<RequestId, Entry>     m_entries;

    // Handlers for requests rejected at submit time. Bounded by how many the caller
    // submits while the queue is full, and drained on the next `Update()`.
    //
    // `void()` rather than `CompletionHandler`: the completion is already decided, so
    // there is nothing left to pass.
    std::vector<std::function<void()>> m_rejected;
};

} // namespace core
} // namespace lootlocker

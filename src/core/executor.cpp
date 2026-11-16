#include "executor.h"

#include <algorithm>
#include <utility>

namespace lootlocker {
namespace core {

namespace {

Error MakeError(ErrorCode code, const std::string& message, int status_code = 0)
{
    Error e;
    e.code = code;
    e.status_code = status_code;
    e.message = message;
    return e;
}

// Whether a finished attempt may be retried at all, before the attempt budget is
// considered. This is the policy from D25, in one place.
bool IsRetryableStatus(int status_code)
{
    // 0 means no response arrived: a transport failure, which is retryable in principle.
    if (status_code == 0) {
        return true;
    }
    // 429 is the server asking us to come back later.
    if (status_code == 429) {
        return true;
    }
    // 5xx is the server failing, which may be transient.
    if (status_code >= 500 && status_code <= 599) {
        return true;
    }
    // Everything else, including every other 4xx, will be the same answer again. A 400
    // will be a 400 again.
    return false;
}

} // namespace

Executor::Executor(const QueueConfig& config, std::weak_ptr<IHttpTransport> transport,
                   Clock& clock, std::weak_ptr<Logger> logger)
    : m_config(config), m_transport(std::move(transport)), m_clock(clock),
      m_logger(std::move(logger))
{
}

Executor::~Executor()
{
    Abandon();
}

void Executor::SetTransport(std::weak_ptr<IHttpTransport> transport)
{
    m_transport = std::move(transport);
}

std::weak_ptr<IHttpTransport> Executor::GetTransport() const
{
    return m_transport;
}

std::size_t Executor::QueuedCount() const
{
    std::size_t queued = 0;
    for (const auto& entry : m_entries) {
        if (!entry.second.in_flight) {
            ++queued;
        }
    }
    return queued;
}

std::size_t Executor::InFlightCount() const
{
    std::size_t in_flight = 0;
    for (const auto& entry : m_entries) {
        if (entry.second.in_flight) {
            ++in_flight;
        }
    }
    return in_flight;
}

std::int64_t Executor::TimeoutMs() const
{
    return static_cast<std::int64_t>(m_config.request_timeout_seconds) * 1000;
}

std::int64_t Executor::BackoffMs(int attempts, const Completion& completion) const
{
    // A server that told us when to come back is obeyed exactly. Guessing at a back-off
    // when the server has stated the answer is how a client gets itself rate limited
    // harder.
    if (completion.error.retry_after_seconds > 0) {
        return static_cast<std::int64_t>(completion.error.retry_after_seconds) * 1000;
    }

    // initial * factor^(attempts-1), capped so a long retry chain cannot overflow into a
    // negative delay. The cap is a day, which is far beyond any sane back-off and exists
    // only to keep the arithmetic total.
    std::int64_t delay = m_config.initial_backoff_ms;
    for (int i = 1; i < attempts; ++i) {
        delay *= m_config.backoff_factor;
        if (delay > 86400000) {
            return 86400000;
        }
    }
    return delay;
}

std::uint64_t Executor::Submit(PendingRequest request, CompletionHandler on_done)
{
    const std::int64_t now = m_clock.NowMs();

    // The queue is full. `deny_when_backed_up` decides whether that is an error or a wait:
    // a game that would rather stall than drop a request leaves it false and the request
    // simply waits its turn.
    if (m_config.deny_when_backed_up
        && static_cast<int>(QueuedCount()) >= m_config.max_queue_size) {
        if (on_done) {
            Completion completion;
            completion.error = MakeError(ErrorCode::QueueFull,
                                         "the request queue is full; the request was not sent");
            m_rejected.push_back(
                [handler = std::move(on_done), completion]() { handler(completion); });
        }
        return 0;
    }

    Entry entry;
    entry.id = m_next_id++;
    entry.request = std::move(request);
    entry.on_done = std::move(on_done);
    entry.ready_at_ms = now;

    const std::uint64_t id = entry.id;
    m_entries.emplace(id, std::move(entry));
    return id;
}

void Executor::DrainRejected()
{
    if (m_rejected.empty()) {
        return;
    }
    // Moved out first: a handler is allowed to submit another request, which would
    // otherwise mutate the vector we are iterating.
    std::vector<std::function<void()>> rejected;
    rejected.swap(m_rejected);
    for (auto& handler : rejected) {
        handler();
    }
}

void Executor::Deliver(std::vector<CompletionHandler>& handlers,
                       std::vector<Completion>& completions)
{
    for (std::size_t i = 0; i < handlers.size(); ++i) {
        if (handlers[i]) {
            handlers[i](completions[i]);
        }
    }
}

void Executor::LogAttempt(const Entry& entry, const Completion& completion,
                          std::int64_t duration_ms)
{
    std::shared_ptr<Logger> logger = m_logger.lock();
    if (!logger) {
        return;
    }

    log::HttpEntry http;
    http.method = entry.request.method;
    http.url = entry.request.url;
    http.request_body = entry.request.body;
    http.response_body = completion.body;
    http.status_code = completion.status_code;
    http.duration_ms = duration_ms;
    http.attempt = entry.attempts;

    std::string trace_id;
    if (completion.headers.Get("LL-Trace-Id", trace_id)) {
        http.trace_id = trace_id;
    }

    // Through `LogExchange`, never `LogHttp` directly: that is the only path that
    // obfuscates both bodies first (D26, R28).
    log::LogExchange(*logger, http);
}

void Executor::FinishOrRetry(std::uint64_t id, Completion completion, std::int64_t now,
                             std::vector<CompletionHandler>& out_handlers,
                             std::vector<Completion>& out_completions)
{
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return;
    }
    Entry& entry = it->second;

    // --- Classify the outcome ---------------------------------------------------------

    if (completion.status_code == 0) {
        completion.error = MakeError(ErrorCode::Transport,
                                     "no response arrived: the transport failed");
    } else if (completion.status_code == 429) {
        completion.error = MakeError(ErrorCode::RateLimited, "the server rate limited us",
                                     completion.status_code);
    } else if (completion.status_code >= 400) {
        completion.error = MakeError(ErrorCode::Http, "the server returned an error status",
                                     completion.status_code);
    }

    // A `Retry-After` header is the server's own instruction, so it is read before the
    // retry decision rather than after.
    std::string retry_after;
    if (completion.headers.Get("Retry-After", retry_after)) {
        int seconds = 0;
        for (const char c : retry_after) {
            if (c < '0' || c > '9') {
                seconds = 0;
                break;
            }
            seconds = seconds * 10 + (c - '0');
        }
        completion.error.retry_after_seconds = seconds;
    }

    // --- Retry or deliver -------------------------------------------------------------

    const bool retryable = IsRetryableStatus(completion.status_code);

    // R25: a non-idempotent request whose response was lost is never retried. A POST that
    // timed out may have succeeded, and retrying it can double-grant. This is checked
    // before the attempt budget because it is a correctness rule, not a policy knob.
    const bool lost_response = completion.status_code == 0;
    const bool may_retry = retryable && !(lost_response && !entry.request.idempotent);

    if (may_retry && entry.attempts <= m_config.max_retries) {
        entry.in_flight = false;
        entry.transfer = 0;
        entry.ready_at_ms = now + BackoffMs(entry.attempts, completion);
        return;
    }

    // Delivered. The entry is erased before the handler runs, so a handler that submits a
    // new request cannot invalidate the map under us.
    CompletionHandler handler = std::move(entry.on_done);
    m_entries.erase(it);

    out_handlers.push_back(std::move(handler));
    out_completions.push_back(std::move(completion));
}

bool Executor::Update()
{
    DrainRejected();

    const std::int64_t now = m_clock.NowMs();

    std::vector<CompletionHandler> handlers;
    std::vector<Completion>        completions;

    // --- Phase 0: is there a transport at all? ----------------------------------------
    //
    // A dead transport is not a retryable condition: there is nothing to retry with, so
    // every queued and in-flight request would sit in the queue forever and `Update()`
    // would keep returning true. That is a hang, and a hang is worse than an error.
    //
    // This is the case the `weak_ptr` seam exists to make detectable (D3): the adopter's
    // transport died, so the SDK notices and fails the requests instead of dereferencing
    // freed memory or spinning.
    std::shared_ptr<IHttpTransport> transport = m_transport.lock();
    if (!transport) {
        if (!m_entries.empty()) {
            if (std::shared_ptr<Logger> logger = m_logger.lock()) {
                logger->Log(LogLevel::Error,
                            "no transport is available; queued and in-flight requests are "
                            "being failed");
            }
            FailAll("no transport is available");
        }
        return false;
    }

    // --- Phase 1: poll what is in flight ----------------------------------------------
    //
    // Collected first, then processed, because `FinishOrRetry` erases from `m_entries` and
    // a handler may submit. Iterating the map while either happens is the bug this
    // two-step exists to avoid.
    std::vector<std::uint64_t> finished;
    std::vector<std::uint64_t> timed_out;

    for (auto& pair : m_entries) {
        Entry& entry = pair.second;
        if (!entry.in_flight) {
            continue;
        }

        // The timeout is checked before the poll, so a transfer that never reports
        // completion is still bounded. A transport that ignores `Cancel` cannot hold a
        // request forever.
        if (now - entry.started_at_ms >= TimeoutMs()) {
            timed_out.push_back(entry.id);
            continue;
        }

        IHttpTransport::Response response;
        if (transport->Poll(entry.transfer, response)) {
            finished.push_back(entry.id);
        }
    }

    // --- Phase 2: collect completions -------------------------------------------------

    for (const std::uint64_t id : finished) {
        const auto it = m_entries.find(id);
        if (it == m_entries.end()) {
            continue;
        }
        Entry& entry = it->second;

        IHttpTransport::Response response;
        transport->Poll(entry.transfer, response);
        transport->Cancel(entry.transfer);

        Completion completion;
        completion.status_code = response.status_code;
        completion.headers = std::move(response.headers);
        completion.body = std::move(response.body);
        completion.attempts = entry.attempts;

        LogAttempt(entry, completion, now - entry.started_at_ms);
        FinishOrRetry(id, std::move(completion), now, handlers, completions);
    }

    for (const std::uint64_t id : timed_out) {
        const auto it = m_entries.find(id);
        if (it == m_entries.end()) {
            continue;
        }
        Entry& entry = it->second;

        if (entry.transfer != 0) {
            transport->Cancel(entry.transfer);
        }

        Completion completion;
        completion.status_code = 0;
        completion.attempts = entry.attempts;
        completion.error = MakeError(ErrorCode::Transport, "the request timed out");

        LogAttempt(entry, completion, now - entry.started_at_ms);
        FinishOrRetry(id, std::move(completion), now, handlers, completions);
    }

    // --- Phase 3: promote what is ready -----------------------------------------------
    //
    // After polling, so a slot freed by a completion in this same `Update()` is used in
    // this same `Update()`. That is what makes one call drain a frame's worth of work
    // rather than one item.

    {
        std::vector<std::uint64_t> ready;
        for (const auto& pair : m_entries) {
            const Entry& entry = pair.second;
            if (!entry.in_flight && entry.ready_at_ms <= now) {
                ready.push_back(entry.id);
            }
        }
        // Id order, so the queue is FIFO among requests that are equally ready.
        std::sort(ready.begin(), ready.end());

        for (const std::uint64_t id : ready) {
            if (static_cast<int>(InFlightCount()) >= m_config.max_in_flight) {
                break;
            }

            const auto it = m_entries.find(id);
            if (it == m_entries.end()) {
                continue;
            }
            Entry& entry = it->second;

            IHttpTransport::Request request;
            request.method = entry.request.method;
            request.url = entry.request.url;
            request.headers = entry.request.headers;
            request.body = entry.request.body;
            request.timeout_seconds = m_config.request_timeout_seconds;

            ++entry.attempts;
            entry.started_at_ms = now;

            const IHttpTransport::TransferId transfer = transport->Start(request);
            if (transfer == 0) {
                // The transport refused to start. Treated as a transport failure rather
                // than a request that is still running, so the retry policy decides.
                Completion completion;
                completion.status_code = 0;
                completion.attempts = entry.attempts;
                completion.error = MakeError(ErrorCode::Transport,
                                             "the transport could not start the request");
                FinishOrRetry(id, std::move(completion), now, handlers, completions);
                continue;
            }

            entry.transfer = transfer;
            entry.in_flight = true;
        }
    }

    // --- Phase 4: deliver -------------------------------------------------------------
    //
    // Last, so every handler sees a queue that has already been advanced. A handler that
    // submits a request gets it started on the next `Update()`, not recursively inside
    // this one.

    Deliver(handlers, completions);

    return !m_entries.empty() || !m_rejected.empty();
}

void Executor::FailAll(const std::string& reason)
{
    std::vector<CompletionHandler> handlers;
    std::vector<Completion>        completions;

    for (auto& pair : m_entries) {
        Entry& entry = pair.second;
        if (entry.on_done) {
            Completion completion;
            completion.status_code = 0;
            completion.attempts = entry.attempts;
            // `Transport`, not `Cancelled`: the request did not fail because anyone
            // cancelled it, it failed because there is no way to send it. An adopter
            // branching on the code needs to tell "I cancelled this" from "the SDK could
            // not send this".
            completion.error = MakeError(ErrorCode::Transport, reason);
            handlers.push_back(std::move(entry.on_done));
            completions.push_back(std::move(completion));
        }
    }
    m_entries.clear();

    Deliver(handlers, completions);
}

void Executor::Abandon()
{
    std::shared_ptr<IHttpTransport> transport = m_transport.lock();
    if (transport) {
        for (auto& pair : m_entries) {
            if (pair.second.in_flight && pair.second.transfer != 0) {
                transport->Cancel(pair.second.transfer);
            }
        }
    }

    // Handlers are dropped, not invoked. This runs from the destructor, where calling back
    // into a half-destroyed client is worse than silence.
    m_entries.clear();
    m_rejected.clear();
}

} // namespace core
} // namespace lootlocker

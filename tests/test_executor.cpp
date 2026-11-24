// The request executor's state machine (T11).
//
// Driven by a fake clock and the public `FakeHttpTransport`, so nothing here sleeps and
// every timing assertion is exact. The end-to-end proof that the real transport works is
// in `test_transport_e2e.cpp`; this file is about the queue's own behaviour.

#include <lootlocker/fake_http_transport.h>
#include <lootlocker/log.h>

#include "clock.h"
#include "executor.h"
#include "test_harness.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lootlocker;
using namespace lootlocker::core;

namespace {

// A clock the test advances by hand. Time never moves on its own, so a back-off either
// fires exactly when it should or the test fails.
class FakeClock : public Clock {
public:
    std::int64_t NowMs() override { return m_now; }
    void Advance(std::int64_t ms) { m_now += ms; }
    void AdvanceSeconds(int seconds) { m_now += static_cast<std::int64_t>(seconds) * 1000; }

private:
    std::int64_t m_now = 1000;   // non-zero, so "not set" is distinguishable
};

// A logger that records what it was given, so the obfuscation path can be asserted on.
class RecordingLogger : public Logger {
public:
    void Log(LogLevel level, const std::string& message) override
    {
        levels.push_back(level);
        messages.push_back(message);
    }

    void LogHttp(const log::HttpEntry& entry) override { entries.push_back(entry); }

    std::vector<LogLevel>          levels;
    std::vector<std::string>       messages;
    std::vector<log::HttpEntry>    entries;
};

QueueConfig DefaultQueue()
{
    QueueConfig config;
    config.max_retries = 5;
    config.backoff_factor = 2;
    config.initial_backoff_ms = 50;
    config.max_in_flight = 50;
    config.max_queue_size = 5000;
    config.request_timeout_seconds = 300;
    return config;
}

PendingRequest Get(const std::string& url)
{
    PendingRequest request;
    request.method = "GET";
    request.url = url;
    return request;
}

PendingRequest Post(const std::string& url)
{
    PendingRequest request;
    request.method = "POST";
    request.url = url;
    request.body = "{}";
    return request;
}

// A harness that owns the pieces in the right order, so the executor's reference to the
// clock stays valid.
struct Fixture {
    explicit Fixture(QueueConfig config = DefaultQueue())
        : transport(std::make_shared<FakeHttpTransport>()), logger(std::make_shared<RecordingLogger>())
    {
        ExecutorSettings settings;
        settings.queue = config;
        settings.transport = transport;
        settings.clock = &clock;
        settings.logger = logger;
        executor.reset(new Executor(settings));
    }

    std::shared_ptr<FakeHttpTransport> transport;
    std::shared_ptr<RecordingLogger>   logger;
    FakeClock                          clock;
    std::unique_ptr<Executor>          executor;
};

// --- Update() drains a frame's worth of work -------------------------------------------

void TestOneUpdateDrainsSeveralCompletions()
{
    Fixture f;

    int delivered = 0;
    std::vector<RequestId> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(f.executor->Submit(Get("https://example.test/a"),
                                         [&delivered](const Completion&) { ++delivered; }));
    }

    CHECK_EQ_INT(static_cast<long long>(ids.size()), 3);
    CHECK(ids[0].IsValid() && ids[1].IsValid() && ids[2].IsValid());

    // One Update starts all three, because max_in_flight is 50.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);
    CHECK_EQ_INT(static_cast<long long>(f.executor->InFlightCount()), 3);

    // Complete all three, then a single Update must deliver all three. This is the
    // "frame's worth of work, not one item" contract (D5).
    const auto order = f.transport->Order();
    CHECK_EQ_INT(static_cast<long long>(order.size()), 3);
    for (const auto transfer : order) {
        f.transport->Complete(transfer, 200, "{}");
    }

    CHECK(!f.executor->Update());
    CHECK_EQ_INT(delivered, 3);
    CHECK_EQ_INT(static_cast<long long>(f.executor->InFlightCount()), 0);
    CHECK_EQ_INT(static_cast<long long>(f.executor->QueuedCount()), 0);
}

// --- Update() returns false exactly when there is nothing left -------------------------

void TestUpdateReturnsFalseOnlyWhenEmpty()
{
    Fixture f;

    // Nothing submitted: nothing to do.
    CHECK(!f.executor->Update());

    bool delivered = false;
    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"),
                           [&delivered](const Completion&) { delivered = true; });
    CHECK(id.IsValid());

    // Queued, so there is work.
    CHECK(f.executor->Update());
    CHECK(!delivered);

    f.transport->Complete(f.transport->Order().back(), 200, "{}");

    // The call that delivers the last completion also reports that nothing is left. A
    // `while (Update())` loop therefore terminates after the callback has fired, which is
    // what lets a loop-less program exit.
    CHECK(!f.executor->Update());
    CHECK(delivered);
}

// --- Retry policy ----------------------------------------------------------------------

void TestServerErrorRetriesWithExponentialBackoff()
{
    Fixture f;

    int delivered = 0;
    Completion last;
    f.executor->Submit(Get("https://example.test/a"), [&](const Completion& c) {
        ++delivered;
        last = c;
    });

    // Attempt 1.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);
    f.transport->Complete(f.transport->Order().back(), 500, "boom");

    // The 500 is retryable, so it is requeued with a 50 ms delay rather than delivered.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(delivered, 0);
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    // Not yet: 49 ms is not enough.
    f.clock.Advance(49);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    // 50 ms is. Attempt 2.
    f.clock.Advance(1);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);

    // Attempt 2 fails too, so the delay doubles to 100 ms.
    f.transport->Complete(f.transport->Order().back(), 500, "boom");
    CHECK(f.executor->Update());
    f.clock.Advance(99);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
    f.clock.Advance(1);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);

    // Attempt 3 succeeds, so it is delivered with the attempt count.
    f.transport->Complete(f.transport->Order().back(), 200, "{\"ok\":true}");
    CHECK(!f.executor->Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.Ok());
    CHECK_EQ_INT(last.status_code, 200);
    CHECK_EQ_INT(last.attempts, 3);
    CHECK_EQ(last.body, std::string("{\"ok\":true}"));
}

void TestClientErrorDoesNotRetry()
{
    Fixture f;

    int delivered = 0;
    Completion last;
    const RequestId id = f.executor->Submit(
        Get("https://example.test/a"), [&](const Completion& c) {
            ++delivered;
            last = c;
        });

    CHECK(f.executor->Update());
    f.transport->Complete(f.transport->Order().back(), 400, "{\"error\":\"bad\"}");

    // A 400 will be a 400 again, so it is delivered on the first Update after it arrives.
    CHECK(!f.executor->Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);
    CHECK(last.error.code == ErrorCode::Http);
    CHECK_EQ_INT(last.status_code, 400);
    CHECK_EQ_INT(last.attempts, 1);

    // The body survives on the failure, which is what makes a 400 diagnosable (D7, D23).
    CHECK_EQ(last.body, std::string("{\"error\":\"bad\"}"));
}

void TestRetryAfterIsHonoured()
{
    Fixture f;

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 429;
    response.body = "{}";
    response.headers.Set("Retry-After", "7");
    f.transport->Complete(f.transport->Order().back(), response);

    CHECK(f.executor->Update());

    // The server said 7 seconds, so the back-off is 7000 ms, not the 50 ms default.
    f.clock.Advance(6999);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    f.clock.Advance(1);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

void TestRetriesStopAtTheBudget()
{
    QueueConfig config = DefaultQueue();
    config.max_retries = 2;   // 1 initial attempt + 2 retries = 3 total
    Fixture f(config);

    int delivered = 0;
    Completion last;
    f.executor->Submit(Get("https://example.test/a"), [&](const Completion& c) {
        ++delivered;
        last = c;
    });

    // Two full retry cycles: each starts an attempt, fails it, and requeues.
    for (int attempt = 0; attempt < 2; ++attempt) {
        CHECK(f.executor->Update());
        f.transport->Complete(f.transport->Order().back(), 503, "unavailable");
        CHECK(f.executor->Update());
        CHECK_EQ_INT(delivered, 0);
        f.clock.Advance(1000);
    }

    // The third attempt is the last the budget allows, so its failure is delivered rather
    // than requeued.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);
    f.transport->Complete(f.transport->Order().back(), 503, "unavailable");
    CHECK(!f.executor->Update());

    CHECK_EQ_INT(delivered, 1);
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);
    CHECK(last.error.code == ErrorCode::Http);
    CHECK_EQ_INT(last.attempts, 3);
}

// --- A lost response is retried, whatever the method (owner, 2026-11-20) ---------------
//
// R25 used to be closed here: a non-idempotent request whose response was lost was never
// retried, because a POST that timed out may have succeeded. The owner's call is that the
// other SDKs do not do this and we should not either, so R25 is now an accepted risk. These
// tests assert the new behaviour, and they exist so the change is visible rather than
// silent.

void TestTimedOutPostIsRetried()
{
    Fixture f;

    int delivered = 0;
    f.executor->Submit(Post("https://example.test/grant"), [&](const Completion& c) {
        ++delivered;
        (void)c;
    });

    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    // The response never arrives. It is retried like any other transport failure - which
    // means a POST that actually succeeded may be sent twice. That is the accepted risk.
    f.clock.Advance(300 * 1000);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(delivered, 0);

    f.clock.Advance(50);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

void TestTimedOutGetIsRetried()
{
    Fixture f;

    int delivered = 0;
    f.executor->Submit(Get("https://example.test/broadcasts"), [&](const Completion& c) {
        ++delivered;
        (void)c;
    });

    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    f.clock.Advance(300 * 1000);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(delivered, 0);

    f.clock.Advance(50);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

void TestServerErrorOnPostIsRetried()
{
    Fixture f;

    int delivered = 0;
    f.executor->Submit(Post("https://example.test/grant"),
                       [&](const Completion&) { ++delivered; });

    CHECK(f.executor->Update());

    f.transport->Complete(f.transport->Order().back(), 500, "boom");
    CHECK(f.executor->Update());
    CHECK_EQ_INT(delivered, 0);

    f.clock.Advance(50);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

// --- The concurrency cap and the queue cap ---------------------------------------------

void TestConcurrencyCapIsRespected()
{
    QueueConfig config = DefaultQueue();
    config.max_in_flight = 2;
    Fixture f(config);

    for (int i = 0; i < 5; ++i) {
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    }

    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
    CHECK_EQ_INT(static_cast<long long>(f.executor->InFlightCount()), 2);
    CHECK_EQ_INT(static_cast<long long>(f.executor->QueuedCount()), 3);

    // A slot freed in this Update is used in this same Update, which is what makes one
    // call drain a frame rather than one item.
    const auto order = f.transport->Order();
    f.transport->Complete(order[0], 200, "{}");
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);
}

void TestFullQueueRejectsAndReportsOnNextUpdate()
{
    QueueConfig config = DefaultQueue();
    config.max_queue_size = 1;
    config.deny_when_backed_up = true;
    Fixture f(config);

    const RequestId first =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    CHECK(first.IsValid());

    int rejected = 0;
    Completion rejection;
    const RequestId second = f.executor->Submit(
        Get("https://example.test/b"), [&](const Completion& c) {
            ++rejected;
            rejection = c;
        });

    // Rejected outright, and the id says so.
    CHECK(!second.IsValid());

    // The handler is not called synchronously: callbacks fire from Update() (D5).
    CHECK_EQ_INT(rejected, 0);

    CHECK(f.executor->Update());
    CHECK_EQ_INT(rejected, 1);
    CHECK(rejection.error.code == ErrorCode::QueueFull);
}

void TestBackedUpQueueWaitsWhenDenyIsOff()
{
    QueueConfig config = DefaultQueue();
    config.max_queue_size = 1;
    config.deny_when_backed_up = false;
    Fixture f(config);

    const RequestId first =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    const RequestId second =
        f.executor->Submit(Get("https://example.test/b"), [](const Completion&) {});

    // Both accepted: the adopter chose to stall rather than drop.
    CHECK(first.IsValid());
    CHECK(second.IsValid());
}

// --- The transport seam ----------------------------------------------------------------

void TestDeadTransportFailsInFlightRequests()
{
    Fixture f;

    int delivered = 0;
    Completion last;
    f.executor->Submit(Get("https://example.test/a"), [&](const Completion& c) {
        ++delivered;
        last = c;
    });

    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.executor->InFlightCount()), 1);

    // The adopter's transport dies while a request is in flight. This is the case the
    // `weak_ptr` seam exists to make detectable: the executor must notice and fail the
    // request rather than dereference freed memory.
    f.transport.reset();

    CHECK(!f.executor->Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.code == ErrorCode::Transport);

    // And it said so, rather than failing silently.
    bool logged = false;
    for (const std::string& message : f.logger->messages) {
        if (message.find("no transport is available") != std::string::npos) {
            logged = true;
        }
    }
    CHECK(logged);
}

void TestSetTransportReplacesTheSeam()
{
    Fixture f;

    auto replacement = std::make_shared<FakeHttpTransport>();
    CHECK(f.executor->SetTransport(replacement));

    f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    CHECK(f.executor->Update());

    // The new transport received it, and the old one did not.
    CHECK_EQ_INT(static_cast<long long>(replacement->StartCount()), 1);
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 0);
}

void TestSetTransportIsRefusedWhileInFlight()
{
    Fixture f;

    f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.executor->InFlightCount()), 1);

    // An in-flight transfer id means nothing to a different transport, so replacing it now
    // would strand the old transfer. Refused, and said so.
    auto replacement = std::make_shared<FakeHttpTransport>();
    CHECK(!f.executor->SetTransport(replacement));

    bool warned = false;
    for (const std::string& message : f.logger->messages) {
        if (message.find("SetTransport was refused") != std::string::npos) {
            warned = true;
        }
    }
    CHECK(warned);

    // Once drained, the replacement is accepted.
    f.transport->Complete(f.transport->Order().back(), 200, "{}");
    CHECK(!f.executor->Update());
    CHECK(f.executor->SetTransport(replacement));
}

// --- Logging ---------------------------------------------------------------------------

void TestEachAttemptIsLoggedAndObfuscated()
{
    Fixture f;

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 200;
    response.body = "{\"session_token\":\"abcdef1234567890\"}";
    response.headers.Set("LL-Trace-Id", "trace-42");
    f.transport->Complete(f.transport->Order().back(), response);

    CHECK(!f.executor->Update());

    CHECK_EQ_INT(static_cast<long long>(f.logger->entries.size()), 1);
    const log::HttpEntry& entry = f.logger->entries[0];
    CHECK_EQ(entry.method, std::string("GET"));
    CHECK_EQ(entry.url, std::string("https://example.test/a"));
    CHECK_EQ_INT(entry.status_code, 200);
    CHECK_EQ_INT(entry.attempt, 1);
    CHECK_EQ(entry.trace_id, std::string("trace-42"));

    // The token must not appear in the logged body. Asserting on the exact masked form
    // rather than on "the secret is absent", because an absent-secret assertion passes
    // when the body is empty for the wrong reason.
    CHECK(entry.response_body.find("abcdef1234567890") == std::string::npos);
    CHECK(entry.response_body.find("abc") != std::string::npos);
}

void TestRetryAfterZeroRetriesImmediately()
{
    Fixture f;

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 429;
    response.body = "{}";
    // A valid instruction to retry immediately. Treating it as "no header" would apply the
    // 50 ms exponential back-off instead, which is not what the server asked for.
    response.headers.Set("Retry-After", "0");
    f.transport->Complete(f.transport->Order().back(), response);

    // No clock advance at all, and the retry is already due.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

void TestHugeRetryAfterIsClampedNotOverflowed()
{
    Fixture f;

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 429;
    response.body = "{}";
    // Server-controlled input, so it is not trusted to be small. This overflows a signed
    // int on the multiply if the accumulator is not checked.
    response.headers.Set("Retry-After", "99999999999999999999");
    f.transport->Complete(f.transport->Order().back(), response);

    // Clamped to the largest representable delay rather than wrapping negative. A negative
    // delay would make the retry due immediately, which is the opposite of obeying.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    f.clock.Advance(1000);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);
}

void TestLargeBackoffFactorDoesNotOverflow()
{
    QueueConfig config = DefaultQueue();
    config.max_retries = 40;
    config.backoff_factor = 1000000;   // absurd, but a config field is adopter input
    config.initial_backoff_ms = 1000000;
    Fixture f(config);

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());
    f.transport->Complete(f.transport->Order().back(), 503, "unavailable");
    f.executor->Update();

    // The first delay is already 1,000,000 ms, so a second is not enough.
    f.clock.Advance(1000);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    // The point of the test: the delay is capped at a day and stays positive. Without the
    // pre-multiplication cap, the multiply overflows signed int64 and the delay goes
    // negative, which would make the retry due immediately - the opposite of a back-off.
    f.clock.Advance(86400000);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);

    // And it keeps working rather than wrapping: another failure, another capped delay.
    f.transport->Complete(f.transport->Order().back(), 503, "unavailable");
    f.executor->Update();
    f.clock.Advance(86400000);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);
}

void TestInvalidConfigRejectsThroughUpdate()
{
    // An inert client still has to deliver its handlers, or a caller waits for a callback
    // that never comes.
    Error config_error;
    config_error.code = ErrorCode::InvalidConfig;
    config_error.message = "domain_key is required";

    auto transport = std::make_shared<FakeHttpTransport>();
    auto logger = std::make_shared<RecordingLogger>();
    FakeClock clock;
    ExecutorSettings settings;
    settings.queue = DefaultQueue();
    settings.transport = transport;
    settings.clock = &clock;
    settings.logger = logger;
    settings.config_error = config_error;
    Executor executor(settings);

    int delivered = 0;
    Completion last;
    const RequestId id = executor.Submit(Get("https://example.test/a"),
                                             [&](const Completion& c) {
                                                 ++delivered;
                                                 last = c;
                                             });

    CHECK(!id.IsValid());
    CHECK_EQ_INT(delivered, 0);

    // Delivered on Update, like every other rejection.
    CHECK(!executor.Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.code == ErrorCode::InvalidConfig);

    // And nothing was ever sent.
    CHECK_EQ_INT(static_cast<long long>(transport->StartCount()), 0);
}

void TestRedirectIsAnErrorNotASuccess()
{
    Fixture f;

    int delivered = 0;
    Completion last;
    const RequestId id = f.executor->Submit(
        Get("https://example.test/a"), [&](const Completion& c) {
            ++delivered;
            last = c;
        });

    CHECK(f.executor->Update());

    // Redirects are disabled in the default transport (D3), so a 302 arrives as a plain
    // response. It must not be delivered as a success: the module layer would try to decode
    // the redirect body as an API response and report a confusing decode error instead of
    // "the server redirected us".
    IHttpTransport::Response response;
    response.status_code = 302;
    response.body = "<html>moved</html>";
    response.headers.Set("Location", "https://elsewhere.test/");
    f.transport->Complete(f.transport->Order().back(), response);

    CHECK(!f.executor->Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.code == ErrorCode::Http);
    CHECK_EQ_INT(last.status_code, 302);

    // Not retried: a 3xx is not in the retryable set, and retrying would just redirect
    // again.
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    // The body is still carried, so the adopter can see what the server actually sent (D7).
    CHECK_EQ(last.body, std::string("<html>moved</html>"));
}

void TestNoContentIsStillASuccess()
{
    Fixture f;

    int delivered = 0;
    Completion last;
    const RequestId id = f.executor->Submit(
        Get("https://example.test/a"), [&](const Completion& c) {
            ++delivered;
            last = c;
        });

    CHECK(f.executor->Update());

    // 204 is the one legitimate empty success (`EndSession`). Widening the error range to
    // "everything that is not 2xx" must not catch it.
    f.transport->Complete(f.transport->Order().back(), 204, "");

    CHECK(!f.executor->Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.Ok());
    CHECK_EQ_INT(last.status_code, 204);
}

// --- RequestId (T11d) ------------------------------------------------------------------

void TestRequestIdIsSentAsTheHeader()
{
    Fixture f;

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    CHECK(id.IsValid());

    CHECK(f.executor->Update());

    // The id the caller got is the id that went on the wire, so a support ticket can be
    // matched to a server log line (D25).
    const auto order = f.transport->Order();
    CHECK_EQ_INT(static_cast<long long>(order.size()), 1);
    const IHttpTransport::Request* sent = f.transport->FindRequest(order[0]);
    CHECK(sent != nullptr);
    if (sent != nullptr) {
        CHECK(sent->headers.Has("LL-Request-Id"));
    }
}

void TestRequestIdHeaderMatchesTheReturnedId()
{
    Fixture f;

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    CHECK(f.executor->Update());

    const auto order = f.transport->Order();
    const IHttpTransport::Request* sent = f.transport->FindRequest(order[0]);
    CHECK(sent != nullptr);
    if (sent != nullptr) {
        std::string header;
        CHECK(sent->headers.Get("LL-Request-Id", header));
        CHECK_EQ(header, id.ToString());
    }
}

void TestRetryKeepsTheSameRequestId()
{
    Fixture f;

    const RequestId id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    CHECK(f.executor->Update());
    f.transport->Complete(f.transport->Order().back(), 500, "boom");
    f.executor->Update();
    f.clock.Advance(50);
    CHECK(f.executor->Update());

    // Both attempts carry the same id: it is the same logical request, and a server log
    // should show one id across attempts rather than one per attempt.
    const auto order = f.transport->Order();
    CHECK_EQ_INT(static_cast<long long>(order.size()), 2);
    for (const auto transfer : order) {
        const IHttpTransport::Request* sent = f.transport->FindRequest(transfer);
        CHECK(sent != nullptr);
        if (sent != nullptr) {
            std::string header;
            CHECK(sent->headers.Get("LL-Request-Id", header));
            CHECK_EQ(header, id.ToString());
        }
    }
}

void TestIdenticalRequestsAreBothSent()
{
    Fixture f;

    int first_delivered = 0;
    int second_delivered = 0;

    // Two identical submissions are two requests, and both are sent. There is no
    // deduplication or grouping (owner, 2026-11-19): the API is not guaranteed to be
    // idempotent, so "add 5 gold" sent twice must yield 10, not 5. The SDK cannot tell that
    // case apart from a harmless repeated read, so it never merges.
    const RequestId first = f.executor->Submit(
        Get("https://example.test/broadcasts"), [&](const Completion&) { ++first_delivered; });
    const RequestId second = f.executor->Submit(
        Get("https://example.test/broadcasts"), [&](const Completion&) { ++second_delivered; });

    CHECK(first.IsValid());
    CHECK(second.IsValid());
    CHECK(first != second);

    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);

    const auto order = f.transport->Order();
    f.transport->Complete(order[0], 200, "{}");
    f.transport->Complete(order[1], 200, "{}");
    CHECK(!f.executor->Update());

    // Each caller was answered exactly once, by its own request.
    CHECK_EQ_INT(first_delivered, 1);
    CHECK_EQ_INT(second_delivered, 1);
}

void TestIdenticalWritesAreBothSent()
{
    Fixture f;

    // The case that made us drop deduplication entirely: two identical "add 5 gold" calls
    // must both reach the server, because merging them would grant 5 instead of 10.
    const RequestId first =
        f.executor->Submit(Post("https://example.test/grant"), [](const Completion&) {});
    const RequestId second =
        f.executor->Submit(Post("https://example.test/grant"), [](const Completion&) {});

    CHECK(first != second);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

void TestRequestIdToString()
{
    CHECK_EQ(RequestId().ToString(), std::string("0"));
    CHECK_EQ(RequestId(1).ToString(), std::string("1"));
    CHECK_EQ(RequestId(42).ToString(), std::string("42"));
    CHECK_EQ(RequestId(1000000).ToString(), std::string("1000000"));

    // The largest value, which is where a hand-rolled decimal conversion is most likely to
    // be wrong.
    CHECK_EQ(RequestId(18446744073709551615ULL).ToString(),
             std::string("18446744073709551615"));

    CHECK(!RequestId().IsValid());
    CHECK(RequestId(1).IsValid());
}

void TestFailureCarriesTheRequestId()
{
    // No retries, so the first failure is delivered rather than requeued.
    QueueConfig config = DefaultQueue();
    config.max_retries = 0;
    Fixture f(config);

    Completion last;
    const RequestId id = f.executor->Submit(
        Get("https://example.test/a"), [&](const Completion& c) { last = c; });

    CHECK(f.executor->Update());
    f.transport->Complete(f.transport->Order().back(), 500, "boom");
    CHECK(!f.executor->Update());

    // A support ticket is usually raised from a failure, not a success, so the id has to be
    // on the error as well as in the log. `Error::request_id` documents itself as the id
    // sent in `LL-Request-Id`.
    CHECK(!last.error.Ok());
    CHECK_EQ(last.error.request_id, id.ToString());
}

void TestTimeoutCarriesTheRequestId()
{
    QueueConfig config = DefaultQueue();
    config.max_retries = 0;
    Fixture f(config);

    Completion last;
    const RequestId id = f.executor->Submit(
        Get("https://example.test/a"), [&](const Completion& c) { last = c; });

    CHECK(f.executor->Update());

    // Let it time out. With no retries the timeout is delivered immediately.
    f.clock.Advance(300 * 1000);
    CHECK(!f.executor->Update());

    CHECK(!last.error.Ok());
    CHECK(last.error.code == ErrorCode::Transport);
    CHECK_EQ(last.error.request_id, id.ToString());
}

void TestRequestIdsAreUniqueAcrossClients()
{
    // Two clients in one process are legal (D20). A per-client counter would let both mint
    // the same id, so a support ticket quoting "request 5" would be ambiguous.
    Fixture a;
    Fixture b;
    const RequestId first =
        a.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    const RequestId second =
        b.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(first.IsValid());
    CHECK(second.IsValid());
    CHECK(first != second);
}

void TestRequestIdNextIsMonotonic()
{
    const RequestId first = RequestId::Next();
    const RequestId second = RequestId::Next();
    const RequestId third = RequestId::Next();

    CHECK(first.IsValid());
    CHECK(first < second);
    CHECK(second < third);

    // Never invalid, even at the wrap point: 0 is skipped.
    CHECK(RequestId::Next().IsValid());
}

void TestRequestIdNextIsUniqueUnderConcurrency()
{
    // The claim under test: every call gets a distinct value, including across threads. A
    // duplicate here would mean two requests sharing a correlation id, which is exactly what
    // the id exists to prevent.
    //
    // The counts are deliberately large and the threads start together. A smaller run does
    // not reliably expose a non-atomic counter - verified by sabotage, where a plain
    // read-then-write passed at 4x2000 and only failed once the contention was real.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20000;

    std::vector<std::vector<std::uint64_t>> minted(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    // A barrier, so the threads contend from the same instant rather than starting in
    // sequence and never overlapping.
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&minted, &ready, &go, t]() {
            // Pre-sized, and written by index rather than push_back: an allocating
            // push_back spends most of its time in the allocator, which shrinks the window
            // where two threads are inside the counter at once. Verified by sabotage - with
            // push_back a non-atomic counter passed, and only this version exposes it.
            minted[t].resize(kPerThread);
            ready.fetch_add(1);
            while (!go.load()) {
                // spin until every thread is in place
            }
            for (int i = 0; i < kPerThread; ++i) {
                minted[t][static_cast<std::size_t>(i)] = RequestId::Next().Value();
            }
        });
    }

    while (ready.load() < kThreads) {
        // wait for all threads to be ready
    }
    go.store(true);

    for (std::thread& thread : threads) {
        thread.join();
    }

    std::vector<std::uint64_t> all;
    all.reserve(static_cast<std::size_t>(kThreads) * kPerThread);
    for (const auto& per_thread : minted) {
        all.insert(all.end(), per_thread.begin(), per_thread.end());
    }

    CHECK_EQ_INT(static_cast<long long>(all.size()),
                 static_cast<long long>(kThreads) * kPerThread);

    std::sort(all.begin(), all.end());
    const auto duplicate = std::adjacent_find(all.begin(), all.end());
    CHECK(duplicate == all.end());

    // And none of them is the invalid id.
    CHECK(all.front() != 0);
}

// --- The rate limiter's integration with the queue (T11a) ------------------------------

// A fixture whose limiter is tight enough to trip in a few requests, so the integration is
// testable without 280 submissions.
struct LimitedFixture {
    LimitedFixture()
        : transport(std::make_shared<FakeHttpTransport>()),
          logger(std::make_shared<RecordingLogger>())
    {
        QueueConfig queue = DefaultQueue();
        queue.max_in_flight = 50;

        RateLimitConfig limit;
        limit.enabled = true;
        limit.tripwire_window_seconds = 10;
        limit.max_requests_per_tripwire = 3;
        limit.seconds_per_bucket = 10;
        limit.moving_average_fraction = 1.0;
        limit.moving_average_time_frames = 1;

        ExecutorSettings settings;
        settings.queue = queue;
        settings.rate_limit = limit;
        settings.transport = transport;
        settings.clock = &clock;
        settings.logger = logger;
        executor.reset(new Executor(settings));
    }

    std::shared_ptr<FakeHttpTransport> transport;
    std::shared_ptr<RecordingLogger>   logger;
    FakeClock                          clock;
    std::unique_ptr<Executor>          executor;
};

void TestTrippedRequestIsHeldThenDelivered()
{
    LimitedFixture f;

    int delivered = 0;
    for (int i = 0; i < 5; ++i) {
        f.executor->Submit(Get("https://example.test/a" + std::to_string(i)),
                           [&delivered](const Completion&) { ++delivered; });
    }

    // The limiter allows 3 in the window, so only 3 are sent. The other 2 are held, not
    // dropped and not failed.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);
    CHECK_EQ_INT(delivered, 0);

    // Complete the three that went out.
    for (const auto transfer : f.transport->Order()) {
        f.transport->Complete(transfer, 200, "{}");
    }
    CHECK(f.executor->Update());
    CHECK_EQ_INT(delivered, 3);

    // The two held requests are still queued, and `Update()` still reports work to do -
    // which is what stops a `while (Update())` loop from exiting early and losing them.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);

    // Once the window clears they are sent, without the caller doing anything.
    f.clock.AdvanceSeconds(11);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 5);

    for (const auto transfer : f.transport->Order()) {
        f.transport->Complete(transfer, 200, "{}");
    }
    CHECK(!f.executor->Update());
    CHECK_EQ_INT(delivered, 5);
}

void TestServerRateLimitIsHandledByRetryNotTheLimiter()
{
    // The limiter is disabled, so a 429 can only be handled by the retry policy. This is
    // the separation D25 insists on: the limiter is client-side self-restraint, and a
    // server 429 is a different thing entirely.
    QueueConfig queue = DefaultQueue();
    queue.max_retries = 2;

    RateLimitConfig limit;
    limit.enabled = false;

    auto transport = std::make_shared<FakeHttpTransport>();
    auto logger = std::make_shared<RecordingLogger>();
    FakeClock clock;
    ExecutorSettings settings;
    settings.queue = queue;
    settings.rate_limit = limit;
    settings.transport = transport;
    settings.clock = &clock;
    settings.logger = logger;
    Executor executor(settings);

    int delivered = 0;
    Completion last;
    executor.Submit(Get("https://example.test/a"), [&](const Completion& c) {
        ++delivered;
        last = c;
    });

    CHECK(executor.Update());
    transport->Complete(transport->Order().back(), 429, "{}");
    CHECK(executor.Update());

    // Retried by the retry policy, not held by the limiter.
    CHECK_EQ_INT(delivered, 0);
    clock.Advance(50);
    CHECK(executor.Update());
    CHECK_EQ_INT(static_cast<long long>(transport->StartCount()), 2);

    // Attempt 2 fails the same way, so it is requeued again with a doubled delay.
    transport->Complete(transport->Order().back(), 429, "{}");
    CHECK(executor.Update());
    CHECK_EQ_INT(delivered, 0);
    clock.Advance(100);
    CHECK(executor.Update());
    CHECK_EQ_INT(static_cast<long long>(transport->StartCount()), 3);

    // Attempt 3 exhausts the budget, so the failure is delivered. It is `RateLimited`
    // because the server said so - the limiter is disabled and had no part in it.
    transport->Complete(transport->Order().back(), 429, "{}");
    CHECK(!executor.Update());

    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.code == ErrorCode::RateLimited);
    CHECK_EQ_INT(last.attempts, 3);
}

// A transport that refuses to start anything, so the executor's start-failure path can be
// exercised.
class RefusingTransport : public IHttpTransport {
public:
    TransferId Start(const Request&) override { return 0; }
    bool       Poll(TransferId, Response&) override { return false; }
    void       Cancel(TransferId) override {}
};

void TestFailedStartDoesNotBurnQuota()
{
    // The limiter allows 3 in the window. Every start fails, so nothing is ever sent - and
    // nothing should be counted. Counting a failed start would burn quota for a request that
    // never left, and repeated failures could hold unrelated queued requests.
    QueueConfig queue = DefaultQueue();
    queue.max_retries = 0;

    RateLimitConfig limit;
    limit.enabled = true;
    limit.tripwire_window_seconds = 10;
    limit.max_requests_per_tripwire = 3;
    limit.seconds_per_bucket = 10;
    limit.moving_average_fraction = 1.0;
    limit.moving_average_time_frames = 1;

    auto transport = std::make_shared<RefusingTransport>();
    auto logger = std::make_shared<RecordingLogger>();
    FakeClock clock;
    ExecutorSettings settings;
    settings.queue = queue;
    settings.rate_limit = limit;
    settings.transport = transport;
    settings.clock = &clock;
    settings.logger = logger;
    Executor executor(settings);

    int delivered = 0;
    for (int i = 0; i < 10; ++i) {
        executor.Submit(Get("https://example.test/a" + std::to_string(i)),
                        [&delivered](const Completion&) { ++delivered; });
    }

    // Every one fails to start, so every one is delivered as a transport failure rather than
    // being held by a limiter that thinks it sent something.
    executor.Update();
    CHECK_EQ_INT(delivered, 10);
}
void TestAbandonCancelsInFlightWithoutCallingBack()
{
    Fixture f;

    int delivered = 0;
    f.executor->Submit(Get("https://example.test/a"),
                       [&](const Completion&) { ++delivered; });
    CHECK(f.executor->Update());

    const auto order = f.transport->Order();
    CHECK_EQ_INT(static_cast<long long>(order.size()), 1);

    f.executor->Abandon();

    // The transfer was cancelled, and the handler was not invoked: this runs from a
    // destructor, where calling back into a half-destroyed client is worse than silence.
    CHECK(f.transport->WasCancelled(order[0]));
    CHECK_EQ_INT(delivered, 0);
    CHECK_EQ_INT(static_cast<long long>(f.executor->InFlightCount()), 0);
}

} // namespace

int main()
{
    TestOneUpdateDrainsSeveralCompletions();
    TestUpdateReturnsFalseOnlyWhenEmpty();
    TestServerErrorRetriesWithExponentialBackoff();
    TestClientErrorDoesNotRetry();
    TestRetryAfterIsHonoured();
    TestRetriesStopAtTheBudget();
    TestTimedOutPostIsRetried();
    TestTimedOutGetIsRetried();
    TestServerErrorOnPostIsRetried();
    TestConcurrencyCapIsRespected();
    TestFullQueueRejectsAndReportsOnNextUpdate();
    TestBackedUpQueueWaitsWhenDenyIsOff();
    TestDeadTransportFailsInFlightRequests();
    TestSetTransportReplacesTheSeam();
    TestSetTransportIsRefusedWhileInFlight();
    TestEachAttemptIsLoggedAndObfuscated();
    TestRetryAfterZeroRetriesImmediately();
    TestHugeRetryAfterIsClampedNotOverflowed();
    TestLargeBackoffFactorDoesNotOverflow();
    TestInvalidConfigRejectsThroughUpdate();
    TestRedirectIsAnErrorNotASuccess();
    TestNoContentIsStillASuccess();
    TestRequestIdIsSentAsTheHeader();
    TestRequestIdHeaderMatchesTheReturnedId();
    TestRetryKeepsTheSameRequestId();
    TestIdenticalRequestsAreBothSent();
    TestIdenticalWritesAreBothSent();
    TestRequestIdToString();
    TestFailureCarriesTheRequestId();
    TestTimeoutCarriesTheRequestId();
    TestRequestIdsAreUniqueAcrossClients();
    TestRequestIdNextIsMonotonic();
    TestRequestIdNextIsUniqueUnderConcurrency();
    TestTrippedRequestIsHeldThenDelivered();
    TestServerRateLimitIsHandledByRetryNotTheLimiter();
    TestFailedStartDoesNotBurnQuota();
    TestAbandonCancelsInFlightWithoutCallingBack();

    return testing::Report();
}

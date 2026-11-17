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

#include <cstdint>
#include <memory>
#include <string>
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
    request.idempotent = false;   // a write: never retried when the response is lost
    return request;
}

// A harness that owns the pieces in the right order, so the executor's reference to the
// clock stays valid.
struct Fixture {
    explicit Fixture(QueueConfig config = DefaultQueue())
        : transport(std::make_shared<FakeHttpTransport>()), logger(std::make_shared<RecordingLogger>())
    {
        executor.reset(new Executor(config, transport, clock, logger));
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
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(f.executor->Submit(Get("https://example.test/a"),
                                         [&delivered](const Completion&) { ++delivered; }));
    }

    CHECK_EQ_INT(static_cast<long long>(ids.size()), 3);
    CHECK(ids[0] != 0 && ids[1] != 0 && ids[2] != 0);

    // One Update starts all three, because max_in_flight is 50.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 3);
    CHECK_EQ_INT(static_cast<long long>(f.executor->InFlightCount()), 3);

    // Complete all three, then a single Update must deliver all three. This is the
    // "frame's worth of work, not one item" contract (D5).
    for (const std::uint64_t id : ids) {
        f.transport->Complete(id, 200, "{}");
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
    const std::uint64_t id =
        f.executor->Submit(Get("https://example.test/a"),
                           [&delivered](const Completion&) { delivered = true; });
    CHECK(id != 0);

    // Queued, so there is work.
    CHECK(f.executor->Update());
    CHECK(!delivered);

    f.transport->Complete(id, 200, "{}");

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
    const std::uint64_t id = f.executor->Submit(
        Get("https://example.test/a"), [&](const Completion& c) {
            ++delivered;
            last = c;
        });

    CHECK(f.executor->Update());
    f.transport->Complete(id, 400, "{\"error\":\"bad\"}");

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

    const std::uint64_t id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 429;
    response.body = "{}";
    response.headers.Set("Retry-After", "7");
    f.transport->Complete(id, response);

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

// --- R25: never retry a non-idempotent request whose response was lost -----------------

void TestTimedOutPostIsNotRetried()
{
    Fixture f;

    int delivered = 0;
    Completion last;
    f.executor->Submit(Post("https://example.test/grant"), [&](const Completion& c) {
        ++delivered;
        last = c;
    });

    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    // The response never arrives. A POST that timed out may have succeeded, and retrying
    // it can double-grant, so it is delivered as a failure instead.
    f.clock.Advance(300 * 1000);
    CHECK(!f.executor->Update());

    CHECK_EQ_INT(delivered, 1);
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);
    CHECK(last.error.code == ErrorCode::Transport);
    CHECK_EQ_INT(last.status_code, 0);
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

    // A GET changed nothing, so a lost response is free to retry.
    f.clock.Advance(300 * 1000);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(delivered, 0);

    f.clock.Advance(50);
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

void TestServerErrorOnPostIsStillRetried()
{
    Fixture f;

    int delivered = 0;
    const std::uint64_t id = f.executor->Submit(Post("https://example.test/grant"),
                                                [&](const Completion&) { ++delivered; });

    CHECK(f.executor->Update());

    // A 500 is a *received* response: the server told us it failed, so the request did not
    // take effect and retrying is safe even for a POST. Only a lost response is ambiguous.
    f.transport->Complete(id, 500, "boom");
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

    const std::uint64_t first =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    CHECK(first != 0);

    int rejected = 0;
    Completion rejection;
    const std::uint64_t second = f.executor->Submit(
        Get("https://example.test/b"), [&](const Completion& c) {
            ++rejected;
            rejection = c;
        });

    // Rejected outright, and the id says so.
    CHECK_EQ_INT(static_cast<long long>(second), 0);

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

    const std::uint64_t first =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});
    const std::uint64_t second =
        f.executor->Submit(Get("https://example.test/b"), [](const Completion&) {});

    // Both accepted: the adopter chose to stall rather than drop.
    CHECK(first != 0);
    CHECK(second != 0);
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

    const std::uint64_t id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 200;
    response.body = "{\"session_token\":\"abcdef1234567890\"}";
    response.headers.Set("LL-Trace-Id", "trace-42");
    f.transport->Complete(id, response);

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

    const std::uint64_t id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 429;
    response.body = "{}";
    // A valid instruction to retry immediately. Treating it as "no header" would apply the
    // 50 ms exponential back-off instead, which is not what the server asked for.
    response.headers.Set("Retry-After", "0");
    f.transport->Complete(id, response);

    // No clock advance at all, and the retry is already due.
    CHECK(f.executor->Update());
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 2);
}

void TestHugeRetryAfterIsClampedNotOverflowed()
{
    Fixture f;

    const std::uint64_t id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());

    IHttpTransport::Response response;
    response.status_code = 429;
    response.body = "{}";
    // Server-controlled input, so it is not trusted to be small. This overflows a signed
    // int on the multiply if the accumulator is not checked.
    response.headers.Set("Retry-After", "99999999999999999999");
    f.transport->Complete(id, response);

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

    const std::uint64_t id =
        f.executor->Submit(Get("https://example.test/a"), [](const Completion&) {});

    CHECK(f.executor->Update());
    f.transport->Complete(id, 503, "unavailable");
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
    Executor executor(DefaultQueue(), transport, clock, logger, config_error);

    int delivered = 0;
    Completion last;
    const std::uint64_t id = executor.Submit(Get("https://example.test/a"),
                                             [&](const Completion& c) {
                                                 ++delivered;
                                                 last = c;
                                             });

    CHECK_EQ_INT(static_cast<long long>(id), 0);
    CHECK_EQ_INT(delivered, 0);

    // Delivered on Update, like every other rejection.
    CHECK(!executor.Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.code == ErrorCode::InvalidConfig);

    // And nothing was ever sent.
    CHECK_EQ_INT(static_cast<long long>(transport->StartCount()), 0);
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
    TestTimedOutPostIsNotRetried();
    TestTimedOutGetIsRetried();
    TestServerErrorOnPostIsStillRetried();
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
    TestAbandonCancelsInFlightWithoutCallingBack();

    return testing::Report();
}

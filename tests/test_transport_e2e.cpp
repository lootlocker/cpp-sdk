// The end-to-end transport test (T11, owner 2026-11-13).
//
// T10 shipped a transport that compiled but had never sent a request. This file is what
// closes that: a real `CurlHttpTransport` talking to a real socket, with the server
// asserting on what actually arrived on the wire and the SDK asserting on what it decoded.
//
// Two things are proven here that nothing else proves:
//   1. The curl adapter works, rather than merely compiles.
//   2. `Config::transport` is actually consulted, rather than the default being hard-wired.
//
// The second test does not need curl, so it runs even in a build without it. The first is
// skipped with a printed reason when curl was not built in, because a test that silently
// passes when the thing under test is absent is worse than no test.

#include <lootlocker/client.h>
#include <lootlocker/config.h>
#include <lootlocker/curl_http_transport.h>
#include <lootlocker/fake_http_transport.h>
#include <lootlocker/http_transport.h>

#include "executor.h"
#include "loopback_server.h"
#include "test_harness.h"

#include <memory>
#include <string>
#include <vector>

using namespace lootlocker;

namespace {

Config MakeConfig(const std::string& base_url)
{
    Config config;
    config.domain_key = "1g9glch3";
    config.game_key = "prod_1c52468fc6e8420c955e3b6c303ea8cc";
    config.game_version = "1.2.1";
    config.base_url = base_url;
    config.log_level = LogLevel::NoLogging;
    return config;
}

// --- 1. A real round trip through the real transport -----------------------------------

#if defined(LOOTLOCKER_HAVE_CURL)

void TestRealRoundTripThroughCurl()
{
    testing::LoopbackServer server;
    if (!server.Start()) {
        // A failure to bind is a real failure, not a skip: the test cannot prove anything
        // without a socket, and silently passing would hide that.
        CHECK(false);
        return;
    }

    testing::ScriptedResponse response;
    response.status_code = 200;
    response.body = "{\"broadcasts\":[{\"id\":\"b1\",\"message\":\"hello\"}]}";
    server.Script({response});

    auto transport = std::make_shared<CurlHttpTransport>();

    IHttpTransport::Request request;
    request.method = "POST";
    request.url = server.BaseUrl() + "/game/broadcasts/v1?limit=10";
    request.headers.Set("Content-Type", "application/json");
    request.headers.Set("x-session-token", "session-abc");
    request.headers.Set("LL-Request-Id", "42");
    request.body = "{\"limit\":10}";
    request.timeout_seconds = 10;

    const IHttpTransport::TransferId id = transport->Start(request);
    CHECK(id != 0);

    // Poll until it finishes. The transport is pollable, so this is the real contract: no
    // blocking call, and the loop is bounded so a hang fails the test instead of hanging
    // CI.
    IHttpTransport::Response received;
    bool done = false;
    for (int i = 0; i < 2000 && !done; ++i) {
        done = transport->Poll(id, received);
        if (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(done);
    transport->Cancel(id);

    // --- What the server actually received --------------------------------------------

    const std::vector<testing::ReceivedRequest> requests = server.Received();
    CHECK_EQ_INT(static_cast<long long>(requests.size()), 1);
    if (requests.empty()) {
        server.Stop();
        return;
    }
    const testing::ReceivedRequest& wire = requests[0];

    // The request line: method, path and query exactly as the SDK built them.
    CHECK_EQ(wire.method, std::string("POST"));
    CHECK_EQ(wire.target, std::string("/game/broadcasts/v1?limit=10"));

    // The headers the SDK set, as they arrived. This is the assertion that would have
    // caught a header being dropped or renamed by the adapter.
    CHECK_EQ(wire.Header("Content-Type"), std::string("application/json"));
    CHECK_EQ(wire.Header("x-session-token"), std::string("session-abc"));
    CHECK_EQ(wire.Header("LL-Request-Id"), std::string("42"));

    // The body, byte for byte.
    CHECK_EQ(wire.body, std::string("{\"limit\":10}"));

    // --- What the SDK received --------------------------------------------------------

    CHECK_EQ_INT(received.status_code, 200);
    CHECK_EQ(received.body, std::string("{\"broadcasts\":[{\"id\":\"b1\",\"message\":\"hello\"}]}"));

    // Response headers are parsed into the map, which is what the executor reads
    // `Retry-After` and `LL-Trace-Id` from.
    std::string content_type;
    CHECK(received.headers.Get("Content-Type", content_type));
    CHECK_EQ(content_type, std::string("application/json"));

    server.Stop();
}

void TestRealTransportReportsATransportFailure()
{
    // Nothing is listening on this port, so the connection is refused. The transport must
    // report that as "no response" (status 0) rather than inventing a status, because the
    // executor's retry policy branches on exactly that (D23).
    auto transport = std::make_shared<CurlHttpTransport>();

    IHttpTransport::Request request;
    request.method = "GET";
    request.url = "http://127.0.0.1:1/nothing-here";
    request.timeout_seconds = 5;

    const IHttpTransport::TransferId id = transport->Start(request);
    CHECK(id != 0);

    IHttpTransport::Response received;
    bool done = false;
    for (int i = 0; i < 2000 && !done; ++i) {
        done = transport->Poll(id, received);
        if (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(done);
    transport->Cancel(id);

    CHECK_EQ_INT(received.status_code, 0);
}

void TestRealTransportDoesNotFollowRedirects()
{
    // A redirect must not be followed, because curl replays the caller's headers to the
    // target and the SDK sends `x-session-token` (D3). The transport reports the 302
    // itself, and the caller decides.
    testing::LoopbackServer server;
    if (!server.Start()) {
        CHECK(false);
        return;
    }

    testing::ScriptedResponse redirect;
    redirect.status_code = 302;
    redirect.reason = "Found";
    redirect.content_type.clear();
    redirect.headers.emplace_back("Location", "http://127.0.0.1:1/elsewhere");
    server.Script({redirect});

    auto transport = std::make_shared<CurlHttpTransport>();

    IHttpTransport::Request request;
    request.method = "GET";
    request.url = server.BaseUrl() + "/game/broadcasts/v1";
    request.headers.Set("x-session-token", "session-abc");
    request.timeout_seconds = 5;

    const IHttpTransport::TransferId id = transport->Start(request);
    IHttpTransport::Response received;
    bool done = false;
    for (int i = 0; i < 2000 && !done; ++i) {
        done = transport->Poll(id, received);
        if (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(done);
    transport->Cancel(id);

    // The 302 is reported as-is, and only one request reached the server: the redirect was
    // not followed, so the token was never replayed anywhere.
    CHECK_EQ_INT(received.status_code, 302);
    CHECK_EQ_INT(static_cast<long long>(server.ReceivedCount()), 1);

    server.Stop();
}

#endif // LOOTLOCKER_HAVE_CURL

// --- 2. The transport seam is actually consulted ---------------------------------------

void TestInjectedTransportIsUsedByTheClient()
{
    // The point of this test: `Config::transport` was a declared field that nothing read.
    // If the client hard-wired the default, this fake would never see the request and the
    // test would fail.
    auto fake = std::make_shared<FakeHttpTransport>();

    Config config = MakeConfig("http://127.0.0.1:1");
    config.transport = fake;

    Client client(config);
    CHECK(client.ConfigError().Ok());

    // The client reports the injected transport, not a default.
    CHECK(!client.GetTransport().expired());
    CHECK(client.GetTransport().lock() == fake);
    int delivered = 0;
    core::Completion last;
    core::PendingRequest request;
    request.method = "GET";
    request.url = "http://127.0.0.1:1/game/broadcasts/v1";

    core::GetExecutor(client).Submit(request, [&](const core::Completion& completion) {
        ++delivered;
        last = completion;
    });

    CHECK(client.Update());

    // The fake received it, which is the whole assertion.
    CHECK_EQ_INT(static_cast<long long>(fake->StartCount()), 1);
    const auto order = fake->Order();
    CHECK_EQ_INT(static_cast<long long>(order.size()), 1);
    if (!order.empty()) {
        const IHttpTransport::Request* sent = fake->FindRequest(order[0]);
        CHECK(sent != nullptr);
        if (sent != nullptr) {
            CHECK_EQ(sent->method, std::string("GET"));
            CHECK_EQ(sent->url, std::string("http://127.0.0.1:1/game/broadcasts/v1"));
        }
    }

    // Complete it through the fake and confirm the result reaches the caller, so the whole
    // path is exercised rather than just the start.
    fake->Complete(order[0], 200, "{\"broadcasts\":[]}");
    CHECK(!client.Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.Ok());
    CHECK_EQ(last.body, std::string("{\"broadcasts\":[]}"));
}

void TestSetTransportReplacesTheInjectedOne()
{
    auto first = std::make_shared<FakeHttpTransport>();
    auto second = std::make_shared<FakeHttpTransport>();

    Config config = MakeConfig("http://127.0.0.1:1");
    config.transport = first;

    Client client(config);
    CHECK(client.SetTransport(second));

    CHECK(client.GetTransport().lock() == second);

    core::PendingRequest request;
    request.method = "GET";
    request.url = "http://127.0.0.1:1/x";
    core::GetExecutor(client).Submit(request, [](const core::Completion&) {});
    CHECK(client.Update());

    CHECK_EQ_INT(static_cast<long long>(second->StartCount()), 1);
    CHECK_EQ_INT(static_cast<long long>(first->StartCount()), 0);
}

void TestInvalidConfigMakesTheClientInert()
{
    Config config;
    // No domain_key, no game key, no version.
    auto fake = std::make_shared<FakeHttpTransport>();
    config.transport = fake;

    Client client(config);

    CHECK(!client.ConfigError().Ok());
    CHECK(client.ConfigError().code == ErrorCode::InvalidConfig);

    // Inert: nothing is ever sent.
    CHECK(!client.Update());
    CHECK_EQ_INT(static_cast<long long>(fake->StartCount()), 0);

    // But a submission is still answered rather than silently dropped. A caller waiting for
    // a callback that never comes is worse than a clear error.
    int delivered = 0;
    core::Completion last;
    core::PendingRequest request;
    request.method = "GET";
    request.url = "http://127.0.0.1:1/x";
    core::GetExecutor(client).Submit(request, [&](const core::Completion& completion) {
        ++delivered;
        last = completion;
    });

    CHECK(!client.Update());
    CHECK_EQ_INT(delivered, 1);
    CHECK(last.error.code == ErrorCode::InvalidConfig);
    CHECK_EQ_INT(static_cast<long long>(fake->StartCount()), 0);
}

} // namespace

int main()
{
    TestInjectedTransportIsUsedByTheClient();
    TestSetTransportReplacesTheInjectedOne();
    TestInvalidConfigMakesTheClientInert();

#if defined(LOOTLOCKER_HAVE_CURL)
    TestRealRoundTripThroughCurl();
    TestRealTransportReportsATransportFailure();
    TestRealTransportDoesNotFollowRedirects();
#else
    std::printf("skipping the curl round-trip tests: libcurl was not built in\n");
#endif

    return testing::Report();
}

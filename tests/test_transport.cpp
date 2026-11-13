// The transport seam (T10).
//
// The pollable contract is the thing under test: `Start` must not block, `Poll` must not
// block, and `Cancel` must release. The fake transport makes all three assertable without
// sleeping or touching the network.

#include <lootlocker/fake_http_transport.h>
#include <lootlocker/header_map.h>
#include <lootlocker/http_transport.h>

#include "test_harness.h"

#include <string>

namespace {

void TestHeaderMapIsCaseInsensitive()
{
    lootlocker::HeaderMap headers;
    headers.Set("Content-Type", "application/json");

    std::string value;
    CHECK(headers.Get("Content-Type", value));
    CHECK_EQ(value, "application/json");

    // Lookup folds case; the stored name keeps its original casing.
    CHECK(headers.Get("content-type", value));
    CHECK_EQ(value, "application/json");
    CHECK(headers.Get("CONTENT-TYPE", value));
    CHECK(headers.Has("Content-Type"));
    CHECK(headers.Has("content-TYPE"));
    CHECK(!headers.Has("Accept"));

    CHECK_EQ(headers.Entries()[0].name, "Content-Type");
}

void TestHeaderMapPreservesOrder()
{
    lootlocker::HeaderMap headers;
    headers.Set("Z-First", "1");
    headers.Set("A-Second", "2");
    headers.Set("M-Third", "3");

    // Insertion order, not alphabetical: some servers and proxies care.
    CHECK_EQ(headers.Entries()[0].name, "Z-First");
    CHECK_EQ(headers.Entries()[1].name, "A-Second");
    CHECK_EQ(headers.Entries()[2].name, "M-Third");
}

void TestHeaderMapSetReplacesInPlace()
{
    lootlocker::HeaderMap headers;
    headers.Set("A", "1");
    headers.Set("B", "2");
    headers.Set("C", "3");

    // Overwriting must not move the header to the end.
    headers.Set("b", "changed");
    CHECK_EQ_INT(static_cast<long long>(headers.Size()), 3);
    CHECK_EQ(headers.Entries()[1].name, "B");
    CHECK_EQ(headers.Entries()[1].value, "changed");
}

void TestHeaderMapErase()
{
    lootlocker::HeaderMap headers;
    headers.Set("A", "1");
    headers.Set("B", "2");

    headers.Erase("a");
    CHECK_EQ_INT(static_cast<long long>(headers.Size()), 1);
    CHECK(!headers.Has("A"));
    CHECK(headers.Has("B"));

    // Erasing something absent is a no-op, not an error.
    headers.Erase("nope");
    CHECK_EQ_INT(static_cast<long long>(headers.Size()), 1);
}

void TestStartDoesNotBlock()
{
    lootlocker::FakeHttpTransport transport;

    lootlocker::IHttpTransport::Request request;
    request.method = "GET";
    request.url = "https://example.test/x";

    const auto id = transport.Start(request);

    // Start returned immediately with a handle, and nothing has completed.
    CHECK(id != 0);
    CHECK_EQ_INT(static_cast<long long>(transport.StartCount()), 1);

    lootlocker::IHttpTransport::Response response;
    CHECK(!transport.Poll(id, response));
}

void TestPollReportsCompletion()
{
    lootlocker::FakeHttpTransport transport;

    lootlocker::IHttpTransport::Request request;
    request.method = "POST";
    request.url = "https://example.test/session";
    request.body = "{\"a\":1}";
    request.headers.Set("Content-Type", "application/json");

    const auto id = transport.Start(request);

    lootlocker::IHttpTransport::Response response;
    CHECK(!transport.Poll(id, response));

    transport.Complete(id, 200, "{\"ok\":true}");

    CHECK(transport.Poll(id, response));
    CHECK_EQ_INT(response.status_code, 200);
    CHECK_EQ(response.body, "{\"ok\":true}");

    // The request the transport saw is the one that was sent.
    const lootlocker::IHttpTransport::Request* sent = transport.FindRequest(id);
    CHECK(sent != nullptr);
    if (sent != nullptr) {
        CHECK_EQ(sent->method, "POST");
        CHECK_EQ(sent->url, "https://example.test/session");
        CHECK_EQ(sent->body, "{\"a\":1}");
        std::string content_type;
        CHECK(sent->headers.Get("content-type", content_type));
        CHECK_EQ(content_type, "application/json");
    }
}

void TestTransportFailureIsNoResponse()
{
    lootlocker::FakeHttpTransport transport;

    lootlocker::IHttpTransport::Request request;
    request.method = "GET";
    request.url = "https://example.test/x";
    const auto id = transport.Start(request);

    transport.Fail(id);

    lootlocker::IHttpTransport::Response response;
    CHECK(transport.Poll(id, response));
    // Status 0 is how a transport failure is distinguished from a 4xx.
    CHECK_EQ_INT(response.status_code, 0);
    CHECK(response.body.empty());
}

void TestCancelReleases()
{
    lootlocker::FakeHttpTransport transport;

    lootlocker::IHttpTransport::Request request;
    request.method = "GET";
    request.url = "https://example.test/x";
    const auto id = transport.Start(request);

    transport.Cancel(id);

    CHECK(transport.WasCancelled(id));

    // A cancelled transfer never reports completion, even if it later would have.
    transport.Complete(id, 200, "late");
    lootlocker::IHttpTransport::Response response;
    CHECK(!transport.Poll(id, response));
}

void TestCancelIsSafeForUnknownId()
{
    lootlocker::FakeHttpTransport transport;
    // The executor cannot always know whether a transfer finished before it cancels, so
    // Cancel must tolerate an id it has never seen.
    transport.Cancel(9999);
    CHECK_EQ_INT(static_cast<long long>(transport.StartCount()), 0);
}

void TestPollIsSafeForUnknownId()
{
    lootlocker::FakeHttpTransport transport;
    lootlocker::IHttpTransport::Response response;
    CHECK(!transport.Poll(9999, response));
}

void TestSeveralTransfersAreIndependent()
{
    lootlocker::FakeHttpTransport transport;

    lootlocker::IHttpTransport::Request request;
    request.method = "GET";
    request.url = "https://example.test/a";
    const auto first = transport.Start(request);
    request.url = "https://example.test/b";
    const auto second = transport.Start(request);

    CHECK(first != second);

    transport.Complete(second, 201, "second");
    lootlocker::IHttpTransport::Response response;

    // Completing the second must not complete the first.
    CHECK(!transport.Poll(first, response));
    CHECK(transport.Poll(second, response));
    CHECK_EQ_INT(response.status_code, 201);
    CHECK_EQ(response.body, "second");

    transport.Complete(first, 200, "first");
    CHECK(transport.Poll(first, response));
    CHECK_EQ(response.body, "first");
}

void TestResponseBodyIsBytesNotText()
{
    lootlocker::FakeHttpTransport transport;

    lootlocker::IHttpTransport::Request request;
    request.method = "GET";
    request.url = "https://example.test/x";
    const auto id = transport.Start(request);

    // A body that is not valid UTF-8 must survive intact (D23): the caller decides what to
    // do with it, and mangling it here would destroy the evidence.
    std::string binary;
    binary.push_back(static_cast<char>(0xFF));
    binary.push_back(static_cast<char>(0x00));
    binary.push_back(static_cast<char>(0x80));

    transport.Complete(id, 200, binary);

    lootlocker::IHttpTransport::Response response;
    CHECK(transport.Poll(id, response));
    CHECK_EQ(response.body, binary);
    CHECK_EQ_INT(static_cast<long long>(response.body.size()), 3);
}

} // namespace

int main()
{
    TestHeaderMapIsCaseInsensitive();
    TestHeaderMapPreservesOrder();
    TestHeaderMapSetReplacesInPlace();
    TestHeaderMapErase();
    TestStartDoesNotBlock();
    TestPollReportsCompletion();
    TestTransportFailureIsNoResponse();
    TestCancelReleases();
    TestCancelIsSafeForUnknownId();
    TestPollIsSafeForUnknownId();
    TestSeveralTransfersAreIndependent();
    TestResponseBodyIsBytesNotText();

    return testing::Report();
}

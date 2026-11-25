// The endpoint definition layer (T13).
//
// The worked `ListBroadcastsDef` from the api-sketch is reproduced here, because the task's
// verification is that it compiles and that its typed wrapper returns the same `RequestId`
// the executor assigned. Everything else in this file exists to prove the generic executor
// actually consumes the definition rather than merely accepting it.
//
// The types live in their real namespaces rather than an anonymous one, because that is
// what a module target does: `from_json` is found by ADL, so it has to sit next to its type.

#include <lootlocker/client.h>
#include <lootlocker/config.h>
#include <lootlocker/fake_http_transport.h>
#include <lootlocker/json_builder.h>
#include <lootlocker/json_value.h>
#include <lootlocker/request_id.h>
#include <lootlocker/result.h>

#include "definition.h"
#include "query_parameters.h"

#include "test_harness.h"

#include <clocale>
#include <memory>
#include <string>
#include <vector>

namespace lootlocker {

// --- the worked definition's types, from api-sketch.md ---------------------

struct Broadcast {
    std::string id;
    std::string message;
};

struct ListBroadcastsRequest {
    int         limit = 0;
    std::string accept_language;
};

struct ListBroadcastsResponse {
    std::vector<Broadcast> broadcasts;
};

// The ADL decoder for the response type (D27). Returns a `Result` rather than `void` so a
// missing field is an error rather than a silent default (D23).
Result<Empty> from_json(const JsonValue& j, ListBroadcastsResponse& out)
{
    if (j.GetKind() != JsonValue::Kind::Object) {
        Error e;
        e.code    = ErrorCode::MalformedResponse;
        e.message = "broadcasts response is not an object";
        return Result<Empty>::Fail(e);
    }

    const JsonValue list = j["broadcasts"];
    if (list.GetKind() != JsonValue::Kind::Array) {
        Error e;
        e.code    = ErrorCode::MalformedResponse;
        e.message = "broadcasts is missing or not an array";
        return Result<Empty>::Fail(e);
    }

    out.broadcasts.clear();
    for (std::size_t i = 0; i < list.Size(); ++i) {
        const JsonValue item = list.At(i);

        // Each field is validated, not just the array. `AsString()` on a missing or
        // non-string value returns an empty string, so assigning it unchecked would turn
        // "the server did not send this" into "the server sent an empty string" - the
        // silent default D23 exists to prevent. A decoder that skipped this would make the
        // generic layer look correct while proving nothing.
        if (item["id"].GetKind() != JsonValue::Kind::String) {
            Error e;
            e.code    = ErrorCode::MalformedResponse;
            e.message = "a broadcast is missing a string id";
            return Result<Empty>::Fail(e);
        }
        if (item["message"].GetKind() != JsonValue::Kind::String) {
            Error e;
            e.code    = ErrorCode::MalformedResponse;
            e.message = "a broadcast is missing a string message";
            return Result<Empty>::Fail(e);
        }

        Broadcast b;
        b.id      = item["id"].AsString();
        b.message = item["message"].AsString();
        out.broadcasts.push_back(b);
    }
    return Result<Empty>::Ok(Empty());
}

namespace core {

struct ListBroadcastsDef {
    static constexpr const char* kMethod = "GET";
    static constexpr const char* kPath   = "/game/broadcasts/v1";

    using Request  = ListBroadcastsRequest;
    using Response = ListBroadcastsResponse;

    static void ConstructQueryParameters(const Request& r, QueryParameters& out)
    {
        out.Add("limit", r.limit);
    }

    static void ConstructHeaders(const Request& r, HeaderMap& out)
    {
        if (!r.accept_language.empty()) {
            out.Set("Accept-Language", r.accept_language);
        }
    }

    // Deliberately omitted: this definition has no endpoint-specific convenience, which is
    // what proves `NormalizeResponse` is optional rather than required.
};

} // namespace core
} // namespace lootlocker

namespace {

using lootlocker::Broadcast;
using lootlocker::Client;
using lootlocker::Config;
using lootlocker::Empty;
using lootlocker::ErrorCode;
using lootlocker::FakeHttpTransport;
using lootlocker::IHttpTransport;
using lootlocker::JsonValue;
using lootlocker::ListBroadcastsRequest;
using lootlocker::ListBroadcastsResponse;
using lootlocker::RequestId;
using lootlocker::Result;
using lootlocker::core::ListBroadcastsDef;

// --- helpers ---------------------------------------------------------------

Config MakeConfig()
{
    Config config;
    config.domain_key   = "testdomain";
    config.game_key     = "testgamekey";
    config.game_version = "1.0.0";
    return config;
}

// A client wired to a fake transport the test keeps alive.
struct Fixture {
    std::shared_ptr<FakeHttpTransport> transport;
    std::unique_ptr<Client>            client;

    Fixture()
    {
        transport        = std::make_shared<FakeHttpTransport>();
        Config config    = MakeConfig();
        config.transport = transport;
        client.reset(new Client(config));
    }

    // The request the transport was first asked to start.
    const IHttpTransport::Request* Sent() const
    {
        if (transport->Order().empty()) {
            return nullptr;
        }
        return transport->FindRequest(transport->Order().front());
    }
};

// --- tests -----------------------------------------------------------------

// The task's stated verification: the typed wrapper returns the id the executor assigned.
void TestTypedWrapperReturnsExecutorId()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 10;

    const RequestId returned = lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [](Result<ListBroadcastsResponse>) {});

    CHECK(returned.IsValid());

    // The executor assigned this id, and the request is queued under it rather than sent:
    // nothing moves until `Update()` (D5).
    CHECK_EQ_INT(static_cast<long long>(lootlocker::core::GetExecutor(*f.client).QueuedCount()),
                 1);
    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 0);
}

// The definition's method, path and query parameters reach the wire.
void TestDefinitionBuildsTheRequest()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit           = 25;
    request.accept_language = "sv-SE";

    lootlocker::core::Send<ListBroadcastsDef>(*f.client, request,
                                              [](Result<ListBroadcastsResponse>) {});
    f.client->Update();

    CHECK_EQ_INT(static_cast<long long>(f.transport->StartCount()), 1);

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK_EQ(sent->method, "GET");
    CHECK_EQ(sent->url, "https://testdomain.api.lootlocker.com/game/broadcasts/v1?limit=25");

    std::string value;
    CHECK(sent->headers.Get("Accept-Language", value));
    CHECK_EQ(value, "sv-SE");
}

// The default headers are present on every request, and the definition's own headers do not
// displace them.
void TestDefaultHeaders()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 1;

    lootlocker::core::Send<ListBroadcastsDef>(*f.client, request,
                                              [](Result<ListBroadcastsResponse>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    std::string value;
    CHECK(sent->headers.Get("LL-Version", value));
    CHECK_EQ(value, "2021-03-01");

    CHECK(sent->headers.Get("Accept", value));
    CHECK_EQ(value, "application/json");

    CHECK(sent->headers.Get("User-Agent", value));
    CHECK(value.find("LootLocker-Cpp-SDK/") == 0);

    // Not development, so the header is absent rather than "false".
    CHECK(!sent->headers.Has("is-development"));

    // No session yet (T14), so no token is sent.
    CHECK(!sent->headers.Has("x-session-token"));
}

// A successful response is decoded through the type's ADL `from_json` and delivered with the
// raw body intact (D7).
void TestSuccessfulDecode()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 5;

    std::vector<Broadcast> received;
    bool                   called = false;
    std::string            raw;

    lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [&](Result<ListBroadcastsResponse> result) {
            called = true;
            CHECK(result.Ok());
            if (result.Ok()) {
                received = result.Value().broadcasts;
            }
            raw = result.RawBody();
        });

    f.client->Update();
    const std::string body =
        R"({"broadcasts":[{"id":"a","message":"hello"},{"id":"b","message":"world"}]})";
    f.transport->Complete(f.transport->Order().front(), 200, body);
    f.client->Update();

    CHECK(called);
    CHECK_EQ_INT(static_cast<long long>(received.size()), 2);
    if (received.size() == 2) {
        CHECK_EQ(received[0].id, "a");
        CHECK_EQ(received[1].message, "world");
    }
    CHECK_EQ(raw, body);
}

// A body that is not JSON is a `MalformedResponse` carrying the original bytes, not a crash
// and not an empty value (D23).
void TestMalformedBody()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 5;

    bool        called = false;
    ErrorCode   code   = ErrorCode::None;
    std::string raw;

    lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [&](Result<ListBroadcastsResponse> result) {
            called = true;
            if (!result.Ok()) {
                code = result.Error().code;
            }
            raw = result.RawBody();
        });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200, "not json at all");
    f.client->Update();

    CHECK(called);
    CHECK(code == ErrorCode::MalformedResponse);
    CHECK_EQ(raw, "not json at all");
}

// A well-formed JSON body of the wrong shape is also a decode error, not a default. This is
// the case D23 exists for: `{"broadcasts": 5}` must not silently yield zero broadcasts.
void TestWrongShapeIsAnError()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 5;

    bool      called = false;
    ErrorCode code   = ErrorCode::None;

    lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [&](Result<ListBroadcastsResponse> result) {
            called = true;
            if (!result.Ok()) {
                code = result.Error().code;
            }
        });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200, R"({"broadcasts":5})");
    f.client->Update();

    CHECK(called);
    CHECK(code == ErrorCode::MalformedResponse);
}

// An HTTP failure is delivered as a failure with the status and the body, and is not
// decoded.
void TestHttpFailure()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 5;

    bool      called = false;
    ErrorCode code   = ErrorCode::None;
    int       status = 0;

    lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [&](Result<ListBroadcastsResponse> result) {
            called = true;
            if (!result.Ok()) {
                code   = result.Error().code;
                status = result.Error().status_code;
            }
        });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 404, R"({"error":"nope"})");
    f.client->Update();

    CHECK(called);
    CHECK(code == ErrorCode::Http);
    CHECK_EQ_INT(status, 404);
}

// `NormalizeResponse` is optional: a definition without one compiles and runs. The trait is
// what makes that true, so it is asserted directly rather than inferred from the tests
// above passing.
void TestNormalizeIsOptional()
{
    CHECK(!lootlocker::core::HasNormalizeResponse<ListBroadcastsDef>::value);
}

// --- query parameters ------------------------------------------------------

void TestQueryParameterEncoding()
{
    lootlocker::core::QueryParameters query;
    query.Add("limit", 25);
    query.Add("name", "a b&c=d");
    query.Add("flag", true);
    query.Add("ratio", 0.5);
    query.Add("big", static_cast<std::int64_t>(9007199254740993LL));

    CHECK_EQ(query.ToQueryString(),
             "limit=25&name=a%20b%26c%3Dd&flag=true&ratio=0.5&big=9007199254740993");
}

void TestQueryParameterEmpty()
{
    lootlocker::core::QueryParameters query;
    CHECK(query.Empty());
    CHECK_EQ(query.ToQueryString(), "");
}

// A double must not pick up a locale's decimal comma, and must not print six trailing
// zeros. Both are real failure modes of the obvious implementations.
void TestQueryParameterDoubleFormatting()
{
    lootlocker::core::QueryParameters query;
    query.Add("a", 1.0);
    query.Add("b", 1.5);
    query.Add("c", 0.1);
    CHECK_EQ(query.ToQueryString(), "a=1&b=1.5&c=0.1");
}

// --- request bodies (T13d) -------------------------------------------------

} // namespace

namespace lootlocker {

// The nested type every session start sends. It is the reason the body hook is an ADL
// `to_json` rather than a definition hook: one serialiser, reused by three bodies.
struct SessionOptionals {
    std::string timezone;
    std::string player_name;
    bool        auto_create_profile = true;
};

void to_json(JsonBuilder& b, const SessionOptionals& in)
{
    b.Set("timezone", in.timezone);
    b.Set("player_name", in.player_name);
    b.Set("auto_create_profile", in.auto_create_profile);
}

struct GuestSessionRequest {
    std::string      game_key;
    std::string      game_version;
    std::string      player_identifier;
    SessionOptionals optionals;
};

void to_json(JsonBuilder& b, const GuestSessionRequest& in)
{
    b.Set("game_key", in.game_key);
    b.Set("game_version", in.game_version);
    b.Set("player_identifier", in.player_identifier);
    b.Set("optionals", in.optionals);   // the nested type, through its own to_json
}

struct GuestSessionResponse {
    std::string session_token;
};

Result<Empty> from_json(const JsonValue& j, GuestSessionResponse& out)
{
    out.session_token = j["session_token"].AsString();
    return Result<Empty>::Ok(Empty());
}

namespace core {

struct GuestSessionDef {
    static constexpr const char* kMethod = "POST";
    static constexpr const char* kPath   = "/game/v2/session/guest";

    using Request  = GuestSessionRequest;
    using Response = GuestSessionResponse;

    static void ConstructQueryParameters(const Request&, QueryParameters&) {}

    static void ConstructHeaders(const Request&, HeaderMap&) {}
};

} // namespace core
} // namespace lootlocker

namespace {

using lootlocker::GuestSessionRequest;
using lootlocker::GuestSessionResponse;
using lootlocker::core::GuestSessionDef;

// A POST carries the serialised body and a JSON content type.
void TestRequestBodyIsSent()
{
    Fixture f;

    GuestSessionRequest request;
    request.game_key          = "gk";
    request.game_version      = "1.0.0";
    request.player_identifier = "player-1";
    request.optionals.timezone      = "Europe/Stockholm";
    request.optionals.player_name   = "Erik";
    request.optionals.auto_create_profile = false;

    lootlocker::core::Send<lootlocker::core::GuestSessionDef>(
        *f.client, request, [](Result<GuestSessionResponse>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK_EQ(sent->method, "POST");

    std::string content_type;
    CHECK(sent->headers.Get("Content-Type", content_type));
    CHECK_EQ(content_type, "application/json");

    // The nested object is serialised through its own `to_json`, which is the whole point of
    // putting the hook on the type.
    //
    // Keys come out in sorted order, not insertion order: nlohmann stores an object in a
    // `std::map`. That is correct for JSON, where an object is unordered by definition, and
    // the server parses rather than pattern-matches. It is asserted exactly so a change in
    // that behaviour is visible.
    CHECK_EQ(sent->body,
             R"({"game_key":"gk","game_version":"1.0.0",)"
             R"("optionals":{"auto_create_profile":false,"player_name":"Erik",)"
             R"("timezone":"Europe/Stockholm"},"player_identifier":"player-1"})");
}

// A GET definition has no `to_json`, so it sends no body and no content type. This is what
// keeps the existing definitions unchanged.
void TestNoBodyWithoutToJson()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 1;

    lootlocker::core::Send<ListBroadcastsDef>(*f.client, request,
                                              [](Result<ListBroadcastsResponse>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK_EQ(sent->body, "");
    CHECK(!sent->headers.Has("Content-Type"));
}

// The trait is what makes the two cases above work, so it is asserted directly.
void TestBodyTrait()
{
    CHECK(lootlocker::core::HasToJson<GuestSessionRequest>::value);
    CHECK(!lootlocker::core::HasToJson<ListBroadcastsRequest>::value);
}

// A string literal must not become `true`, the same trap `QueryParameters::Add` had.
void TestBuilderStringLiteral()
{
    lootlocker::JsonBuilder b;
    b.Set("name", "abc");
    CHECK_EQ(b.ToJson(), R"({"name":"abc"})");
}

// A uint64 above 2^63 must round-trip rather than wrap negative.
void TestBuilderUnsigned()
{
    lootlocker::JsonBuilder b;
    b.Set("big", static_cast<std::uint64_t>(18446744073709551615ULL));
    CHECK_EQ(b.ToJson(), R"({"big":18446744073709551615})");
}

// A null `const char*` is JSON null, not an empty string.
void TestBuilderNullPointer()
{
    lootlocker::JsonBuilder b;
    const char* nothing = nullptr;
    b.Set("x", nothing);
    CHECK_EQ(b.ToJson(), R"({"x":null})");
}

// Invalid UTF-8 must not abort the process. With exceptions off, nlohmann's default error
// handler throws, and a throw under `JSON_NOEXCEPTION` is an abort.
void TestBuilderInvalidUtf8DoesNotAbort()
{
    lootlocker::JsonBuilder b;
    // Split so `\xFE` does not swallow the following `b` as a hex digit.
    b.Set("name", std::string("bad\xFF\xFE" "byte"));
    const std::string out = b.ToJson();
    CHECK(!out.empty());
    // The bad bytes are replaced with U+FFFD rather than aborting, and the rest of the
    // string survives.
    CHECK(out.find("\"name\":\"bad") != std::string::npos);
    CHECK(out.find("byte\"") != std::string::npos);
}

// A 204 is the one legitimate empty success (`EndSession`). Parsing an empty body would
// report `MalformedResponse` for a request that succeeded.
void TestNoContentIsSuccess()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 1;

    bool      called = false;
    bool      ok     = false;
    ErrorCode code   = ErrorCode::None;

    lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [&](Result<ListBroadcastsResponse> result) {
            called = true;
            ok     = result.Ok();
            if (!result.Ok()) {
                code = result.Error().code;
            }
        });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 204, "");
    f.client->Update();

    CHECK(called);
    CHECK(ok);
    CHECK(code == ErrorCode::None);
}

// A malformed body must keep the transport metadata: `request_id` is what ties the failure
// to a server log line, and `status_code` says the request was actually answered.
void TestMalformedBodyKeepsRequestMetadata()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 1;

    std::string request_id;
    int         status = 0;
    ErrorCode   code   = ErrorCode::None;

    lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [&](Result<ListBroadcastsResponse> result) {
            if (!result.Ok()) {
                request_id = result.Error().request_id;
                status     = result.Error().status_code;
                code       = result.Error().code;
            }
        });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200, "not json");
    f.client->Update();

    CHECK(code == ErrorCode::MalformedResponse);
    CHECK_EQ_INT(status, 200);
    CHECK(!request_id.empty());
}

// A malformed nested item is an error, not an empty string. Without this the decoder would
// silently default a field and the generic layer would look correct while proving nothing.
void TestMalformedNestedItem()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 1;

    bool      called = false;
    ErrorCode code   = ErrorCode::None;

    lootlocker::core::Send<ListBroadcastsDef>(
        *f.client, request, [&](Result<ListBroadcastsResponse> result) {
            called = true;
            if (!result.Ok()) {
                code = result.Error().code;
            }
        });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200,
                          R"({"broadcasts":[{"id":"a"}]})");
    f.client->Update();

    CHECK(called);
    CHECK(code == ErrorCode::MalformedResponse);
}

// The SDK version and instance identifier headers the other SDKs send, and which the
// backend's tracing middleware reads.
void TestSdkAndInstanceHeaders()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 1;

    lootlocker::core::Send<ListBroadcastsDef>(*f.client, request,
                                              [](Result<ListBroadcastsResponse>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    std::string value;
    CHECK(sent->headers.Get("LL-SDK-Version", value));
    CHECK(!value.empty());

    CHECK(sent->headers.Get("LL-Instance-Identifier", value));
    CHECK_EQ_INT(static_cast<long long>(value.size()), 36);   // UUID-shaped

    // Stable for the client's lifetime, and different between clients.
    CHECK_EQ(value, f.client->GetInstanceIdentifier());

    Fixture other;
    CHECK(other.client->GetInstanceIdentifier() != f.client->GetInstanceIdentifier());
}

// `is-development` is read only on the white-label login path, so it is not a default
// header.
void TestIsDevelopmentIsNotADefaultHeader()
{
    Fixture f;

    ListBroadcastsRequest request;
    request.limit = 1;

    lootlocker::core::Send<ListBroadcastsDef>(*f.client, request,
                                              [](Result<ListBroadcastsResponse>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }
    CHECK(!sent->headers.Has("is-development"));
}

// A `uint64` query parameter must be accepted without a narrowing cast, and must not wrap.
void TestQueryParameterUnsigned()
{
    lootlocker::core::QueryParameters query;
    query.Add("id", static_cast<std::uint64_t>(18446744073709551615ULL));
    CHECK_EQ(query.ToQueryString(), "id=18446744073709551615");
}

// The double formatter must not emit a locale's decimal comma. The test forces a comma
// locale, which is the only way to prove the normalisation actually runs.
void TestQueryParameterDoubleIsLocaleIndependent()
{
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous == nullptr ? std::string("C") : std::string(previous);

    // A comma-decimal locale. Not every platform has every locale, so a failure to set one
    // is not a test failure - the assertion below still holds under "C".
    std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
    std::setlocale(LC_NUMERIC, "German_Germany.1252");

    lootlocker::core::QueryParameters query;
    query.Add("ratio", 1.5);
    const std::string encoded = query.ToQueryString();

    std::setlocale(LC_NUMERIC, saved.c_str());

    CHECK_EQ(encoded, "ratio=1.5");
}

} // namespace

int main()
{
    TestTypedWrapperReturnsExecutorId();
    TestDefinitionBuildsTheRequest();
    TestDefaultHeaders();
    TestSuccessfulDecode();
    TestMalformedBody();
    TestWrongShapeIsAnError();
    TestHttpFailure();
    TestNormalizeIsOptional();
    TestQueryParameterEncoding();
    TestQueryParameterEmpty();
    TestQueryParameterDoubleFormatting();
    TestRequestBodyIsSent();
    TestNoBodyWithoutToJson();
    TestBodyTrait();
    TestBuilderStringLiteral();
    TestBuilderUnsigned();
    TestBuilderNullPointer();
    TestBuilderInvalidUtf8DoesNotAbort();
    TestNoContentIsSuccess();
    TestMalformedBodyKeepsRequestMetadata();
    TestMalformedNestedItem();
    TestSdkAndInstanceHeaders();
    TestIsDevelopmentIsNotADefaultHeader();
    TestQueryParameterUnsigned();
    TestQueryParameterDoubleIsLocaleIndependent();
    return testing::Report();
}

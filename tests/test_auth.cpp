// Guest auth (T14).
//
// The verification the task asks for: `auth::StartGuestSession` over
// `POST /game/v2/session/guest`, including `player_identifier` reuse across restarts.

#include <lootlocker/auth.h>
#include <lootlocker/client.h>
#include <lootlocker/config.h>
#include <lootlocker/fake_http_transport.h>
#include <lootlocker/request_id.h>
#include <lootlocker/result.h>
#include <lootlocker/session.h>
#include <lootlocker/state_writer.h>

#include "test_harness.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

using lootlocker::Client;
using lootlocker::Config;
using lootlocker::ErrorCode;
using lootlocker::FakeHttpTransport;
using lootlocker::IHttpTransport;
using lootlocker::NullStateWriter;
using lootlocker::RequestId;
using lootlocker::Result;
using lootlocker::Session;

// A scratch file that removes itself.
struct TempFile {
    std::string path;

    explicit TempFile(const char* name)
    {
        path = std::string("test-auth-") + name + ".txt";
        std::remove(path.c_str());
    }

    ~TempFile() { std::remove(path.c_str()); }
};

Config MakeConfig()
{
    Config config;
    config.domain_key   = "testdomain";
    config.game_key     = "testgamekey";
    config.game_version = "1.0.0";
    return config;
}

// A config with no persistence, for the tests that do not care about it.
//
// Without this, a test that does not inject a writer falls back to the default
// `FileStateWriter`, which reads and writes `lootlocker-state.txt` in the working directory -
// so tests leak state into each other and into the developer's checkout. That is exactly
// what happened: a stale file made a sabotage test pass.
Config MakeConfigWithoutPersistence(std::shared_ptr<NullStateWriter>& keep_alive)
{
    Config config = MakeConfig();
    keep_alive    = std::make_shared<NullStateWriter>();
    config.state_writer = keep_alive;
    return config;
}

// A successful guest session response, as the API sends it.
const char* kSessionBody = R"({
    "session_token": "tok-abc",
    "player_identifier": "pid-1",
    "player_ulid": "01ARZ3NDEKTSV4RRFFQ69G5FAV",
    "public_uid": "ABCD1234",
    "player_name": "Erik",
    "player_id": 9007199254740993,
    "player_created_at": "2026-01-01T00:00:00Z",
    "seen_before": true,
    "check_grant_notifications": false,
    "check_deactivation_notifications": true
})";

struct Fixture {
    std::shared_ptr<FakeHttpTransport> transport;
    std::unique_ptr<Client>            client;

    explicit Fixture(const Config& config_in)
    {
        transport        = std::make_shared<FakeHttpTransport>();
        Config config    = config_in;
        config.transport = transport;
        client.reset(new Client(config));
    }

    const IHttpTransport::Request* Sent() const
    {
        if (transport->Order().empty()) {
            return nullptr;
        }
        return transport->FindRequest(transport->Order().front());
    }
};

// --- the request -----------------------------------------------------------

// The endpoint, method and body are what the API expects.
void TestGuestSessionRequest()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK_EQ(sent->method, "POST");
    CHECK_EQ(sent->url, "https://testdomain.api.lootlocker.com/game/v2/session/guest");

    // The body carries the config's game key and version, the guest platform, and a
    // generated identifier.
    CHECK(sent->body.find(R"("game_key":"testgamekey")") != std::string::npos);
    CHECK(sent->body.find(R"("game_version":"1.0.0")") != std::string::npos);
    CHECK(sent->body.find(R"("platform":"guest")") != std::string::npos);
    CHECK(sent->body.find(R"("player_identifier":")") != std::string::npos);

    // No session yet, so no token is sent.
    CHECK(!sent->headers.Has("x-session-token"));
}

// The optionals are sent only when set. `timezone` is the exception: it is resolved from the
// config, the provider and finally the default, so it is always present unless
// `send_timezone` is false (covered by `TestSendTimezoneFalse`).
void TestOptionalsAreOmittedWhenEmpty()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK(sent->body.find("locale") == std::string::npos);
    CHECK(sent->body.find("player_name") == std::string::npos);
    CHECK(sent->body.find("character_name") == std::string::npos);
}

void TestOptionalsAreSentWhenSet()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    lootlocker::GuestSessionOptions opts;
    opts.timezone    = "Europe/Stockholm";
    opts.locale      = "sv-SE";
    opts.player_name = "Erik";

    lootlocker::auth::StartGuestSession(*f.client, opts, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK(sent->body.find(R"("timezone":"Europe/Stockholm")") != std::string::npos);
    CHECK(sent->body.find(R"("locale":"sv-SE")") != std::string::npos);
    CHECK(sent->body.find(R"("player_name":"Erik")") != std::string::npos);
}

// --- the response ----------------------------------------------------------

// A successful response is decoded, stored on the client, and delivered.
void TestSuccessfulSession()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    bool    called = false;
    Session received;

    lootlocker::auth::StartGuestSession(*f.client, [&](Result<Session> result) {
        called = true;
        CHECK(result.Ok());
        if (result.Ok()) {
            received = result.Value();
        }
    });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200, kSessionBody);
    f.client->Update();

    CHECK(called);
    CHECK_EQ(received.session_token, "tok-abc");
    CHECK_EQ(received.player_identifier, "pid-1");
    CHECK_EQ(received.player_ulid, "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    CHECK_EQ(received.public_uid, "ABCD1234");
    CHECK_EQ(received.player_name, "Erik");
    CHECK(received.player_id == 9007199254740993ULL);   // exact, above 2^53
    CHECK(received.seen_before);
    CHECK(!received.check_grant_notifications);
    CHECK(received.check_deactivation_notifications);

    // Stored on the client, so the caller does not have to remember to do it.
    CHECK(f.client->HasSession());
    CHECK_EQ(f.client->GetSession().session_token, "tok-abc");
}

// The next request carries the token.
void TestSessionTokenIsSentOnSubsequentRequests()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200, kSessionBody);
    f.client->Update();

    CHECK(f.client->HasSession());

    // A second request, after the session exists.
    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();

    const auto order = f.transport->Order();
    CHECK_EQ_INT(static_cast<long long>(order.size()), 2);
    if (order.size() < 2) {
        return;
    }

    const IHttpTransport::Request* second = f.transport->FindRequest(order[1]);
    CHECK(second != nullptr);
    if (second == nullptr) {
        return;
    }

    std::string token;
    CHECK(second->headers.Get("x-session-token", token));
    CHECK_EQ(token, "tok-abc");
}

// A missing required field is a decode error, not a silent default (D23).
void TestMissingRequiredFieldIsAnError()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    bool      called = false;
    ErrorCode code   = ErrorCode::None;

    lootlocker::auth::StartGuestSession(*f.client, [&](Result<Session> result) {
        called = true;
        if (!result.Ok()) {
            code = result.Error().code;
        }
    });

    f.client->Update();
    // No session_token.
    f.transport->Complete(f.transport->Order().front(), 200,
                          R"({"player_identifier":"p","player_ulid":"u","public_uid":"x","player_id":1})");
    f.client->Update();

    CHECK(called);
    CHECK(code == ErrorCode::MalformedResponse);
    CHECK(!f.client->HasSession());
}

// An absent optional field is not an error: an absent name genuinely means "not set".
void TestMissingOptionalFieldIsFine()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    bool called = false;
    bool ok     = false;

    lootlocker::auth::StartGuestSession(*f.client, [&](Result<Session> result) {
        called = true;
        ok     = result.Ok();
    });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200,
                          R"({"session_token":"t","player_identifier":"p","player_ulid":"u",)"
                          R"("public_uid":"x","player_id":1})");
    f.client->Update();

    CHECK(called);
    CHECK(ok);
    CHECK(f.client->HasSession());
    CHECK_EQ(f.client->GetSession().player_name, "");
}

// --- identifier reuse across restarts --------------------------------------

// The whole point of guest auth: the same identifier on the next launch, so the player is
// the same account.
//
// The writer is held in a named `shared_ptr` in each scope, not assigned as a temporary:
// `Config` holds a `weak_ptr`, so a temporary expires before the client is constructed and
// the SDK silently falls back to the default file - which would make this test pass for the
// wrong reason.
void TestPlayerIdentifierIsReusedAcrossRestarts()
{
    TempFile file("reuse");

    std::string first_identifier;

    {
        std::shared_ptr<lootlocker::FileStateWriter> writer =
            std::make_shared<lootlocker::FileStateWriter>(file.path);

        Config config = MakeConfig();
        config.state_writer = writer;

        Fixture f(config);
        lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
        f.client->Update();

        const IHttpTransport::Request* sent = f.Sent();
        CHECK(sent != nullptr);
        if (sent != nullptr) {
            // Pull the identifier out of the body.
            const std::string marker = R"("player_identifier":")";
            const std::size_t at = sent->body.find(marker);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) {
                const std::size_t start = at + marker.size();
                const std::size_t end   = sent->body.find('"', start);
                first_identifier = sent->body.substr(start, end - start);
            }
        }
        CHECK(!first_identifier.empty());
    }

    // A new client, as if the game restarted, reading the same file.
    {
        std::shared_ptr<lootlocker::FileStateWriter> writer =
            std::make_shared<lootlocker::FileStateWriter>(file.path);

        Config config = MakeConfig();
        config.state_writer = writer;

        Fixture f(config);
        lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
        f.client->Update();

        const IHttpTransport::Request* sent = f.Sent();
        CHECK(sent != nullptr);
        if (sent != nullptr) {
            const std::string marker = R"("player_identifier":")";
            const std::size_t at = sent->body.find(marker);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) {
                const std::size_t start = at + marker.size();
                const std::size_t end   = sent->body.find('"', start);
                const std::string second = sent->body.substr(start, end - start);
                CHECK_EQ(second, first_identifier);
            }
        }
    }
}

// An explicit identifier wins over the persisted one.
void TestExplicitIdentifierWins()
{
    TempFile file("explicit");

    std::shared_ptr<lootlocker::FileStateWriter> writer =
        std::make_shared<lootlocker::FileStateWriter>(file.path);

    Config config = MakeConfig();
    config.state_writer = writer;

    Fixture f(config);

    lootlocker::GuestSessionOptions opts;
    opts.player_identifier = "chosen-by-the-game";

    lootlocker::auth::StartGuestSession(*f.client, opts, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent != nullptr) {
        CHECK(sent->body.find(R"("player_identifier":"chosen-by-the-game")") !=
              std::string::npos);
    }
}

// With no persistence, every launch generates a new identifier - which is the documented
// cost of a `NullStateWriter`.
//
// The writer is held in a named `shared_ptr`, not assigned as a temporary: `Config` holds a
// `weak_ptr`, so `config.state_writer = std::make_shared<...>()` expires before the client
// is constructed and the SDK silently falls back to the file default. This test caught that
// exact mistake in its own first draft, which is why the README warns about it.
void TestNullWriterGeneratesNewIdentifierEachTime()
{
    std::string first;
    std::string second;

    for (std::string* out : {&first, &second}) {
        std::shared_ptr<NullStateWriter> writer = std::make_shared<NullStateWriter>();

        Config config = MakeConfig();
        config.state_writer = writer;

        Fixture f(config);
        lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
        f.client->Update();

        const IHttpTransport::Request* sent = f.Sent();
        CHECK(sent != nullptr);
        if (sent != nullptr) {
            const std::string marker = R"("player_identifier":")";
            const std::size_t at = sent->body.find(marker);
            if (at != std::string::npos) {
                const std::size_t start = at + marker.size();
                const std::size_t end   = sent->body.find('"', start);
                *out = sent->body.substr(start, end - start);
            }
        }
    }

    CHECK(!first.empty());
    CHECK(!second.empty());
    CHECK(first != second);
}

// The optionals are nested under `optionals`, not at the root. The backend's `GuestInput`
// carries `Optionals Optionals` tagged `json:"optionals"`, so a root-level `timezone` is
// silently ignored and the player gets UTC.
void TestOptionalsAreNested()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    lootlocker::GuestSessionOptions opts;
    opts.timezone    = "Europe/Stockholm";
    opts.locale      = "sv-SE";
    opts.player_name = "Erik";

    lootlocker::auth::StartGuestSession(*f.client, opts, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK(sent->body.find(R"("optionals":{)") != std::string::npos);
    CHECK(sent->body.find(R"("timezone":"Europe/Stockholm")") != std::string::npos);
    CHECK(sent->body.find(R"("locale":"sv-SE")") != std::string::npos);
    CHECK(sent->body.find(R"("player_name":"Erik")") != std::string::npos);

    // And not at the root: the only root-level keys are the four the API expects.
    const std::size_t optionals_at = sent->body.find(R"("optionals")");
    CHECK(sent->body.find(R"("timezone")") > optionals_at);
}

// The timezone falls back to the config, then the provider, then the default - and is
// normalised to an IANA name, because a Windows name sent as-is gets UTC server-side.
void TestTimezoneResolution()
{
    std::shared_ptr<NullStateWriter> keep_alive;

    // A Windows name in the config is normalised.
    Config config = MakeConfigWithoutPersistence(keep_alive);
    config.timezone = "W. Europe Standard Time";

    Fixture f(config);
    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent != nullptr) {
        CHECK(sent->body.find(R"("timezone":"Europe/Berlin")") != std::string::npos);
    }
}

// `send_timezone = false` sends no timezone at all, which is a different statement from
// "I have no opinion".
void TestSendTimezoneFalse()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Config config = MakeConfigWithoutPersistence(keep_alive);
    config.send_timezone = false;

    Fixture f(config);
    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent != nullptr) {
        CHECK(sent->body.find("timezone") == std::string::npos);
    }
}

// A 200 with an empty body is a malformed response for a definition whose response carries
// required fields. Without this it would become an all-default `Session` and `HasSession()`
// would report true with no token.
void TestEmptyBodyIsNotASuccess()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    bool      called = false;
    ErrorCode code   = ErrorCode::None;

    lootlocker::auth::StartGuestSession(*f.client, [&](Result<Session> result) {
        called = true;
        if (!result.Ok()) {
            code = result.Error().code;
        }
    });

    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200, "");
    f.client->Update();

    CHECK(called);
    CHECK(code == ErrorCode::MalformedResponse);
    CHECK(!f.client->HasSession());
}

// A present field of the wrong type says so, rather than claiming it is missing.
void TestWrongTypeMessage()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    std::string message;

    lootlocker::auth::StartGuestSession(*f.client, [&](Result<Session> result) {
        if (!result.Ok()) {
            message = result.Error().message;
        }
    });

    f.client->Update();
    // `session_token` is present but a number.
    f.transport->Complete(f.transport->Order().front(), 200,
                          R"({"session_token":42,"player_identifier":"p","player_ulid":"u",)"
                          R"("public_uid":"x","player_id":1})");
    f.client->Update();

    CHECK(message.find("non-string") != std::string::npos);
    CHECK(message.find("missing") == std::string::npos);
}

// An explicit identifier is persisted too, so a game that supplies one on the first launch
// and relies on the SDK afterwards does not get a different account.
void TestExplicitIdentifierIsPersisted()
{
    TempFile file("explicit-persist");

    std::shared_ptr<lootlocker::FileStateWriter> writer =
        std::make_shared<lootlocker::FileStateWriter>(file.path);

    Config config = MakeConfig();
    config.state_writer = writer;

    {
        Fixture f(config);

        lootlocker::GuestSessionOptions opts;
        opts.player_identifier = "chosen-by-the-game";

        lootlocker::auth::StartGuestSession(*f.client, opts, [](Result<Session>) {});
        f.client->Update();
    }

    // A second launch, with no explicit identifier: the persisted one is used.
    std::shared_ptr<lootlocker::FileStateWriter> reopened =
        std::make_shared<lootlocker::FileStateWriter>(file.path);

    Config second = MakeConfig();
    second.state_writer = reopened;

    Fixture f(second);
    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent != nullptr) {
        CHECK(sent->body.find(R"("player_identifier":"chosen-by-the-game")") !=
              std::string::npos);
    }
}

// --- the client accessors --------------------------------------------------

void TestSessionAccessors()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Client client(MakeConfigWithoutPersistence(keep_alive));

    CHECK(!client.HasSession());

    Session session;
    session.session_token = "t";
    client.SetSession(session);

    CHECK(client.HasSession());
    CHECK_EQ(client.GetSession().session_token, "t");

    client.ClearSession();
    CHECK(!client.HasSession());
}

// The task's stated verification: the typed wrapper returns the id the executor assigned.
void TestReturnsExecutorId()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    const RequestId returned =
        lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});

    CHECK(returned.IsValid());
}

} // namespace

int main()
{
    TestGuestSessionRequest();
    TestOptionalsAreOmittedWhenEmpty();
    TestOptionalsAreSentWhenSet();
    TestSuccessfulSession();
    TestSessionTokenIsSentOnSubsequentRequests();
    TestMissingRequiredFieldIsAnError();
    TestMissingOptionalFieldIsFine();
    TestPlayerIdentifierIsReusedAcrossRestarts();
    TestExplicitIdentifierWins();
    TestExplicitIdentifierIsPersisted();
    TestNullWriterGeneratesNewIdentifierEachTime();
    TestOptionalsAreNested();
    TestTimezoneResolution();
    TestSendTimezoneFalse();
    TestEmptyBodyIsNotASuccess();
    TestWrongTypeMessage();
    TestSessionAccessors();
    TestReturnsExecutorId();
    return testing::Report();
}

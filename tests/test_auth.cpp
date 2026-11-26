// Guest auth (T14).
//
// The verification the task asks for: `auth::StartGuestSession` over
// `POST /game/v2/session/guest`, including the guest identifier's reuse across restarts.

#include <lootlocker/auth.h>
#include <lootlocker/client.h>
#include <lootlocker/config.h>
#include <lootlocker/fake_http_transport.h>
#include <lootlocker/platform.h>
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
using lootlocker::GuestSessionInputs;
using lootlocker::IHttpTransport;
using lootlocker::NullStateWriter;
using lootlocker::Platform;
using lootlocker::RequestId;
using lootlocker::Result;
using lootlocker::Session;
using lootlocker::SessionOptionals;

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
// what happened once: a stale file made a sabotage test pass.
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

// Pulls a string field out of a JSON body. Crude on purpose: the tests assert on the wire
// shape, so parsing it with the SDK's own parser would let a bug hide.
std::string FieldFromBody(const std::string& body, const std::string& key)
{
    const std::string marker = "\"" + key + "\":\"";
    const std::size_t at = body.find(marker);
    if (at == std::string::npos) {
        return std::string();
    }
    const std::size_t start = at + marker.size();
    const std::size_t end   = body.find('"', start);
    if (end == std::string::npos) {
        return std::string();
    }
    return body.substr(start, end - start);
}

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

    CHECK(sent->body.find(R"("game_key":"testgamekey")") != std::string::npos);
    CHECK(sent->body.find(R"("game_version":"1.0.0")") != std::string::npos);
    CHECK(sent->body.find(R"("platform":"guest")") != std::string::npos);
    CHECK(!FieldFromBody(sent->body, "player_identifier").empty());
}

// An authentication request is not an authenticated request, so it must not carry a token -
// even when a session from a previous call exists.
void TestTokenIsNotSentOnAuthRequest()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    // Establish a session first.
    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();
    f.transport->Complete(f.transport->Order().front(), 200, kSessionBody);
    f.client->Update();
    CHECK(f.client->HasSession());

    // A second auth request must not carry the token.
    lootlocker::auth::StartGuestSession(*f.client, [](Result<Session>) {});
    f.client->Update();

    const auto order = f.transport->Order();
    CHECK_EQ_INT(static_cast<long long>(order.size()), 2);
    if (order.size() < 2) {
        return;
    }

    const IHttpTransport::Request* second = f.transport->FindRequest(order[1]);
    CHECK(second != nullptr);
    if (second != nullptr) {
        CHECK(!second->headers.Has("x-session-token"));
    }
}

// The optionals are nested under `optionals`, not at the root. The backend's `GuestInput`
// carries `Optionals Optionals` tagged `json:"optionals"`, so a root-level `timezone` is
// silently ignored and the player gets UTC.
void TestOptionalsAreNested()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    GuestSessionInputs inputs;
    SessionOptionals optionals;
    optionals.timezone    = "Europe/Stockholm";
    optionals.player_name = "Erik";
    inputs.optionals      = optionals;

    lootlocker::auth::StartGuestSession(*f.client, inputs, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK(sent->body.find(R"("optionals":{)") != std::string::npos);
    CHECK(sent->body.find(R"("timezone":"Europe/Stockholm")") != std::string::npos);
    CHECK(sent->body.find(R"("player_name":"Erik")") != std::string::npos);

    // And not at the root: `timezone` appears after `optionals` opens.
    const std::size_t optionals_at = sent->body.find(R"("optionals")");
    CHECK(sent->body.find(R"("timezone")") > optionals_at);
}

// The optionals carry the backend's fields, not invented ones.
void TestOptionalsFields()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    GuestSessionInputs inputs;
    SessionOptionals optionals;
    optionals.auto_create_profile  = false;
    optionals.check_dlc            = true;
    optionals.referral_code        = "REF123";
    optionals.referral_program_key = "prog";
    inputs.optionals               = optionals;

    lootlocker::auth::StartGuestSession(*f.client, inputs, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK(sent->body.find(R"("auto_create_profile":false)") != std::string::npos);
    CHECK(sent->body.find(R"("check_dlc":true)") != std::string::npos);
    CHECK(sent->body.find(R"("referral_code":"REF123")") != std::string::npos);
    CHECK(sent->body.find(R"("referral_program_key":"prog")") != std::string::npos);

    // The invented fields are gone.
    CHECK(sent->body.find("locale") == std::string::npos);
    CHECK(sent->body.find("character_name") == std::string::npos);
}

// A field equal to the backend's default is omitted rather than sent.
void TestDefaultsAreOmitted()
{
    std::shared_ptr<NullStateWriter> keep_alive;
    Fixture f(MakeConfigWithoutPersistence(keep_alive));

    GuestSessionInputs inputs;
    inputs.optionals = SessionOptionals();   // all defaults

    lootlocker::auth::StartGuestSession(*f.client, inputs, [](Result<Session>) {});
    f.client->Update();

    const IHttpTransport::Request* sent = f.Sent();
    CHECK(sent != nullptr);
    if (sent == nullptr) {
        return;
    }

    CHECK(sent->body.find("auto_create_profile") == std::string::npos);
    CHECK(sent->body.find("check_dlc") == std::string::npos);
}

// The timezone falls back to the config and is normalised to an IANA name, because a Windows
// name sent as-is gets UTC server-side.
void TestTimezoneResolution()
{
    std::shared_ptr<NullStateWriter> keep_alive;
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

// --- the response ----------------------------------------------------------

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

    CHECK(f.client->HasSession());
    CHECK_EQ(f.client->GetSession().session_token, "tok-abc");
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

// A 200 with an empty body is a malformed response for a definition whose response carries
// required fields.
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
    f.transport->Complete(f.transport->Order().front(), 200,
                          R"({"session_token":42,"player_identifier":"p","player_ulid":"u",)"
                          R"("public_uid":"x","player_id":1})");
    f.client->Update();

    CHECK(message.find("non-string") != std::string::npos);
    CHECK(message.find("missing") == std::string::npos);
}

// --- identifier reuse across restarts --------------------------------------

// The whole point of guest auth: the same identifier on the next launch, so the player is
// the same account.
//
// The writer is held in a named `shared_ptr` in each scope, not assigned as a temporary:
// `Config` holds a `weak_ptr`, so a temporary expires before the client is constructed and
// the SDK silently falls back to the default file - which would make this test pass for the
// wrong reason.
void TestGuestIdentifierIsReusedAcrossRestarts()
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
            first_identifier = FieldFromBody(sent->body, "player_identifier");
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
            CHECK_EQ(FieldFromBody(sent->body, "player_identifier"), first_identifier);
        }
    }
}

// An explicit identifier wins over the persisted one, and is persisted itself.
void TestExplicitIdentifierWinsAndPersists()
{
    TempFile file("explicit");

    std::shared_ptr<lootlocker::FileStateWriter> writer =
        std::make_shared<lootlocker::FileStateWriter>(file.path);

    Config config = MakeConfig();
    config.state_writer = writer;

    {
        Fixture f(config);

        GuestSessionInputs inputs;
        inputs.guest_identifier = std::string("chosen-by-the-game");

        lootlocker::auth::StartGuestSession(*f.client, inputs, [](Result<Session>) {});
        f.client->Update();

        const IHttpTransport::Request* sent = f.Sent();
        CHECK(sent != nullptr);
        if (sent != nullptr) {
            CHECK_EQ(FieldFromBody(sent->body, "player_identifier"), "chosen-by-the-game");
        }
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
        CHECK_EQ(FieldFromBody(sent->body, "player_identifier"), "chosen-by-the-game");
    }
}

// With no persistence, every launch generates a new identifier - which is the documented
// cost of a `NullStateWriter`.
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
            *out = FieldFromBody(sent->body, "player_identifier");
        }
    }

    CHECK(!first.empty());
    CHECK(!second.empty());
    CHECK(first != second);
}

// --- platform --------------------------------------------------------------

void TestPlatformNames()
{
    CHECK_EQ(lootlocker::ToString(Platform::Guest), "guest");
    CHECK_EQ(lootlocker::ToString(Platform::WhiteLabel), "white_label_login");
    CHECK_EQ(lootlocker::ToString(Platform::PlayStationNetwork), "psn");
    CHECK_EQ(lootlocker::ToString(Platform::NintendoSwitch), "nintendo_switch");

    Platform parsed = Platform::None;
    CHECK(lootlocker::TryParsePlatform("steam", parsed));
    CHECK(parsed == Platform::Steam);

    // An unknown name is a contract change, not a missing value.
    CHECK(!lootlocker::TryParsePlatform("not_a_platform", parsed));
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
    TestTokenIsNotSentOnAuthRequest();
    TestOptionalsAreNested();
    TestOptionalsFields();
    TestDefaultsAreOmitted();
    TestTimezoneResolution();
    TestSendTimezoneFalse();
    TestSuccessfulSession();
    TestMissingRequiredFieldIsAnError();
    TestMissingOptionalFieldIsFine();
    TestEmptyBodyIsNotASuccess();
    TestWrongTypeMessage();
    TestGuestIdentifierIsReusedAcrossRestarts();
    TestExplicitIdentifierWinsAndPersists();
    TestNullWriterGeneratesNewIdentifierEachTime();
    TestPlatformNames();
    TestSessionAccessors();
    TestReturnsExecutorId();
    return testing::Report();
}

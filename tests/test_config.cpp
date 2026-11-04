// T9's verification: config.
//
// The task asks for "a test that constructing a client with a hand-filled struct reads no
// file and no environment variable". There is no client yet (T11), so the equivalent
// assertion here is that a hand-filled struct is usable with no loader called at all, and
// that the loaders only ever touch what they are given.

#include "test_harness.h"

#include <lootlocker/config.h>
#include <lootlocker/config_cli.h>
#include <lootlocker/config_file.h>
#include <lootlocker/result.h>

#include <cstdlib>
#include <string>

namespace {

using lootlocker::Config;
using lootlocker::ErrorCode;
using lootlocker::Result;
using lootlocker::Empty;

Config MakeValid()
{
    Config c;
    c.domain_key = "1g9glch3";
    c.game_key = "prod_1c52468fc6e8420c955e3b6c303ea8cc";
    c.game_version = "1.2.1";
    return c;
}

void TestHandFilledStructIsValid()
{
    // No loader called, no file read, no environment variable consulted. This is the
    // whole point of D9.
    const Config c = MakeValid();
    CHECK(lootlocker::ValidateConfig(c).Ok());
}

void TestBaseUrlIsDerivedFromDomainKey()
{
    const Config c = MakeValid();
    CHECK_EQ(c.ResolveBaseUrl(), "https://1g9glch3.api.lootlocker.com");
}

void TestBaseUrlOverrideWins()
{
    Config c = MakeValid();
    c.base_url = "http://localhost:8080";
    CHECK_EQ(c.ResolveBaseUrl(), "http://localhost:8080");
}

void TestMissingDomainKeyIsInvalid()
{
    Config c = MakeValid();
    c.domain_key.clear();

    const auto e = lootlocker::ValidateConfig(c);
    CHECK(!e.Ok());
    CHECK(e.code == ErrorCode::InvalidConfig);
}

void TestMissingBothKeysIsInvalid()
{
    Config c = MakeValid();
    c.game_key.clear();
    c.game_api_key.clear();

    const auto e = lootlocker::ValidateConfig(c);
    CHECK(!e.Ok());
    CHECK(e.code == ErrorCode::InvalidConfig);
}

void TestSteamKeyAloneIsValid()
{
    Config c = MakeValid();
    c.game_key.clear();
    c.game_api_key = "steam_key";

    CHECK(lootlocker::ValidateConfig(c).Ok());
}

void TestGameVersionMustBeSemver()
{
    const char* bad[] = {"", "1", "1.2", "1.2.3.4", "v1.2.3", "1.2.x", "01.2.3", "1..3"};
    for (const char* v : bad) {
        Config c = MakeValid();
        c.game_version = v;
        const auto e = lootlocker::ValidateConfig(c);
        CHECK(!e.Ok());
        CHECK(e.code == ErrorCode::InvalidConfig);
    }

    const char* good[] = {"0.0.0", "1.2.3", "10.20.30", "1.2.3-rc.1", "1.2.3+build.5"};
    for (const char* v : good) {
        Config c = MakeValid();
        c.game_version = v;
        CHECK(lootlocker::ValidateConfig(c).Ok());
    }
}

void TestRateLimitPerBucketLimitIsDerived()
{
    // floor(280 * 0.8 * 5 / 60) = floor(18.67) = 18
    const lootlocker::RateLimitConfig r;
    CHECK_EQ_INT(r.PerBucketLimit(), 18);
}

void TestRateLimitPerBucketLimitFollowsTheFraction()
{
    lootlocker::RateLimitConfig r;
    r.moving_average_fraction = 0.5;
    // floor(280 * 0.5 * 5 / 60) = floor(11.67) = 11
    CHECK_EQ_INT(r.PerBucketLimit(), 11);
}

void TestCliAppliesLongFlags()
{
    Config c;
    const char* argv[] = {"game", "--domain-key", "abc", "--game-key=xyz", "--game-version",
                          "1.0.0", "--is-development"};

    const auto r = lootlocker::config_cli::Apply(7, argv, c);
    CHECK(r.Ok());
    CHECK_EQ(c.domain_key, "abc");
    CHECK_EQ(c.game_key, "xyz");
    CHECK_EQ(c.game_version, "1.0.0");
    CHECK(c.is_development);
}

void TestCliIgnoresUnknownFlags()
{
    // A host's own flags must not make the helper unusable.
    Config c;
    const char* argv[] = {"game", "--fullscreen", "--domain-key", "abc", "-w", "1920"};

    const auto r = lootlocker::config_cli::Apply(6, argv, c);
    CHECK(r.Ok());
    CHECK_EQ(c.domain_key, "abc");
}

void TestCliMissingValueIsAnError()
{
    Config c;
    const char* argv[] = {"game", "--domain-key"};

    const auto r = lootlocker::config_cli::Apply(2, argv, c);
    CHECK(!r.Ok());
    CHECK(r.Error().code == ErrorCode::InvalidConfig);
}

void TestFileLoaderParsesJson()
{
    Config c;
    const auto r = lootlocker::config_file::LoadFromString(
        R"({"domain_key":"abc","game_key":"k","game_version":"1.0.0","is_development":true})", c);

    CHECK(r.Ok());
    CHECK_EQ(c.domain_key, "abc");
    CHECK_EQ(c.game_key, "k");
    CHECK_EQ(c.game_version, "1.0.0");
    CHECK(c.is_development);
}

void TestFileLoaderLeavesAbsentFieldsUntouched()
{
    // Layering: a file over a hand-filled struct only overwrites what it contains.
    Config c = MakeValid();
    const auto r = lootlocker::config_file::LoadFromString(R"({"game_version":"9.9.9"})", c);

    CHECK(r.Ok());
    CHECK_EQ(c.game_version, "9.9.9");
    CHECK_EQ(c.domain_key, "1g9glch3");   // untouched
}

void TestFileLoaderParsesNestedSections()
{
    Config c;
    const auto r = lootlocker::config_file::LoadFromString(
        R"({"queue":{"max_retries":9},"rate_limit":{"enabled":false}})", c);

    CHECK(r.Ok());
    CHECK_EQ_INT(c.queue.max_retries, 9);
    CHECK(!c.rate_limit.enabled);
}

void TestFileLoaderRejectsInvalidJson()
{
    Config c;
    const auto r = lootlocker::config_file::LoadFromString("{not json", c);

    CHECK(!r.Ok());
    CHECK(r.Error().code == ErrorCode::MalformedResponse);
    // D7: the raw body is carried on failure, so the adopter can see what was in the file.
    CHECK_EQ(r.RawBody(), "{not json");
}

void TestFileLoaderRejectsWrongTypes()
{
    Config c;
    const auto r = lootlocker::config_file::LoadFromString(R"({"domain_key":42})", c);

    CHECK(!r.Ok());
    CHECK(r.Error().code == ErrorCode::MalformedResponse);
}

void TestFileLoaderRejectsNonObject()
{
    Config c;
    const auto r = lootlocker::config_file::LoadFromString("[1,2,3]", c);

    CHECK(!r.Ok());
    CHECK(r.Error().code == ErrorCode::MalformedResponse);
}

void TestFileLoaderMissingFileIsTransportError()
{
    Config c;
    const auto r = lootlocker::config_file::Load("this-file-does-not-exist.json", c);

    CHECK(!r.Ok());
    CHECK(r.Error().code == ErrorCode::Transport);
}

} // namespace

int main()
{
    TestHandFilledStructIsValid();
    TestBaseUrlIsDerivedFromDomainKey();
    TestBaseUrlOverrideWins();
    TestMissingDomainKeyIsInvalid();
    TestMissingBothKeysIsInvalid();
    TestSteamKeyAloneIsValid();
    TestGameVersionMustBeSemver();
    TestRateLimitPerBucketLimitIsDerived();
    TestRateLimitPerBucketLimitFollowsTheFraction();
    TestCliAppliesLongFlags();
    TestCliIgnoresUnknownFlags();
    TestCliMissingValueIsAnError();
    TestFileLoaderParsesJson();
    TestFileLoaderLeavesAbsentFieldsUntouched();
    TestFileLoaderParsesNestedSections();
    TestFileLoaderRejectsInvalidJson();
    TestFileLoaderRejectsWrongTypes();
    TestFileLoaderRejectsNonObject();
    TestFileLoaderMissingFileIsTransportError();

    return testing::Report();
}

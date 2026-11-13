// Config obfuscation (D26, R29).
//
// The round trip is the easy part. The tests that matter are the ones that pin down what
// this is NOT: it must not be described as secure, and it must not silently accept
// garbage.

#include <lootlocker/config.h>
#include <lootlocker/config_file.h>
#include <lootlocker/config_obfuscation.h>

#include "test_harness.h"

#include <string>

namespace {

void TestBase64RoundTrip()
{
    using lootlocker::config_obfuscation::Base64Decode;
    using lootlocker::config_obfuscation::Base64Encode;

    // The three padding cases, because they are where a hand-written encoder goes wrong.
    CHECK_EQ(Base64Encode(""), "");
    CHECK_EQ(Base64Encode("f"), "Zg==");
    CHECK_EQ(Base64Encode("fo"), "Zm8=");
    CHECK_EQ(Base64Encode("foo"), "Zm9v");
    CHECK_EQ(Base64Encode("foob"), "Zm9vYg==");
    CHECK_EQ(Base64Encode("fooba"), "Zm9vYmE=");
    CHECK_EQ(Base64Encode("foobar"), "Zm9vYmFy");

    // The empty string is deliberately excluded: `Base64Encode("")` is `""`, and
    // `IsValidBase64("")` is false because an empty config file is not a config. That
    // asymmetry is intentional and asserted separately.
    const char* const inputs[] = {"f", "fo", "foo", "foob", "fooba", "foobar",
                                  "{\"domain_key\":\"abc\"}"};
    for (std::size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        const std::string encoded = Base64Encode(inputs[i]);
        const auto decoded = Base64Decode(encoded);
        CHECK(decoded.Ok());
        if (decoded.Ok()) {
            CHECK_EQ(decoded.Value(), inputs[i]);
        }
    }
}

void TestBase64RejectsGarbage()
{
    using lootlocker::config_obfuscation::Base64Decode;
    using lootlocker::config_obfuscation::IsValidBase64;

    CHECK(!IsValidBase64(""));
    CHECK(!IsValidBase64("abc"));        // not a multiple of four
    CHECK(!IsValidBase64("ab=c"));       // padding in the middle
    CHECK(!IsValidBase64("a==="));       // too much padding
    CHECK(!IsValidBase64("ab!d"));       // not a Base64 digit
    CHECK(!IsValidBase64("{\"a\":1}"));  // JSON is not Base64

    CHECK(IsValidBase64("Zm9v"));
    CHECK(IsValidBase64("Zg=="));
    CHECK(IsValidBase64("Zm8="));

    CHECK(!Base64Decode("abc").Ok());
    CHECK(!Base64Decode("").Ok());
}

void TestEmptyStringAsymmetry()
{
    using lootlocker::config_obfuscation::Base64Encode;
    using lootlocker::config_obfuscation::IsValidBase64;
    using lootlocker::config_obfuscation::Obfuscate;

    // Encoding an empty string yields an empty string, which is correct Base64. But
    // `IsValidBase64("")` is false, because its caller is the config loader and an empty
    // file is not a config. The asymmetry is deliberate: the encoder is a general utility,
    // the validator answers a question about config files.
    CHECK_EQ(Base64Encode(""), "");
    CHECK_EQ(Obfuscate(""), "");
    CHECK(!IsValidBase64(""));
}

void TestObfuscateRoundTrip()
{
    using lootlocker::config_obfuscation::Deobfuscate;
    using lootlocker::config_obfuscation::Obfuscate;

    // The empty string is excluded for the same reason as above.
    const char* const inputs[] = {
        "a",
        "{\"domain_key\":\"my-domain\",\"game_key\":\"my-game\"}",
        "{\"domain_key\":\"d\",\"game_version\":\"1.0.0\",\"log_level\":\"Verbose\"}",
    };
    for (std::size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        const std::string obfuscated = Obfuscate(inputs[i]);
        const auto plain = Deobfuscate(obfuscated);
        CHECK(plain.Ok());
        if (plain.Ok()) {
            CHECK_EQ(plain.Value(), inputs[i]);
        }
    }
}

void TestObfuscatedOutputIsNotThePlaintext()
{
    using lootlocker::config_obfuscation::Obfuscate;

    // The whole point: a player opening the file must not read the API key.
    const std::string plain = "{\"game_api_key\":\"a-very-recognisable-key\"}";
    const std::string obfuscated = Obfuscate(plain);

    CHECK(obfuscated.find("a-very-recognisable-key") == std::string::npos);
    CHECK(obfuscated.find("game_api_key") == std::string::npos);
    CHECK(obfuscated.find('{') == std::string::npos);

    // And it is ASCII, so it survives a text editor.
    for (std::size_t i = 0; i < obfuscated.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(obfuscated[i]);
        CHECK(c >= 0x20 && c < 0x7F);
    }
}

void TestObfuscationIsDeterministic()
{
    using lootlocker::config_obfuscation::Obfuscate;

    // No random IV, deliberately: a config file that changes every time it is written is
    // a config file that shows up in every diff. The trade is that identical plaintext
    // produces identical output, which is fine for obfuscation and would not be for
    // encryption.
    const std::string plain = "{\"domain_key\":\"d\"}";
    CHECK_EQ(Obfuscate(plain), Obfuscate(plain));
}

void TestDeobfuscateRejectsGarbage()
{
    using lootlocker::config_obfuscation::Deobfuscate;

    CHECK(!Deobfuscate("not base64 at all!").Ok());
    CHECK(!Deobfuscate("").Ok());
    CHECK(!Deobfuscate("abc").Ok());
}

void TestPlainJsonConfigStillLoads()
{
    // The verification the task asks for: adding obfuscation must not break the plain path.
    lootlocker::Config config;
    const auto result = lootlocker::config_file::LoadFromString(
        "{\"domain_key\":\"d\",\"game_key\":\"g\",\"game_version\":\"1.0.0\"}", config);
    CHECK(result.Ok());
    CHECK_EQ(config.domain_key, "d");
    CHECK_EQ(config.game_key, "g");
}

void TestObfuscatedConfigLoads()
{
    using lootlocker::config_obfuscation::Obfuscate;

    const std::string plain =
        "{\"domain_key\":\"obf-domain\",\"game_key\":\"obf-game\",\"game_version\":\"2.3.4\"}";
    lootlocker::Config config;
    const auto result = lootlocker::config_file::LoadFromString(Obfuscate(plain), config);

    CHECK(result.Ok());
    CHECK_EQ(config.domain_key, "obf-domain");
    CHECK_EQ(config.game_key, "obf-game");
    CHECK_EQ(config.game_version, "2.3.4");
}

void TestNeitherJsonNorBase64IsReportedClearly()
{
    lootlocker::Config config;
    const auto result = lootlocker::config_file::LoadFromString("this is not a config", config);
    CHECK(!result.Ok());
    CHECK(result.Error().code == lootlocker::ErrorCode::MalformedResponse);
    // The message must name both possibilities, because the adopter does not know which
    // one they were supposed to have.
    CHECK(result.Error().message.find("JSON") != std::string::npos);
    CHECK(result.Error().message.find("Base64") != std::string::npos);
}

void TestBase64ThatIsNotAConfigIsReported()
{
    using lootlocker::config_obfuscation::Base64Encode;

    // Valid Base64, but it decodes to something that is not JSON. This is the case a
    // naive "is it Base64?" check would accept and then fail confusingly on.
    lootlocker::Config config;
    const auto result =
        lootlocker::config_file::LoadFromString(Base64Encode("not json at all"), config);
    CHECK(!result.Ok());
    CHECK(result.Error().code == lootlocker::ErrorCode::MalformedResponse);
}

void TestRawBodyCarriesTheOriginalFileText()
{
    using lootlocker::config_obfuscation::Obfuscate;

    // The contract in config_file.h: RawBody() is what was actually in the file. For an
    // obfuscated file that is the Base64, not the decoded JSON - an adopter can decode
    // Base64 themselves, but cannot recover the original bytes once we replace them.
    const std::string plain = "{\"domain_key\":\"d\",\"game_version\":\"not-semver\"}";
    const std::string obfuscated = Obfuscate(plain);

    lootlocker::Config config;
    const auto result = lootlocker::config_file::LoadFromString(obfuscated, config);

    CHECK(!result.Ok());
    CHECK_EQ(result.RawBody(), obfuscated);
    // And specifically not the decoded JSON.
    CHECK(result.RawBody().find("not-semver") == std::string::npos);
}

void TestRawBodyCarriesThePlainJson()
{
    // The same contract on the plain path, so the two forms agree.
    const std::string plain = "{\"domain_key\":\"d\",\"game_version\":\"not-semver\"}";
    lootlocker::Config config;
    const auto result = lootlocker::config_file::LoadFromString(plain, config);

    CHECK(!result.Ok());
    CHECK_EQ(result.RawBody(), plain);
}

} // namespace

int main()
{
    TestBase64RoundTrip();
    TestBase64RejectsGarbage();
    TestEmptyStringAsymmetry();
    TestObfuscateRoundTrip();
    TestObfuscatedOutputIsNotThePlaintext();
    TestObfuscationIsDeterministic();
    TestDeobfuscateRejectsGarbage();
    TestPlainJsonConfigStillLoads();
    TestObfuscatedConfigLoads();
    TestNeitherJsonNorBase64IsReportedClearly();
    TestBase64ThatIsNotAConfigIsReported();
    TestRawBodyCarriesTheOriginalFileText();
    TestRawBodyCarriesThePlainJson();

    return testing::Report();
}

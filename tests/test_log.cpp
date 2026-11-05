// Logging and log obfuscation (D26, R28).
//
// The obfuscation tests are the important ones: a leak here ships a session token in a
// player's crash report.

#include <lootlocker/config.h>
#include <lootlocker/config_cli.h>
#include <lootlocker/config_file.h>
#include <lootlocker/log.h>

#include "test_harness.h"

#include <string>
#include <vector>

namespace {

// Records everything it is given, so a test can assert on what a real logger would see.
class RecordingLogger : public lootlocker::Logger {
public:
    struct Message {
        lootlocker::LogLevel level;
        std::string          text;
    };

    std::vector<Message>                      messages;
    std::vector<lootlocker::log::HttpEntry>   http_entries;

    void Log(lootlocker::LogLevel level, const std::string& message) override
    {
        messages.push_back(Message{level, message});
    }

    void LogHttp(const lootlocker::log::HttpEntry& entry) override
    {
        http_entries.push_back(entry);
    }
};

void TestLevelNames()
{
    using lootlocker::LogLevel;
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::NoLogging)), "NoLogging");
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::Fatal)), "Fatal");
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::Error)), "Error");
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::Warning)), "Warning");
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::Display)), "Display");
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::Log)), "Log");
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::Verbose)), "Verbose");
    CHECK_EQ(std::string(lootlocker::ToString(LogLevel::VeryVerbose)), "VeryVerbose");

    // Ordering is what makes the level filter work, so it is asserted rather than assumed.
    CHECK(LogLevel::Fatal < LogLevel::Error);
    CHECK(LogLevel::Error < LogLevel::Warning);
    CHECK(LogLevel::Warning < LogLevel::Display);
    CHECK(LogLevel::Display < LogLevel::Log);
    CHECK(LogLevel::Log < LogLevel::Verbose);
    CHECK(LogLevel::Verbose < LogLevel::VeryVerbose);
}

void TestObfuscatesDeniedKeys()
{
    const std::string body =
        R"({"session_token":"abcd1234efgh5678","player_name":"Erik"})";
    const std::string masked = lootlocker::log::ObfuscateJson(body);

    CHECK(masked.find("abcd1234efgh5678") == std::string::npos);
    CHECK(masked.find("abc***678") != std::string::npos);
    CHECK(masked.find("Erik") != std::string::npos);
}

void TestEveryDocumentedKeyIsMasked()
{
    // The spec names these explicitly. A regression in any one of them is a credential
    // leak, so each is asserted rather than a representative sample.
    const char* const keys[] = {"session_token",    "white_label_token", "steam_ticket",
                                "password",         "email",             "refresh_token",
                                "player_identifier"};
    for (std::size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const std::string body =
            std::string(R"({"key":")") + "averylongsecretvalue" + R"(","other":"keepme"})";
        std::string with_key = body;
        // Replace the placeholder key with the real one.
        const std::string needle = R"({"key":")";
        with_key.replace(with_key.find(needle), needle.size(),
                         std::string(R"({")") + keys[i] + R"(":")");

        const std::string masked = lootlocker::log::ObfuscateJson(with_key);
        if (masked.find("averylongsecretvalue") != std::string::npos) {
            CHECK_EQ(std::string("leaked: ") + keys[i], "masked");
        }
        CHECK(masked.find("keepme") != std::string::npos);
    }
}

void TestShortSecretIsFullyMasked()
{
    // Asserting the exact output, not "does the value appear": a split mask also makes
    // the whole value non-contiguous, so a substring check cannot tell fully-masked from
    // split. The boundary is keep_start + keep_end + 4, and 10 is the last fully-masked
    // length.
    struct Case {
        const char* value;
        const char* expected;
    };
    const Case cases[] = {
        {"abcd", "***"},                // 4 chars, would show 2+2
        {"abcdef", "***"},              // 6 chars, would show 3+3
        {"abcdefgh", "***"},            // 8 = 3+3+2, still inside the guard
        {"abcdefghij", "***"},          // 10 = 3+3+4, the last fully-masked length
        {"abcdefghijk", "abc***ijk"},   // 11 = 3+3+5, the first that splits
        {"0123456789abcdef", "012***def"},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const std::string with_value =
            std::string(R"({"session_token":")") + cases[i].value + R"("})";
        const std::string expected =
            std::string(R"({"session_token":")") + cases[i].expected + R"("})";
        CHECK_EQ(lootlocker::log::ObfuscateJson(with_value), expected);
    }
}

void TestMaskKeepsBodyShape()
{
    // The masked body must still be parseable-shaped: quotes balanced, keys intact, and
    // everything that was not a denied value byte-identical.
    const std::string body =
        R"({"z_first":1,"session_token":"secret-value-here","a_bool":true,"n":1.50})";
    const std::string masked = lootlocker::log::ObfuscateJson(body);

    CHECK(masked.find("\"z_first\":1") != std::string::npos);
    CHECK(masked.find("\"a_bool\":true") != std::string::npos);
    // Number formatting survives, which a parse-and-reserialise would not preserve.
    CHECK(masked.find("1.50") != std::string::npos);
    CHECK(masked.find("secret-value-here") == std::string::npos);
}

void TestObfuscationReachesNestedValues()
{
    const std::string body =
        R"({"data":{"auth":{"session_token":"nested-secret-value"}},"ok":true})";
    const std::string masked = lootlocker::log::ObfuscateJson(body);
    CHECK(masked.find("nested-secret-value") == std::string::npos);
    CHECK(masked.find("\"ok\":true") != std::string::npos);
}

void TestObfuscationHandlesSpacing()
{
    // Real responses are pretty-printed as often as not.
    const std::string body = "{\n  \"session_token\" : \"spaced-secret-value\" ,\n  \"x\" : 1\n}";
    const std::string masked = lootlocker::log::ObfuscateJson(body);
    CHECK(masked.find("spaced-secret-value") == std::string::npos);
    CHECK(masked.find("\"x\" : 1") != std::string::npos);
}

void TestKeyMatchingIsCaseInsensitive()
{
    const std::string body = R"({"Session_Token":"case-secret-value"})";
    const std::string masked = lootlocker::log::ObfuscateJson(body);
    CHECK(masked.find("case-secret-value") == std::string::npos);
}

void TestNonJsonBodyIsUnchanged()
{
    // An HTML error page is exactly the artefact a support ticket needs, so it must reach
    // the log intact rather than being guessed at.
    const std::string html = "<html><body>502 Bad Gateway</body></html>";
    CHECK_EQ(lootlocker::log::ObfuscateJson(html), html);

    const std::string empty;
    CHECK_EQ(lootlocker::log::ObfuscateJson(empty), empty);

    const std::string truncated = R"({"session_token":"unterminated)";
    const std::string masked = lootlocker::log::ObfuscateJson(truncated);
    CHECK(masked.find("unterminated") == std::string::npos ||
          masked.find("***") != std::string::npos);
}

void TestAdopterCanAddKeysButNotRemoveOurs()
{
    lootlocker::log::AddDeniedKey("my_custom_secret");
    const std::string body =
        R"({"my_custom_secret":"custom-value-here","session_token":"sdk-value-here"})";
    const std::string masked = lootlocker::log::ObfuscateJson(body);

    CHECK(masked.find("custom-value-here") == std::string::npos);
    // Ours is still masked: there is no API to remove a built-in key.
    CHECK(masked.find("sdk-value-here") == std::string::npos);

    lootlocker::log::AddDeniedKey("my_custom_secret"); // idempotent
    lootlocker::log::ResetDeniedKeys();

    const std::string after = lootlocker::log::ObfuscateJson(body);
    CHECK(after.find("custom-value-here") != std::string::npos);
    CHECK(after.find("sdk-value-here") == std::string::npos);
}

void TestDeniedKeyList()
{
    CHECK(lootlocker::log::IsDeniedKey("session_token"));
    CHECK(lootlocker::log::IsDeniedKey("SESSION_TOKEN"));
    CHECK(!lootlocker::log::IsDeniedKey("player_name"));
    CHECK(lootlocker::log::DeniedKeyCount() >= 6);
}

void TestHttpEntryIsFormatted()
{
    lootlocker::log::HttpEntry entry;
    entry.method = "POST";
    entry.url = "https://x.api.lootlocker.com/game/v2/session/guest";
    entry.status_code = 200;
    entry.duration_ms = 42;
    entry.request_id = "req-1";
    entry.trace_id = "trace-1";

    const std::string line = lootlocker::log::FormatHttpEntry(entry);
    CHECK(line.find("POST") != std::string::npos);
    CHECK(line.find("session/guest") != std::string::npos);
    CHECK(line.find("200") != std::string::npos);
    CHECK(line.find("42") != std::string::npos);
    CHECK(line.find("req-1") != std::string::npos);
    CHECK(line.find("trace-1") != std::string::npos);
}

void TestCustomLoggerReceivesEveryLevel()
{
    RecordingLogger logger;
    const lootlocker::LogLevel levels[] = {
        lootlocker::LogLevel::Fatal,   lootlocker::LogLevel::Error,
        lootlocker::LogLevel::Warning, lootlocker::LogLevel::Display,
        lootlocker::LogLevel::Log,     lootlocker::LogLevel::Verbose,
        lootlocker::LogLevel::VeryVerbose,
    };
    for (std::size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        logger.Log(levels[i], "message");
    }
    CHECK_EQ_INT(static_cast<long long>(logger.messages.size()), 7);
    CHECK(logger.messages[0].level == lootlocker::LogLevel::Fatal);
    CHECK(logger.messages[6].level == lootlocker::LogLevel::VeryVerbose);
}

void TestHttpEntryReachesTheCustomLogger()
{
    RecordingLogger logger;
    lootlocker::log::HttpEntry entry;
    entry.method = "GET";
    entry.url = "https://example.test/x";
    entry.status_code = 404;
    entry.request_id = "the-returned-id";

    logger.LogHttp(entry);

    CHECK_EQ_INT(static_cast<long long>(logger.http_entries.size()), 1);
    // The id the logger sees is the id the call returned, which is the whole point of
    // logging it: a support ticket can be traced back to a specific call.
    CHECK_EQ(logger.http_entries[0].request_id, "the-returned-id");
    CHECK_EQ_INT(logger.http_entries[0].status_code, 404);
}

void TestDefaultLogHttpFallsBackToText()
{
    // A logger that only implements `Log` must still see the exchange, because `LogHttp`
    // is not pure.
    class TextOnly : public lootlocker::Logger {
    public:
        std::vector<lootlocker::LogLevel> levels;
        std::vector<std::string>          texts;
        void Log(lootlocker::LogLevel level, const std::string& message) override
        {
            levels.push_back(level);
            texts.push_back(message);
        }
    };

    TextOnly logger;
    lootlocker::log::HttpEntry entry;
    entry.method = "GET";
    entry.url = "https://example.test/y";
    entry.status_code = 200;
    logger.LogHttp(entry);

    CHECK_EQ_INT(static_cast<long long>(logger.texts.size()), 1);
    CHECK(logger.texts[0].find("example.test") != std::string::npos);
}

void TestConfigCarriesTheSeam()
{
    lootlocker::Config config;
    CHECK(config.logger == nullptr); // default is the SDK's stderr logger
    CHECK(config.log_level == lootlocker::LogLevel::Warning);

    RecordingLogger logger;
    config.logger = &logger;
    config.log_level = lootlocker::LogLevel::VeryVerbose;
    CHECK(config.logger == &logger);
    CHECK(config.log_level == lootlocker::LogLevel::VeryVerbose);

    // NoLogging is how a shipped game silences the SDK without swapping the logger.
    config.log_level = lootlocker::LogLevel::NoLogging;
    CHECK(config.log_level == lootlocker::LogLevel::NoLogging);
}

} // namespace

void TestUnicodeEscapedKeysAreMasked()
{
    // A key written with a \u escape is the same key to every JSON parser, so a
    // backslash-stripping "decoder" would leave the token readable. This is the bypass
    // that made the first implementation wrong.
    const char* const bodies[] = {
        "{\"sess\\u0069on_token\":\"SECRETVALUE123\"}",
        "{\"\\u0070assword\":\"SECRETVALUE123\"}",
        "{\"\\u0073\\u0065\\u0073\\u0073\\u0069\\u006f\\u006e\\u005f\\u0074\\u006f\\u006b\\u0065\\u006e\":\"SECRETVALUE123\"}",
    };
    for (std::size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
        const std::string out = lootlocker::log::ObfuscateJson(bodies[i]);
        if (out.find("SECRETVALUE123") != std::string::npos) {
            CHECK_EQ(std::string("escaped key leaked: ") + bodies[i], "masked");
        }
    }
}

void TestCompositeValueIsReplacedWholesale()
{
    // Asserting the exact output, not "the secret is absent": masking an array with the
    // scalar policy also removes the full strings, so a substring check passes even when
    // bytes from inside the array are retained. The retained bytes are the bug.
    CHECK_EQ(lootlocker::log::ObfuscateJson(
                 "{\"session_token\":[\"SECRETVALUE123\",\"SECONDVALUE456\"]}"),
             "{\"session_token\":\"***\"}");

    CHECK_EQ(lootlocker::log::ObfuscateJson(
                 "{\"session_token\":{\"inner\":\"SECRETVALUE123\"}}"),
             "{\"session_token\":\"***\"}");
}

void TestUnterminatedStringDoesNotUnderflow()
{
    // SkipString returns body.size() for an unterminated string, so subtracting the
    // closing quote underflows size_t and asks for an enormous string - which terminates
    // the process in a no-exceptions build rather than returning the body.
    const char* const bodies[] = {
        "{\"session_token\":\"",
        "{\"session_token\":\"abc",
        "\"",
        "{\"a\":\"b\",\"session_token\":\"",
    };
    for (std::size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
        const std::string out = lootlocker::log::ObfuscateJson(bodies[i]);
        CHECK(!out.empty());
    }
}

void TestSeparatorWhitespaceSurvives()
{
    // The contract is byte-identical apart from the masked value, so the whitespace
    // around the colon must not be rewritten.
    const std::string out = lootlocker::log::ObfuscateJson(
        "{ \"session_token\" : \"SECRETVALUE123\" }");
    CHECK(out.find("{ \"session_token\" : ") != std::string::npos);
    CHECK(out.find("SECRETVALUE123") == std::string::npos);
}

void TestLogExchangeObfuscatesBeforeTheSink()
{
    // The whole point of the dispatch: a custom logger must never see an unmasked body,
    // because it cannot be trusted to obfuscate and a new call site cannot be trusted to
    // remember.
    RecordingLogger logger;
    lootlocker::log::HttpEntry entry;
    entry.method = "POST";
    entry.url = "https://x/y";
    entry.status_code = 200;
    entry.request_body = "{\"password\":\"SECRETVALUE123\"}";
    entry.response_body = "{\"session_token\":\"SECRETVALUE123\"}";

    lootlocker::log::LogExchange(logger, entry);

    CHECK_EQ_INT(static_cast<long long>(logger.http_entries.size()), 1);
    CHECK(logger.http_entries[0].request_body.find("SECRETVALUE123") == std::string::npos);
    CHECK(logger.http_entries[0].response_body.find("SECRETVALUE123") == std::string::npos);
    // The caller's entry is untouched: the dispatch copies.
    CHECK(entry.response_body.find("SECRETVALUE123") != std::string::npos);
}

void TestLogLevelFromConfigFile()
{
    lootlocker::Config config;
    const std::string json = "{\"domain_key\":\"d\",\"game_key\":\"g\",\"game_version\":\"1.0.0\",\"log_level\":\"Verbose\"}";
    const auto result = lootlocker::config_file::LoadFromString(json, config);
    CHECK(result.Ok());
    CHECK(config.log_level == lootlocker::LogLevel::Verbose);

    lootlocker::Config bad;
    const auto bad_result = lootlocker::config_file::LoadFromString(
        "{\"domain_key\":\"d\",\"game_key\":\"g\",\"game_version\":\"1.0.0\",\"log_level\":\"Chatty\"}", bad);
    CHECK(!bad_result.Ok());
}

void TestLogLevelFromCommandLine()
{
    lootlocker::Config config;
    const std::vector<std::string> args = {"--domain-key", "d", "--game-key", "g", "--game-version", "1.0.0",
                                           "--log-level", "verbose"};
    const auto result = lootlocker::config_cli::Apply(args, config);
    CHECK(result.Ok());
    CHECK(config.log_level == lootlocker::LogLevel::Verbose);

    lootlocker::Config bad;
    const std::vector<std::string> bad_args = {"--log-level", "Chatty"};
    CHECK(!lootlocker::config_cli::Apply(bad_args, bad).Ok());
}

int main()
{
    TestLevelNames();
    TestObfuscatesDeniedKeys();
    TestEveryDocumentedKeyIsMasked();
    TestShortSecretIsFullyMasked();
    TestMaskKeepsBodyShape();
    TestObfuscationReachesNestedValues();
    TestObfuscationHandlesSpacing();
    TestKeyMatchingIsCaseInsensitive();
    TestNonJsonBodyIsUnchanged();
    TestAdopterCanAddKeysButNotRemoveOurs();
    TestDeniedKeyList();
    TestHttpEntryIsFormatted();
    TestCustomLoggerReceivesEveryLevel();
    TestHttpEntryReachesTheCustomLogger();
    TestDefaultLogHttpFallsBackToText();
    TestConfigCarriesTheSeam();
    TestUnicodeEscapedKeysAreMasked();
    TestCompositeValueIsReplacedWholesale();
    TestUnterminatedStringDoesNotUnderflow();
    TestSeparatorWhitespaceSurvives();
    TestLogExchangeObfuscatesBeforeTheSink();
    TestLogLevelFromConfigFile();
    TestLogLevelFromCommandLine();

    return testing::Report();
}

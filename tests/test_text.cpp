// The shared string helpers in `core::text`.
//
// These are load-bearing for four translation units now, so they get their own tests
// rather than being covered incidentally through the timezone and logging suites.

#include "test_harness.h"

#include "text.h"

#include <string>

namespace {

void TestLowerAscii()
{
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('A'), 'a');
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('Z'), 'z');
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('a'), 'a');
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('z'), 'z');

    // Non-letters pass through untouched, including the characters that a locale-aware
    // tolower would be entitled to change.
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('0'), '0');
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('/'), '/');
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('_'), '_');
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii('-'), '-');
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii(' '), ' ');

    // Bytes above ASCII are left alone rather than mangled. This is the reason the helper
    // exists: std::tolower takes an int and is locale-dependent, so a signed char above
    // 0x7F is undefined behaviour there.
    CHECK_EQ_INT(lootlocker::core::text::LowerAscii(static_cast<char>(0xC5)),
                 static_cast<char>(0xC5));
}

void TestLower()
{
    CHECK_EQ(lootlocker::core::text::Lower("ABC"), "abc");
    CHECK_EQ(lootlocker::core::text::Lower("abc"), "abc");
    CHECK_EQ(lootlocker::core::text::Lower("W. Europe Standard Time"),
             "w. europe standard time");
    CHECK_EQ(lootlocker::core::text::Lower(""), "");
    CHECK_EQ(lootlocker::core::text::Lower("A1_b-2"), "a1_b-2");
}

void TestEqualsIgnoreCaseAgainstLiteral()
{
    using lootlocker::core::text::EqualsIgnoreCase;

    CHECK(EqualsIgnoreCase("session_token", "session_token"));
    CHECK(EqualsIgnoreCase("SESSION_TOKEN", "session_token"));
    CHECK(EqualsIgnoreCase("Session_Token", "session_token"));
    CHECK(EqualsIgnoreCase("", ""));

    CHECK(!EqualsIgnoreCase("session_token", "session_tokens"));
    CHECK(!EqualsIgnoreCase("session_tokens", "session_token"));
    CHECK(!EqualsIgnoreCase("session_token", ""));
    CHECK(!EqualsIgnoreCase("", "session_token"));
    CHECK(!EqualsIgnoreCase("abc", "abd"));
}

void TestEqualsIgnoreCaseAgainstString()
{
    using lootlocker::core::text::EqualsIgnoreCase;

    CHECK(EqualsIgnoreCase(std::string("Europe/Berlin"), std::string("europe/berlin")));
    CHECK(EqualsIgnoreCase(std::string(""), std::string("")));

    // The length check is what makes this overload safe: without it, a shorter `b` would
    // be read past its end.
    CHECK(!EqualsIgnoreCase(std::string("abc"), std::string("ab")));
    CHECK(!EqualsIgnoreCase(std::string("ab"), std::string("abc")));
    CHECK(!EqualsIgnoreCase(std::string("abc"), std::string("abd")));
}

void TestEqualsIgnoreCaseDoesNotReadPastTheLiteral()
{
    // A prefix must not match. This is the bug the NUL check exists to prevent, and it is
    // worth asserting directly because the failure mode is a read past the end of a
    // string literal rather than a wrong answer.
    CHECK(!lootlocker::core::text::EqualsIgnoreCase("session_token_extra", "session_token"));
}

} // namespace

int main()
{
    TestLowerAscii();
    TestLower();
    TestEqualsIgnoreCaseAgainstLiteral();
    TestEqualsIgnoreCaseAgainstString();
    TestEqualsIgnoreCaseDoesNotReadPastTheLiteral();

    return testing::Report();
}

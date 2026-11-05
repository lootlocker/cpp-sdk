// T9a's verification: the timezone converter.
//
// Three things the task asks for:
//   1. A fixture of known Windows->IANA pairs converts correctly.
//   2. A session start from a Windows name sends the IANA equivalent.
//   3. A session start with no timezone configured still succeeds.
//
// (2) and (3) need a session start, which is T14. What is testable now is the resolution
// order that a session start will use, which is `ResolveTimezone`.

#include "test_harness.h"

#include <lootlocker/config.h>
#include <lootlocker/timezone.h>

#include <string>

namespace {

using lootlocker::kDefaultIanaTimezone;

// A fixture of pairs that matter: the ones a real game is most likely to hit, plus the
// awkward ones (reversed Etc/GMT signs, half-hour offsets, southern hemisphere).
struct Pair {
    const char* windows;
    const char* iana;
};

const Pair kFixture[] = {
    {"W. Europe Standard Time", "Europe/Berlin"},
    {"Romance Standard Time", "Europe/Paris"},
    {"GMT Standard Time", "Europe/London"},
    {"Eastern Standard Time", "America/New_York"},
    {"Central Standard Time", "America/Chicago"},
    {"Mountain Standard Time", "America/Denver"},
    {"Pacific Standard Time", "America/Los_Angeles"},
    {"Tokyo Standard Time", "Asia/Tokyo"},
    {"China Standard Time", "Asia/Shanghai"},
    {"India Standard Time", "Asia/Calcutta"},
    {"AUS Eastern Standard Time", "Australia/Sydney"},
    {"E. Australia Standard Time", "Australia/Brisbane"},
    {"New Zealand Standard Time", "Pacific/Auckland"},
    {"South Africa Standard Time", "Africa/Johannesburg"},
    {"Russian Standard Time", "Europe/Moscow"},
    {"Hawaiian Standard Time", "Pacific/Honolulu"},
    {"Alaskan Standard Time", "America/Anchorage"},
    {"UTC", "Etc/GMT"},
    {"Dateline Standard Time", "Etc/GMT+12"},
};

void TestFixtureConverts()
{
    for (const Pair& p : kFixture) {
        std::string iana;
        CHECK(lootlocker::TryConvertWindowsToIana(p.windows, iana));
        CHECK_EQ(iana, p.iana);
    }
}

void TestConversionIsCaseInsensitive()
{
    std::string iana;
    CHECK(lootlocker::TryConvertWindowsToIana("w. europe standard time", iana));
    CHECK_EQ(iana, "Europe/Berlin");

    CHECK(lootlocker::TryConvertWindowsToIana("W. EUROPE STANDARD TIME", iana));
    CHECK_EQ(iana, "Europe/Berlin");
}

void TestUnknownWindowsNameFails()
{
    std::string iana = "untouched";
    CHECK(!lootlocker::TryConvertWindowsToIana("Not A Timezone", iana));
    // The caller decides what to do; the function does not silently hand back UTC.
    CHECK_EQ(iana, "untouched");
}

void TestEmptyInputFails()
{
    std::string iana = "untouched";
    CHECK(!lootlocker::TryConvertWindowsToIana("", iana));
    CHECK_EQ(iana, "untouched");
}

void TestReverseConversion()
{
    std::string windows;
    CHECK(lootlocker::TryConvertIanaToWindows("Europe/Berlin", windows));
    CHECK_EQ(windows, "W. Europe Standard Time");

    CHECK(lootlocker::TryConvertIanaToWindows("America/New_York", windows));
    CHECK_EQ(windows, "Eastern Standard Time");
}

void TestKnownChecks()
{
    CHECK(lootlocker::IsKnownIanaTimezone("Europe/Berlin"));
    CHECK(lootlocker::IsKnownWindowsTimezone("W. Europe Standard Time"));
    CHECK(!lootlocker::IsKnownIanaTimezone("Not/AZone"));
    CHECK(!lootlocker::IsKnownWindowsTimezone("Not A Zone"));
}

void TestUtcOffsetUsesReversedIanaSign()
{
    // Etc/GMT+5 is UTC-5. This is an IANA convention and the single easiest thing to get
    // wrong, so it is asserted explicitly.
    CHECK_EQ(lootlocker::UtcOffsetToIana(-5), "Etc/GMT+5");
    CHECK_EQ(lootlocker::UtcOffsetToIana(2), "Etc/GMT-2");
    CHECK_EQ(lootlocker::UtcOffsetToIana(0), "Etc/UTC");
}

void TestUtcOffsetClamps()
{
    CHECK_EQ(lootlocker::UtcOffsetToIana(-999), "Etc/GMT+12");
    CHECK_EQ(lootlocker::UtcOffsetToIana(999), "Etc/GMT-14");
}

void TestNormalizeAcceptsAllForms()
{
    std::string out;

    // A numeric offset.
    CHECK(lootlocker::TryNormalizeToIana("2", out));
    CHECK_EQ(out, "Etc/GMT-2");

    // An IANA name, passed through with its casing preserved.
    CHECK(lootlocker::TryNormalizeToIana("Asia/Tokyo", out));
    CHECK_EQ(out, "Asia/Tokyo");

    // A Windows name, converted.
    CHECK(lootlocker::TryNormalizeToIana("Romance Standard Time", out));
    CHECK_EQ(out, "Europe/Paris");
}

void TestNormalizeFallsBackToUtc()
{
    std::string out;

    // Unknown input: false, but a valid IANA name rather than an empty string, so a caller
    // that ignores the return value still sends something the backend accepts.
    CHECK(!lootlocker::TryNormalizeToIana("NotATimezone", out));
    CHECK_EQ(out, kDefaultIanaTimezone);

    CHECK(!lootlocker::TryNormalizeToIana("", out));
    CHECK_EQ(out, kDefaultIanaTimezone);
}

void TestDefaultIsCorrectlyCased()
{
    // The backend rejects "ETC/GMT" and similar with an "invalid JSON input" error, which
    // is a confusing failure for a casing mistake.
    CHECK_EQ(std::string(kDefaultIanaTimezone), "Etc/UTC");
}

// A provider that returns whatever it is told, so the resolution order is testable without
// depending on the machine's actual timezone.
class FixedProvider : public lootlocker::TimezoneProvider {
public:
    explicit FixedProvider(std::string value) : m_value(std::move(value)) {}
    std::string GetTimezone() override { return m_value; }

private:
    std::string m_value;
};

void TestOverrideWinsOverProvider()
{
    FixedProvider provider("Asia/Tokyo");
    // An adopter who set Config::timezone explicitly meant it.
    CHECK_EQ(lootlocker::ResolveTimezone(&provider, "Europe/Berlin"), "Europe/Berlin");
}

void TestProviderWinsOverOs()
{
    FixedProvider provider("Asia/Tokyo");
    CHECK_EQ(lootlocker::ResolveTimezone(&provider, ""), "Asia/Tokyo");
}

void TestProviderAcceptsAWindowsName()
{
    // A provider is allowed to hand back whatever the engine has, including a Windows name.
    FixedProvider provider("Romance Standard Time");
    CHECK_EQ(lootlocker::ResolveTimezone(&provider, ""), "Europe/Paris");
}

void TestProviderAcceptsANumericOffset()
{
    FixedProvider provider("-5");
    CHECK_EQ(lootlocker::ResolveTimezone(&provider, ""), "Etc/GMT+5");
}

void TestEmptyProviderFallsThrough()
{
    // A provider with nothing to offer must not produce an empty timezone.
    FixedProvider provider("");
    const std::string resolved = lootlocker::ResolveTimezone(&provider, "");
    CHECK(!resolved.empty());
    // Whatever the OS says, or the default. Both are valid IANA names.
    CHECK(lootlocker::IsKnownIanaTimezone(resolved) || resolved == kDefaultIanaTimezone);
}

void TestNullProviderStillResolves()
{
    // No provider, no override: the OS, then the default. Never empty.
    const std::string resolved = lootlocker::ResolveTimezone(nullptr, "");
    CHECK(!resolved.empty());
}

void TestOverrideIsNormalized()
{
    // A Windows name in Config::timezone is converted rather than sent raw.
    CHECK_EQ(lootlocker::ResolveTimezone(nullptr, "W. Europe Standard Time"), "Europe/Berlin");
}

void TestSystemProviderReturnsSomethingValid()
{
    // The OS read may fail on an unusual machine, but it must never return a name the
    // backend would reject: either empty (caller falls back) or a known IANA name.
    lootlocker::SystemTimezoneProvider provider;
    const std::string tz = provider.GetTimezone();
    if (!tz.empty()) {
        CHECK(lootlocker::IsKnownIanaTimezone(tz));
    }
}

} // namespace

int main()
{
    TestFixtureConverts();
    TestConversionIsCaseInsensitive();
    TestUnknownWindowsNameFails();
    TestEmptyInputFails();
    TestReverseConversion();
    TestKnownChecks();
    TestUtcOffsetUsesReversedIanaSign();
    TestUtcOffsetClamps();
    TestNormalizeAcceptsAllForms();
    TestNormalizeFallsBackToUtc();
    TestDefaultIsCorrectlyCased();
    TestOverrideWinsOverProvider();
    TestProviderWinsOverOs();
    TestProviderAcceptsAWindowsName();
    TestProviderAcceptsANumericOffset();
    TestEmptyProviderFallsThrough();
    TestNullProviderStillResolves();
    TestOverrideIsNormalized();
    TestSystemProviderReturnsSomethingValid();

    return testing::Report();
}

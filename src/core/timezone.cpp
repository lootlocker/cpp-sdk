#include <lootlocker/timezone.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#  include <unistd.h>
#endif

namespace lootlocker {

const char* const kDefaultIanaTimezone = "Etc/UTC";

namespace {

struct TimezonePair {
    const char* windows;
    const char* iana;
};

// Generated from data/windows-to-iana.tsv by tools/generate_timezone_table.py.
const TimezonePair kWindowsToIana[] = {
#include "timezone_table.inc"
};

constexpr std::size_t kPairCount = sizeof(kWindowsToIana) / sizeof(kWindowsToIana[0]);

// ASCII-only case folding. Timezone names are ASCII, and std::tolower is locale-dependent
// and takes an int, which is a footgun with signed chars.
char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(const std::string& a, const char* b)
{
    std::size_t i = 0;
    for (; i < a.size(); ++i) {
        if (b[i] == '\0') {
            return false;
        }
        if (LowerAscii(a[i]) != LowerAscii(b[i])) {
            return false;
        }
    }
    return b[i] == '\0';
}

} // namespace

bool TryConvertWindowsToIana(const std::string& windows_name, std::string& out)
{
    if (windows_name.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < kPairCount; ++i) {
        if (EqualsIgnoreCase(windows_name, kWindowsToIana[i].windows)) {
            out = kWindowsToIana[i].iana;
            return true;
        }
    }
    return false;
}

bool TryConvertIanaToWindows(const std::string& iana_name, std::string& out)
{
    if (iana_name.empty()) {
        return false;
    }
    // Many-to-one: several IANA names share a Windows name, so the first match wins. The
    // table is ordered so the canonical IANA name for each Windows zone comes first.
    for (std::size_t i = 0; i < kPairCount; ++i) {
        if (EqualsIgnoreCase(iana_name, kWindowsToIana[i].iana)) {
            out = kWindowsToIana[i].windows;
            return true;
        }
    }
    return false;
}

bool IsKnownIanaTimezone(const std::string& name)
{
    // Deliberately not "is it in the table". The table is a Windows mapping, not the IANA
    // database, so it holds one representative per Windows zone and misses real names like
    // America/Argentina/Buenos_Aires. Validating against it would reject a correct system
    // timezone and silently fall through to UTC.
    //
    // The shape check is what the API actually needs: a well-formed Area/Location path.
    // The server is the authority on whether the zone exists.
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (name[0] == '/' || name[name.size() - 1] == '/') {
        return false;
    }
    bool has_slash = false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '/') {
            has_slash = true;
            continue;
        }
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+' ||
                        c == '.';
        if (!ok) {
            return false;
        }
    }
    return has_slash;
}

bool IsKnownWindowsTimezone(const std::string& name)
{
    std::string ignored;
    return TryConvertWindowsToIana(name, ignored);
}

std::string UtcOffsetToIana(int utc_offset_hours)
{
    // Clamp to the range IANA defines for Etc/GMT zones.
    if (utc_offset_hours < -12) {
        utc_offset_hours = -12;
    }
    if (utc_offset_hours > 14) {
        utc_offset_hours = 14;
    }

    if (utc_offset_hours == 0) {
        return kDefaultIanaTimezone;
    }

    // Reversed sign: Etc/GMT+5 is UTC-5. This is an IANA convention, not a bug.
    const int magnitude = utc_offset_hours < 0 ? -utc_offset_hours : utc_offset_hours;
    const char* sign = utc_offset_hours < 0 ? "+" : "-";

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "Etc/GMT%s%d", sign, magnitude);
    return std::string(buffer);
}

bool TryNormalizeToIana(const std::string& timezone, std::string& out)
{
    if (timezone.empty()) {
        out = kDefaultIanaTimezone;
        return false;
    }

    // A numeric UTC offset, e.g. "2" or "-5".
    {
        const char* begin = timezone.c_str();
        char* end = nullptr;
        const long parsed = std::strtol(begin, &end, 10);
        if (end != begin && *end == '\0') {
            // Clamp while still a `long`: casting first would make an out-of-range value
            // implementation-defined, and it could wrap to a small or negative offset.
            long hours = parsed;
            if (hours < -12) {
                hours = -12;
            }
            if (hours > 14) {
                hours = 14;
            }
            out = UtcOffsetToIana(static_cast<int>(hours));
            return true;
        }
    }

    // Already IANA: pass it through unchanged, preserving the caller's casing.
    if (IsKnownIanaTimezone(timezone)) {
        out = timezone;
        return true;
    }

    // A Windows name: convert.
    if (TryConvertWindowsToIana(timezone, out)) {
        return true;
    }

    out = kDefaultIanaTimezone;
    return false;
}

std::string SystemTimezoneProvider::GetTimezone()
{
#if defined(_WIN32)
    // GetTimeZoneInformation returns a Windows name, which the table converts.
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    std::memset(&tz, 0, sizeof(tz));
    const DWORD result = GetDynamicTimeZoneInformation(&tz);
    if (result == TIME_ZONE_ID_INVALID) {
        return std::string();
    }

    // Prefer the dynamic key (the IANA-ish name on modern Windows), then the standard name.
    std::string iana;
    if (tz.TimeZoneKeyName[0] != L'\0') {
        char narrow[128];
        const int written = WideCharToMultiByte(CP_UTF8, 0, tz.TimeZoneKeyName, -1, narrow,
                                                sizeof(narrow), nullptr, nullptr);
        if (written > 0 && TryConvertWindowsToIana(narrow, iana)) {
            return iana;
        }
    }
    if (tz.StandardName[0] != L'\0') {
        char narrow[128];
        const int written = WideCharToMultiByte(CP_UTF8, 0, tz.StandardName, -1, narrow,
                                                sizeof(narrow), nullptr, nullptr);
        if (written > 0 && TryConvertWindowsToIana(narrow, iana)) {
            return iana;
        }
    }
    return std::string();
#elif defined(__unix__) || defined(__APPLE__)
    // POSIX: TZ first, then /etc/localtime, then give up and let the caller fall back.
    const char* tz_env = std::getenv("TZ");
    if (tz_env != nullptr && tz_env[0] != '\0') {
        std::string iana;
        if (TryNormalizeToIana(tz_env, iana)) {
            return iana;
        }
    }

    // /etc/localtime is usually a symlink into /usr/share/zoneinfo/<Area>/<City>.
    char link[512];
    const ssize_t len = readlink("/etc/localtime", link, sizeof(link) - 1);
    if (len > 0) {
        link[len] = '\0';
        const char* marker = "/zoneinfo/";
        const char* found = std::strstr(link, marker);
        if (found != nullptr) {
            const std::string candidate = found + std::strlen(marker);
            if (IsKnownIanaTimezone(candidate)) {
                return candidate;
            }
        }
    }

    return std::string();
#else
    // A platform we do not cover. The contract says UTC, so return nothing and let
    // ResolveTimezone fall through to kDefaultIanaTimezone.
    return std::string();
#endif
}

std::string ResolveTimezone(TimezoneProvider* provider, const std::string& override_value,
                            bool send)
{
    // An adopter who said "send nothing" meant it, and it outranks everything else.
    if (!send) {
        return std::string();
    }

    // An explicit override wins outright: an adopter who set it meant it.
    if (!override_value.empty()) {
        std::string iana;
        TryNormalizeToIana(override_value, iana);
        return iana;
    }

    // Then the injected provider, which knows more than the OS does.
    if (provider != nullptr) {
        const std::string from_provider = provider->GetTimezone();
        if (!from_provider.empty()) {
            std::string iana;
            if (TryNormalizeToIana(from_provider, iana)) {
                return iana;
            }
        }
    }

    // Then the OS.
    SystemTimezoneProvider system;
    const std::string from_system = system.GetTimezone();
    if (!from_system.empty()) {
        return from_system;
    }

    // Finally the documented default.
    return kDefaultIanaTimezone;
}

} // namespace lootlocker

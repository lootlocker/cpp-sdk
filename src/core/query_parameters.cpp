#include "query_parameters.h"

#include <clocale>
#include <cmath>
#include <cstdio>

namespace lootlocker {
namespace core {

namespace {

// RFC 3986 unreserved characters are passed through; everything else is percent-encoded.
//
// Deliberately conservative: a space becomes `%20` rather than `+`. `+` is only a space in
// `application/x-www-form-urlencoded`, and a query string is not that, so encoding a space
// as `+` would be wrong for a value that legitimately contains a plus sign.
bool IsUnreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string PercentEncode(const std::string& value)
{
    static const char* kHex = "0123456789ABCDEF";

    std::string out;
    out.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (IsUnreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// A double, formatted so it round-trips and never picks up a locale's decimal comma.
//
// `snprintf`'s `%g` is governed by the process-wide C locale, so in a locale with a decimal
// comma `1.5` would become `1,5` and then `%2C` on the wire - a different number. Rather
// than calling `setlocale` (which would change global process state and is not ours to
// touch), the digits are produced with `%g` and then any locale-specific separator is
// normalised back to '.'.
//
// `%g` with 17 significant digits is the shortest form that round-trips an IEEE double.
// `std::to_string` is not used because it always emits six decimals, which turns `1` into
// `1.000000` and a limit of `1.5` into `1.500000` - both wrong on the wire.
std::string FormatDouble(double value)
{
    if (!std::isfinite(value)) {
        // A NaN or an infinity cannot be expressed in a query string. Sending "nan" would
        // be a request the server rejects with a confusing message; sending nothing is
        // worse. An empty value is the honest answer and the server's validation reports
        // it.
        return std::string();
    }

    // The locale's decimal separator, whatever it is. `localeconv` is the only portable way
    // to ask, and it is read-only.
    const char* decimal_point = std::localeconv()->decimal_point;
    const std::string separator =
        (decimal_point == nullptr || *decimal_point == '\0') ? std::string(".") : std::string(decimal_point);

    // `%.17g` is exact but noisy: 0.1 prints as 0.10000000000000001. Trim to the shortest
    // representation that still round-trips, by trying progressively shorter precisions.
    for (int precision = 1; precision <= 17; ++precision) {
        char candidate[64];
        std::snprintf(candidate, sizeof(candidate), "%.*g", precision, value);

        std::string text(candidate);

        // Normalise the separator to '.', which is what JSON and query strings require.
        if (separator != ".") {
            const std::size_t at = text.find(separator);
            if (at != std::string::npos) {
                text.replace(at, separator.size(), ".");
            }
        }

        double reparsed = 0.0;
        if (std::sscanf(text.c_str(), "%lf", &reparsed) == 1 && reparsed == value) {
            return text;
        }
    }

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    std::string text(buffer);
    if (separator != ".") {
        const std::size_t at = text.find(separator);
        if (at != std::string::npos) {
            text.replace(at, separator.size(), ".");
        }
    }
    return text;
}

} // namespace

void QueryParameters::Add(const std::string& name, const std::string& value)
{
    Entry entry;
    entry.name = name;
    entry.value = value;
    m_entries.push_back(entry);
}

void QueryParameters::Add(const std::string& name, const char* value)
{
    Add(name, std::string(value == nullptr ? "" : value));
}

void QueryParameters::Add(const std::string& name, int value)
{
    Add(name, static_cast<std::int64_t>(value));
}

void QueryParameters::Add(const std::string& name, std::int64_t value)
{
    // `%lld` rather than `std::to_string`, which is locale-independent for integers but
    // allocates twice. This is not a hot path, but the cast is needed either way.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    Add(name, std::string(buffer));
}

void QueryParameters::Add(const std::string& name, std::uint64_t value)
{
    // `%llu`, not a cast to `int64_t`: a value above 2^63 would wrap negative.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    Add(name, std::string(buffer));
}

void QueryParameters::Add(const std::string& name, double value)
{
    Add(name, FormatDouble(value));
}

void QueryParameters::Add(const std::string& name, bool value)
{
    // The API expects `true`/`false`, not `1`/`0`. Both are accepted by most endpoints, but
    // the documented form is the one to send.
    Add(name, std::string(value ? "true" : "false"));
}

std::string QueryParameters::ToQueryString() const
{
    std::string out;
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (i != 0) {
            out.push_back('&');
        }
        out += PercentEncode(m_entries[i].name);
        out.push_back('=');
        out += PercentEncode(m_entries[i].value);
    }
    return out;
}

} // namespace core
} // namespace lootlocker

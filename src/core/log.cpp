// Logging (D26) and the log-side obfuscation (R28).
//
// The obfuscator is a scanner rather than a parse-and-reserialise: it walks the body
// looking for `"key"` followed by `:`, and if the key is denied it splices a mask over the
// value. That keeps everything else byte-identical, which is what makes a log comparable
// against a report and what makes a secret impossible to miss because a key we do not
// model was silently dropped by a round-trip.

#include <lootlocker/log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lootlocker {

const char* ToString(LogLevel level)
{
    switch (level) {
        case LogLevel::NoLogging:   return "NoLogging";
        case LogLevel::Fatal:       return "Fatal";
        case LogLevel::Error:       return "Error";
        case LogLevel::Warning:     return "Warning";
        case LogLevel::Display:     return "Display";
        case LogLevel::Log:         return "Log";
        case LogLevel::Verbose:     return "Verbose";
        case LogLevel::VeryVerbose: return "VeryVerbose";
    }
    return "Unknown";
}

namespace log {

namespace {

// The SDK's own denied keys. An adopter adds to this; nothing removes from it.
const char* const kDeniedKeys[] = {
    "session_token",    "white_label_token", "steam_ticket", "password",
    "email",            "refresh_token",     "player_identifier",
};

std::vector<std::string>& DenyList()
{
    static std::vector<std::string> list(kDeniedKeys,
                                         kDeniedKeys + sizeof(kDeniedKeys) / sizeof(kDeniedKeys[0]));
    return list;
}

char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lower(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out.push_back(LowerAscii(s[i]));
    }
    return out;
}

void SkipWhitespace(const std::string& s, std::size_t& i)
{
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
        } else {
            break;
        }
    }
}

// Find the end of a JSON string starting at `i` (which must be the opening quote).
// Returns the index just past the closing quote, or s.size() if unterminated.
std::size_t SkipString(const std::string& s, std::size_t i)
{
    ++i; // opening quote
    while (i < s.size()) {
        if (s[i] == '\\') {
            i += 2;
            continue;
        }
        if (s[i] == '"') {
            return i + 1;
        }
        ++i;
    }
    return s.size();
}

// Unescape a JSON string value for comparison against the deny-list. Only the escapes that
// can appear in a key name matter; anything else is left as-is.
std::string UnescapeKey(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            ++i;
            out.push_back(raw[i]);
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

// Find the end of a JSON value starting at `i`, skipping nested structures and strings.
// Returns the index just past the value.
std::size_t SkipValue(const std::string& s, std::size_t i)
{
    int depth = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            i = SkipString(s, i);
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
            ++i;
            continue;
        }
        if (c == '}' || c == ']') {
            if (depth == 0) {
                return i;
            }
            --depth;
            ++i;
            continue;
        }
        if (c == ',' && depth == 0) {
            return i;
        }
        ++i;
    }
    return s.size();
}

std::string Mask(const std::string& value, std::size_t keep_start, std::size_t keep_end)
{
    std::size_t begin = 0;
    std::size_t end = value.size();

    // A quoted value keeps its quotes outside the mask, so the body stays valid JSON.
    if (end >= 2 && value[0] == '"' && value[end - 1] == '"') {
        begin = 1;
        end -= 1;
    }

    const std::size_t length = end - begin;

    // A short value is masked entirely rather than mostly revealed. The +4 is the guard:
    // without it, 3+3 visible characters of an 8-character secret leaves 2 hidden.
    if (length <= keep_start + keep_end + 4) {
        return std::string(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(begin)) +
               "***" +
               std::string(value.begin() + static_cast<std::ptrdiff_t>(end), value.end());
    }

    return std::string(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(begin + keep_start)) +
           "***" +
           std::string(value.begin() + static_cast<std::ptrdiff_t>(end - keep_end), value.end());
}

} // namespace

const char* const* DeniedKeys()
{
    return kDeniedKeys;
}

std::size_t DeniedKeyCount()
{
    return sizeof(kDeniedKeys) / sizeof(kDeniedKeys[0]);
}

bool IsDeniedKey(const std::string& key)
{
    const std::string lowered = Lower(key);
    const std::vector<std::string>& list = DenyList();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i] == lowered) {
            return true;
        }
    }
    return false;
}

void AddDeniedKey(const std::string& key)
{
    if (key.empty()) {
        return;
    }
    const std::string lowered = Lower(key);
    std::vector<std::string>& list = DenyList();
    if (std::find(list.begin(), list.end(), lowered) == list.end()) {
        list.push_back(lowered);
    }
}

void ResetDeniedKeys()
{
    std::vector<std::string>& list = DenyList();
    list.assign(kDeniedKeys, kDeniedKeys + sizeof(kDeniedKeys) / sizeof(kDeniedKeys[0]));
}

std::string ObfuscateJson(const std::string& body, std::size_t visible_from_start,
                          std::size_t visible_from_end)
{
    if (body.empty() || DenyList().empty()) {
        return body;
    }

    // A body with no quote cannot contain a key, and a body with no brace is not an object.
    // Both are cheap rejections that keep a non-JSON body untouched.
    if (body.find('"') == std::string::npos) {
        return body;
    }

    std::string out;
    out.reserve(body.size());

    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] != '"') {
            out.push_back(body[i]);
            ++i;
            continue;
        }

        // A string. It is a key only if the next non-space character is ':'.
        const std::size_t string_end = SkipString(body, i);
        const std::string raw(body, i + 1, string_end - i - (string_end > i ? 2 : 1));

        std::size_t after = string_end;
        SkipWhitespace(body, after);

        if (after < body.size() && body[after] == ':' && IsDeniedKey(UnescapeKey(raw))) {
            out.append(body, i, string_end - i); // the key, verbatim
            out.push_back(':');
            ++after;
            SkipWhitespace(body, after);

            const std::size_t value_end = SkipValue(body, after);
            const std::string value(body, after, value_end - after);
            out.append(Mask(value, visible_from_start, visible_from_end));
            i = value_end;
            continue;
        }

        out.append(body, i, string_end - i);
        i = string_end;
    }

    return out;
}

std::string FormatHttpEntry(const HttpEntry& entry)
{
    char prefix[128];
    std::snprintf(prefix, sizeof(prefix), "%s %s -> %d in %lld ms (attempt %d)",
                  entry.method.empty() ? "?" : entry.method.c_str(),
                  entry.url.empty() ? "?" : entry.url.c_str(), entry.status_code,
                  static_cast<long long>(entry.duration_ms), entry.attempt);

    std::string line(prefix);

    if (!entry.request_id.empty()) {
        line += " request_id=" + entry.request_id;
    }
    if (!entry.trace_id.empty()) {
        line += " trace_id=" + entry.trace_id;
    }
    if (!entry.request_body.empty()) {
        line += "\n  request: " + entry.request_body;
    }
    if (!entry.response_body.empty()) {
        line += "\n  response: " + entry.response_body;
    }
    return line;
}

} // namespace log

void Logger::LogHttp(const log::HttpEntry& entry)
{
    // The default renders a summary at Display: an exchange is diagnostic rather than a
    // warning, and emitting it at Warning would make a default-configured SDK noisy.
    Log(LogLevel::Display, log::FormatHttpEntry(entry));
}

namespace log {

StderrLogger::StderrLogger(LogLevel minimum) : m_minimum(minimum) {}

void StderrLogger::Log(LogLevel level, const std::string& message)
{
    if (level == LogLevel::NoLogging || level > m_minimum) {
        return;
    }
    std::fprintf(stderr, "[LootLocker][%s] %s\n", ToString(level), message.c_str());
}

} // namespace log

} // namespace lootlocker

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
// Decode a JSON string body into the characters it represents.
//
// This has to be a real decoder, not a backslash-stripper. A key written as
// "sess\u0069on_token" is the same key as "session_token" to every JSON parser, so a
// stripper would leave it unmatched and the token readable - a deny-list bypass that
// costs an attacker one escape sequence.
std::string DecodeJsonString(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            out.push_back(raw[i]);
            continue;
        }

        const char esc = raw[++i];
        switch (esc) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/') ; break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                // \uXXXX. Only the BMP subset that can appear in a field name matters
                // here; a surrogate pair cannot spell an ASCII key, so it is passed
                // through rather than decoded.
                if (i + 4 >= raw.size()) {
                    out.push_back(esc);
                    break;
                }
                unsigned code = 0;
                bool valid = true;
                for (int k = 1; k <= 4; ++k) {
                    const char h = raw[i + static_cast<std::size_t>(k)];
                    unsigned digit = 0;
                    if (h >= '0' && h <= '9') {
                        digit = static_cast<unsigned>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        digit = static_cast<unsigned>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        digit = static_cast<unsigned>(h - 'A' + 10);
                    } else {
                        valid = false;
                        break;
                    }
                    code = code * 16 + digit;
                }
                if (!valid) {
                    out.push_back(esc);
                    break;
                }
                i += 4;
                // ASCII is what a deny-list key can be. Anything above it is emitted as
                // a placeholder rather than encoded, because it cannot match a key.
                out.push_back(code < 0x80 ? static_cast<char>(code) : '?') ;
                break;
            }
            default:
                out.push_back(esc);
                break;
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

    // Cheap rejection: a body with no quote cannot contain a key.
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

        // An unterminated string: SkipString returns body.size(), so the closing quote is
        // not there to subtract. Copying the remainder verbatim is the only safe move -
        // the arithmetic that assumed a closing quote underflows size_t and asks for an
        // enormous string, which terminates the process in a no-exceptions build.
        if (string_end >= body.size() || body[string_end - 1] != '"') {
            out.append(body, i, std::string::npos);
            break;
        }

        const std::string raw(body, i + 1, string_end - i - 2);

        std::size_t after = string_end;
        SkipWhitespace(body, after);

        if (after < body.size() && body[after] == ':' && IsDeniedKey(DecodeJsonString(raw))) {
            // Copy the key and the separator region verbatim, so whitespace around the
            // colon survives and the body stays byte-comparable with the original.
            out.append(body, i, after - i + 1);
            ++after;
            const std::size_t value_start = after;
            SkipWhitespace(body, after);
            out.append(body, value_start, after - value_start);

            const std::size_t value_end = SkipValue(body, after);
            const std::string value(body, after, value_end - after);

            // A composite value is replaced wholesale. Applying the visible-prefix policy
            // to an array or object would keep bytes from inside it, and those bytes can
            // be other secrets - an array of tokens would leak its first and last.
            if (!value.empty() && (value[0] == '[' || value[0] == '{')) {
                out.append("\"***\"");
            } else {
                out.append(Mask(value, visible_from_start, visible_from_end));
            }
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

namespace log {

// The SDK's single entry point for logging an exchange.
//
// Obfuscation happens HERE - not in the sink, not at the call site - so a custom Logger
// cannot receive an unmasked body and a new log statement cannot forget. The virtual
// LogHttp is the sink; this is the dispatch in front of it.
void LogExchange(Logger& logger, const HttpEntry& entry)
{
    HttpEntry safe = entry;
    safe.request_body = ObfuscateJson(entry.request_body);
    safe.response_body = ObfuscateJson(entry.response_body);
    logger.LogHttp(safe);
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


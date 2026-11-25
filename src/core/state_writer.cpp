#include <lootlocker/state_writer.h>

#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace lootlocker {

// ---------------------------------------------------------------------------
// NullStateWriter
// ---------------------------------------------------------------------------

bool NullStateWriter::GetString(const std::string&, std::string&) { return false; }
void NullStateWriter::SetString(const std::string&, const std::string&) {}

bool NullStateWriter::GetInt64(const std::string&, std::int64_t&) { return false; }
void NullStateWriter::SetInt64(const std::string&, std::int64_t) {}

bool NullStateWriter::GetUInt64(const std::string&, std::uint64_t&) { return false; }
void NullStateWriter::SetUInt64(const std::string&, std::uint64_t) {}

bool NullStateWriter::GetDouble(const std::string&, double&) { return false; }
void NullStateWriter::SetDouble(const std::string&, double) {}

bool NullStateWriter::GetBool(const std::string&, bool&) { return false; }
void NullStateWriter::SetBool(const std::string&, bool) {}

void NullStateWriter::Erase(const std::string&) {}
bool NullStateWriter::Has(const std::string&) { return false; }

// ---------------------------------------------------------------------------
// FileStateWriter
// ---------------------------------------------------------------------------

namespace {

// The type tag on each line. A key written as one type is not readable as another, which is
// what stops a corrupt or hand-edited file from silently becoming a wrong player id.
enum class ValueType { String, Int64, UInt64, Double, Bool };

const char* TypeTag(ValueType type)
{
    switch (type) {
    case ValueType::String: return "s";
    case ValueType::Int64:  return "i";
    case ValueType::UInt64: return "u";
    case ValueType::Double: return "d";
    case ValueType::Bool:   return "b";
    }
    return "s";
}

bool ParseTypeTag(const std::string& tag, ValueType& out)
{
    if (tag == "s") { out = ValueType::String; return true; }
    if (tag == "i") { out = ValueType::Int64;  return true; }
    if (tag == "u") { out = ValueType::UInt64; return true; }
    if (tag == "d") { out = ValueType::Double; return true; }
    if (tag == "b") { out = ValueType::Bool;   return true; }
    return false;
}

struct Entry {
    ValueType   type = ValueType::String;
    std::string text;
};

// A key may not contain the separators, because the format is line-based and unescaped.
// Rejecting is better than escaping: an escaped format is a parser, and this is deliberately
// not one.
bool IsValidKey(const std::string& key)
{
    if (key.empty()) {
        return false;
    }
    return key.find('=') == std::string::npos && key.find('\n') == std::string::npos &&
           key.find('\r') == std::string::npos;
}

// A value may not contain a newline, for the same reason. A session token is base64-ish and
// a player name could contain anything, so a newline is replaced rather than rejected - the
// alternative is losing the whole write over a display name.
std::string SanitizeValue(const std::string& value)
{
    std::string out = value;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = ' ';
        }
    }
    return out;
}

std::string FormatDouble(double value)
{
    // `%.17g` round-trips an IEEE double exactly. A shorter form would lose precision on a
    // value the SDK wrote itself, which is the one thing a store must not do.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);

    // `%g` is governed by the process-wide `LC_NUMERIC` locale, so in a comma-decimal locale
    // this writes `0,1` - and `GetDouble` uses `strtod`, which under the same locale would
    // read it back, but under a *different* locale (the game changed it, or the file was
    // written on another machine) would not. Normalising to '.' makes the file
    // locale-independent, which is what a persisted format has to be.
    std::string text(buffer);
    const char* decimal_point = std::localeconv()->decimal_point;
    if (decimal_point != nullptr && *decimal_point != '\0' && std::string(decimal_point) != ".") {
        const std::size_t at = text.find(decimal_point);
        if (at != std::string::npos) {
            text.replace(at, std::string(decimal_point).size(), ".");
        }
    }
    return text;
}

} // namespace

struct FileStateWriter::Impl {
    std::string                path;
    std::map<std::string, Entry> entries;
    bool                       last_write_ok = true;

    void Load()
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) {
            // No file yet is the normal first-launch case, not an error.
            return;
        }

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }

            // `type:key=value`. A malformed line is skipped rather than aborting the load,
            // so one bad line does not lose the whole session.
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::size_t equals = line.find('=', colon + 1);
            if (equals == std::string::npos) {
                continue;
            }

            ValueType type = ValueType::String;
            if (!ParseTypeTag(line.substr(0, colon), type)) {
                continue;
            }

            const std::string key = line.substr(colon + 1, equals - colon - 1);
            if (!IsValidKey(key)) {
                continue;
            }

            Entry entry;
            entry.type = type;
            entry.text = line.substr(equals + 1);
            entries[key] = entry;
        }
    }

    void Save()
    {
        // Written whole. A partial write is possible on a crash, which is documented on the
        // class rather than papered over: the cost is one re-authentication.
        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            last_write_ok = false;
            return;
        }

        for (std::map<std::string, Entry>::const_iterator it = entries.begin();
             it != entries.end(); ++it) {
            out << TypeTag(it->second.type) << ':' << it->first << '=' << it->second.text
                << '\n';
        }

        out.flush();
        last_write_ok = out.good();
    }

    void Put(const std::string& key, ValueType type, const std::string& text)
    {
        if (!IsValidKey(key)) {
            // A key with a separator cannot be represented. Silently dropping it is better
            // than writing a file that will not load back.
            return;
        }
        Entry entry;
        entry.type = type;
        entry.text = SanitizeValue(text);
        entries[key] = entry;
        Save();
    }

    bool Get(const std::string& key, ValueType expected, std::string& out) const
    {
        const std::map<std::string, Entry>::const_iterator it = entries.find(key);
        if (it == entries.end() || it->second.type != expected) {
            return false;
        }
        out = it->second.text;
        return true;
    }
};

FileStateWriter::FileStateWriter(const std::string& path) : m_impl(new Impl())
{
    m_impl->path = path.empty() ? std::string("lootlocker-state.txt") : path;
    m_impl->Load();
}

FileStateWriter::~FileStateWriter() = default;

const std::string& FileStateWriter::GetPath() const
{
    return m_impl->path;
}

bool FileStateWriter::LastWriteSucceeded() const
{
    return m_impl->last_write_ok;
}

bool FileStateWriter::GetString(const std::string& key, std::string& out)
{
    return m_impl->Get(key, ValueType::String, out);
}

void FileStateWriter::SetString(const std::string& key, const std::string& value)
{
    m_impl->Put(key, ValueType::String, value);
}

bool FileStateWriter::GetInt64(const std::string& key, std::int64_t& out)
{
    std::string text;
    if (!m_impl->Get(key, ValueType::Int64, text)) {
        return false;
    }

    // `strtoll` with an end pointer, so trailing garbage is a failure rather than a silent
    // truncation.
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::int64_t>(parsed);
    return true;
}

void FileStateWriter::SetInt64(const std::string& key, std::int64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    m_impl->Put(key, ValueType::Int64, buffer);
}

bool FileStateWriter::GetUInt64(const std::string& key, std::uint64_t& out)
{
    std::string text;
    if (!m_impl->Get(key, ValueType::UInt64, text)) {
        return false;
    }

    // `strtoull` rather than `strtoll`: a `player_id` above `INT64_MAX` is valid and must
    // round-trip exactly (D4). A leading '-' is rejected explicitly, because `strtoull`
    // accepts it and wraps.
    if (!text.empty() && text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint64_t>(parsed);
    return true;
}

void FileStateWriter::SetUInt64(const std::string& key, std::uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    m_impl->Put(key, ValueType::UInt64, buffer);
}

bool FileStateWriter::GetDouble(const std::string& key, double& out)
{
    std::string text;
    if (!m_impl->Get(key, ValueType::Double, text)) {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

void FileStateWriter::SetDouble(const std::string& key, double value)
{
    m_impl->Put(key, ValueType::Double, FormatDouble(value));
}

bool FileStateWriter::GetBool(const std::string& key, bool& out)
{
    std::string text;
    if (!m_impl->Get(key, ValueType::Bool, text)) {
        return false;
    }
    if (text == "1") {
        out = true;
        return true;
    }
    if (text == "0") {
        out = false;
        return true;
    }
    return false;
}

void FileStateWriter::SetBool(const std::string& key, bool value)
{
    m_impl->Put(key, ValueType::Bool, value ? "1" : "0");
}

void FileStateWriter::Erase(const std::string& key)
{
    if (m_impl->entries.erase(key) != 0) {
        m_impl->Save();
    }
}

bool FileStateWriter::Has(const std::string& key)
{
    return m_impl->entries.find(key) != m_impl->entries.end();
}

} // namespace lootlocker

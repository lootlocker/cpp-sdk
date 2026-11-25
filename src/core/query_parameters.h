#pragma once

// Query parameters for one request (T13). Internal: `lootlocker::core::` is not on an
// adopter's include path (D18).
//
// Typed rather than a string map, because a definition knows the type of each parameter and
// a generic map would throw that away at the call site. Insertion-ordered, because the API
// documents parameter order and some proxies care.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lootlocker {
namespace core {

class QueryParameters {
public:
    void Add(const std::string& name, const std::string& value);

    // A string literal must not fall through to the `bool` overload.
    //
    // Without this, `Add("name", "abc")` picks `bool`: `const char*` to `bool` is a standard
    // conversion and `const char*` to `std::string` is a user-defined one, so the literal
    // silently becomes `true`. That is a wrong request with no diagnostic, which is the
    // worst kind of bug to ship.
    void Add(const std::string& name, const char* value);

    void Add(const std::string& name, int value);
    void Add(const std::string& name, std::int64_t value);
    void Add(const std::string& name, double value);
    void Add(const std::string& name, bool value);

    // Percent-encoded, joined with '&', no leading '?'. Empty when nothing was added.
    std::string ToQueryString() const;

    std::size_t Size() const { return m_entries.size(); }
    bool        Empty() const { return m_entries.empty(); }

private:
    struct Entry {
        std::string name;
        std::string value;
    };

    std::vector<Entry> m_entries;
};

} // namespace core
} // namespace lootlocker

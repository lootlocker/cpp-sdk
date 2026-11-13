#pragma once

// An ordered, case-insensitive header collection (D3).
//
// Public because `IHttpTransport` is public and an adopter implementing it has to build
// one. It is deliberately not `std::map<std::string, std::string>`: header order and case
// matter to some servers and proxies, and a `std::map` would silently reorder them and
// lose the original casing.

#include <cstddef>
#include <string>
#include <vector>

namespace lootlocker {

// Insertion-ordered, case-insensitive on lookup, case-preserving on send.
//
// `Set` replaces an existing header in place, keeping its original position, so a caller
// that overwrites `Content-Type` does not move it to the end of the request.
class HeaderMap {
public:
    void Set(const std::string& name, const std::string& value);

    // Case-insensitive lookup. Returns false and leaves `out` untouched when absent.
    bool Get(const std::string& name, std::string& out) const;

    bool Has(const std::string& name) const;

    // Case-insensitive. A no-op when absent.
    void Erase(const std::string& name);

    std::size_t Size() const { return m_entries.size(); }
    bool        Empty() const { return m_entries.empty(); }

    // Iteration in insertion order, for a transport that has to build a wire format.
    struct Entry {
        std::string name;
        std::string value;
    };

    const std::vector<Entry>& Entries() const { return m_entries; }

    void Clear() { m_entries.clear(); }

private:
    std::vector<Entry> m_entries;
};

} // namespace lootlocker

#include <lootlocker/header_map.h>

#include "text.h"

namespace lootlocker {

namespace {

bool NameMatches(const std::string& a, const std::string& b)
{
    return core::text::EqualsIgnoreCase(a, b);
}

} // namespace

void HeaderMap::Set(const std::string& name, const std::string& value)
{
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (NameMatches(m_entries[i].name, name)) {
            // Replace in place: overwriting a header must not move it to the end, because
            // some servers care about order.
            m_entries[i].value = value;
            return;
        }
    }
    Entry entry;
    entry.name = name;
    entry.value = value;
    m_entries.push_back(entry);
}

bool HeaderMap::Get(const std::string& name, std::string& out) const
{
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (NameMatches(m_entries[i].name, name)) {
            out = m_entries[i].value;
            return true;
        }
    }
    return false;
}

bool HeaderMap::Has(const std::string& name) const
{
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (NameMatches(m_entries[i].name, name)) {
            return true;
        }
    }
    return false;
}

void HeaderMap::Erase(const std::string& name)
{
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        if (NameMatches(m_entries[i].name, name)) {
            m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

} // namespace lootlocker

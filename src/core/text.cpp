#include "text.h"

namespace lootlocker {
namespace core {
namespace text {

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

bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace text
} // namespace core
} // namespace lootlocker

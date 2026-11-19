#include <lootlocker/request_id.h>

namespace lootlocker {

std::string RequestId::ToString() const
{
    // Hand-rolled rather than std::to_string: this is called once per request for the
    // header and once per log line, and std::to_string goes through the locale-aware
    // formatting machinery for a value that is always plain decimal digits.
    if (m_value == 0) {
        return std::string("0");
    }

    char buffer[20];   // 2^64 - 1 is 20 digits
    std::size_t length = 0;
    std::uint64_t value = m_value;
    while (value != 0) {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    std::string out;
    out.reserve(length);
    while (length > 0) {
        out.push_back(buffer[--length]);
    }
    return out;
}

} // namespace lootlocker

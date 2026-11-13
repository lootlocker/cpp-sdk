#pragma once

// Small string helpers shared across `core` (D18).
//
// These exist because the same three functions were written independently in four
// translation units, which is how two of them ended up subtly different. Anything that is
// genuinely general-purpose string handling belongs here rather than in a file-local
// anonymous namespace.

#include <cstddef>
#include <string>

namespace lootlocker {
namespace core {
namespace text {

// ASCII-only case folding.
//
// Deliberately not `std::tolower`: that is locale-dependent, which makes behaviour depend
// on the host's locale rather than on the code, and it takes an `int`, which is a footgun
// with signed `char`. Everything this SDK case-folds is ASCII — timezone names, log level
// names, denied field names — so ASCII folding is the correct behaviour rather than a
// shortcut.
char LowerAscii(char c);

// A lowercased copy. Allocates, so prefer `EqualsIgnoreCase` when comparing.
std::string Lower(const std::string& s);

// Case-insensitive equality against a NUL-terminated string.
//
// The `const char*` overload exists because the common call is against a literal or a
// table entry, and constructing a `std::string` for that would allocate on a path that
// runs per log line.
bool EqualsIgnoreCase(const std::string& a, const char* b);

bool EqualsIgnoreCase(const std::string& a, const std::string& b);

} // namespace text
} // namespace core
} // namespace lootlocker

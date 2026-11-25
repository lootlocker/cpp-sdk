#pragma once

// The JSON layer (D4, D18). Internal: `lootlocker::core::` is not on an adopter's include
// path.
//
// **This is the only header that names the JSON library, and `json.cpp` is the only
// translation unit that includes it.** Everything else in the SDK sees `JsonValue`, which
// is a handle over an opaque implementation. That is what makes D4's "swap the parser"
// fallback a one-file change rather than a breaking change.

#include <lootlocker/json_value.h>
#include <lootlocker/result.h>

#include <string>

namespace lootlocker {
namespace core {

// Parses `text` into a `JsonValue`.
//
// Never throws: the build has exceptions disabled (`/EHs-c-`), so the library is configured
// with `JSON_NOEXCEPTION` and parsing uses the non-throwing overload. A malformed document
// is a `MalformedResponse` error carrying the parser's own message, not a crash and not a
// silently empty value (D23).
//
// The returned value owns its document, so it stays valid after `text` goes out of scope.
Result<JsonValue> ParseJson(const std::string& text);

// Serialises a value back to text. Used by tests and by anything that has to log a
// re-encoded body; the SDK itself never re-serialises a response, because `RawBody()` is
// the original bytes (D7).
std::string SerializeJson(const JsonValue& value);

} // namespace core
} // namespace lootlocker

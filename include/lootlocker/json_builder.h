#pragma once

// A parser-independent JSON writer (D4, D27).
//
// The counterpart to `JsonValue`: that one reads a decoded document, this one builds a
// request body. Both are handles over an opaque implementation, so the chosen JSON library
// stays behind the pimpl and swapping it is not a breaking change.
//
// Deliberately a separate type rather than a mutable `JsonValue`. A `JsonValue` is a path
// into a shared, immutable document, which is what makes it cheap to pass around and safe
// to hold across a callback. Making it writable would mean either copying the document on
// every write or giving up that sharing - so the two roles get two types.
//
// An adopter writes a `to_json` next to their type, exactly as they write a `from_json`:
//
//     void to_json(lootlocker::JsonBuilder& b, const MyRequest& in)
//     {
//         b.Set("name", in.name);
//         b.Set("score", in.score);
//     }
//
// and `core::Send<Definition>()` finds it by ADL and sends the result as the body.

#include <cstdint>
#include <memory>
#include <string>

namespace lootlocker {

class JsonBuilder {
public:
    // An empty object. A request body is always an object at the root.
    JsonBuilder();
    ~JsonBuilder();

    JsonBuilder(JsonBuilder&&) noexcept;
    JsonBuilder& operator=(JsonBuilder&&) noexcept;

    JsonBuilder(const JsonBuilder&)            = delete;
    JsonBuilder& operator=(const JsonBuilder&) = delete;

    void Set(const std::string& key, const std::string& value);

    // A string literal must not fall through to the `bool` overload. Without this,
    // `Set("name", "abc")` picks `bool`, because `const char*` to `bool` is a standard
    // conversion and to `std::string` is a user-defined one - so the literal silently
    // becomes `true`. The same trap as `QueryParameters::Add`.
    void Set(const std::string& key, const char* value);

    void Set(const std::string& key, int value);
    void Set(const std::string& key, std::int64_t value);
    void Set(const std::string& key, std::uint64_t value);
    void Set(const std::string& key, double value);
    void Set(const std::string& key, bool value);

    // An explicit JSON null, which is different from omitting the key. The API treats
    // `"optionals": null` and an absent `optionals` the same way, but a field that is
    // genuinely nullable needs to be expressible.
    void SetNull(const std::string& key);

    // A nested object, serialised through the type's ADL `to_json`.
    template <typename T>
    void Set(const std::string& key, const T& value)
    {
        JsonBuilder child;
        to_json(child, value);
        SetValue(key, std::move(child));
    }

    // The serialised object. Never throws: invalid UTF-8 in a string is replaced rather
    // than aborting, because the build has exceptions disabled and a player name is not
    // worth terminating the process over.
    std::string ToJson() const;

private:
    // Attaches an already-built child under `key`. Out of line because it moves the
    // underlying document, which only this translation unit's implementation knows about.
    void SetValue(const std::string& key, JsonBuilder&& child);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lootlocker

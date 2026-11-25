#pragma once

// A parser-independent view over a decoded JSON value (D4, D27).
//
// This exists so an adopter can write a custom serialiser without taking a compile-time
// dependency on our parser choice. D4 chose nlohmann/json *behind a pimpl* precisely so the
// library is not part of our API; handing adopters `nlohmann::json` in a `from_json`
// signature would undo that in one line, and swapping the parser (which D4 names as the
// fallback if the pimpl is ever dropped) would become a breaking change.
//
// So this is a handle, not a container: it exposes the shape (kind, members, elements) and
// nothing about which library produced it. The implementation is in `src/core/json.cpp`,
// which is the only translation unit that includes the JSON library.

#include <lootlocker/result.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lootlocker {

class JsonValue;

namespace core {
class JsonParser;

// Declared here so the friend declaration below can name it. Defined in `src/core/json.cpp`.
Result<JsonValue> ParseJson(const std::string& text);
} // namespace core

class JsonValue {
public:
    // `Invalid` is not a JSON kind: it is what an absent key or an out-of-range index
    // returns, so a caller can tell "the key is missing" from "the key is null". Those are
    // different answers and conflating them is how a decoder silently defaults a field.
    enum class Kind { Invalid, Null, Bool, Number, String, Array, Object };

    JsonValue();
    ~JsonValue();

    JsonValue(const JsonValue&);
    JsonValue& operator=(const JsonValue&);
    JsonValue(JsonValue&&) noexcept;
    JsonValue& operator=(JsonValue&&) noexcept;

    // False for the default-constructed value and for an absent key.
    bool IsValid() const;

    Kind GetKind() const;

    // Typed reads. Each returns a default (false, 0, empty) when the value is not of that
    // kind, so a caller that has already checked `GetKind()` pays nothing and a caller that
    // has not gets a defined answer rather than undefined behaviour.
    bool          AsBool() const;
    double        AsDouble() const;
    std::int64_t  AsInt64() const;
    std::uint64_t AsUInt64() const;
    std::string   AsString() const;

    // Object access. Returns an invalid value when the key is absent or this is not an
    // object.
    JsonValue operator[](const std::string& key) const;
    bool      HasKey(const std::string& key) const;

    // Array access. `Size()` is 0 for anything that is not an array; `At()` returns an
    // invalid value when the index is out of range.
    std::size_t Size() const;
    JsonValue   At(std::size_t index) const;

private:
    friend class core::JsonParser;

    // `ParseJson` builds the root handle, so it needs the private constructor. Declared as a
    // friend function rather than widening the constructor, so nothing else can mint a
    // `JsonValue` from an impl.
    friend Result<JsonValue> core::ParseJson(const std::string& text);

    struct Impl;

    explicit JsonValue(std::shared_ptr<const Impl> impl);

    // Shared, so a sub-value keeps its document alive and copying a handle is cheap. A
    // `unique_ptr` would force a deep copy on every `operator[]`, which for a large response
    // is the whole document.
    std::shared_ptr<const Impl> m_impl;
};

} // namespace lootlocker

// The only translation unit that includes the JSON library (D4, D18).
//
// Everything else in the SDK sees `JsonValue`, an opaque handle. Swapping the parser is a
// change to this file and nothing else.

#include "json.h"

// `JSON_NOEXCEPTION` makes the library's throwing paths abort rather than throw, and
// `parse(..., allow_exceptions = false)` returns a discarded value instead. Both are
// required: the build has exceptions disabled (`/EHs-c-`), so a `try`/`catch` here would
// not even compile.
#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace lootlocker {

// The opaque implementation.
//
// One per parsed document, shared by every sub-value. A sub-value is a *path* into that
// document rather than a copy, so `operator[]` on a large response is a lookup rather than
// a deep copy - and a `JsonValue` stays valid for as long as any handle to it exists,
// which is what makes it safe to hold across a callback.
struct JsonValue::Impl {
    std::shared_ptr<const nlohmann::json> document;

    // The path to this value within `document`, as a sequence of keys and indices. Empty
    // for the root.
    //
    // A path rather than a pointer because nlohmann's iterators are invalidated by
    // mutation, and a path stays valid for the lifetime of the document.
    std::vector<std::string> path;
};

namespace {

// Resolve a path against a document. Returns nullptr when any step is missing, which is how
// an absent key becomes an invalid `JsonValue` rather than an exception.
const nlohmann::json* Resolve(const nlohmann::json& document,
                              const std::vector<std::string>& path)
{
    const nlohmann::json* current = &document;

    for (const std::string& step : path) {
        if (current->is_object()) {
            const auto it = current->find(step);
            if (it == current->end()) {
                return nullptr;
            }
            current = &(*it);
        } else if (current->is_array()) {
            std::size_t index = 0;
            for (const char c : step) {
                if (c < '0' || c > '9') {
                    return nullptr;
                }
                index = index * 10 + static_cast<std::size_t>(c - '0');
            }
            if (index >= current->size()) {
                return nullptr;
            }
            current = &(*current)[index];
        } else {
            return nullptr;
        }
    }

    return current;
}

} // namespace

JsonValue::JsonValue() = default;
JsonValue::~JsonValue() = default;
JsonValue::JsonValue(const JsonValue&) = default;
JsonValue& JsonValue::operator=(const JsonValue&) = default;
JsonValue::JsonValue(JsonValue&&) noexcept = default;
JsonValue& JsonValue::operator=(JsonValue&&) noexcept = default;

JsonValue::JsonValue(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}

bool JsonValue::IsValid() const
{
    return m_impl != nullptr && Resolve(*m_impl->document, m_impl->path) != nullptr;
}

JsonValue::Kind JsonValue::GetKind() const
{
    if (!m_impl) {
        return Kind::Invalid;
    }
    const nlohmann::json* value = Resolve(*m_impl->document, m_impl->path);
    if (value == nullptr) {
        return Kind::Invalid;
    }

    switch (value->type()) {
    case nlohmann::json::value_t::null:
        return Kind::Null;
    case nlohmann::json::value_t::boolean:
        return Kind::Bool;
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
    case nlohmann::json::value_t::number_float:
        return Kind::Number;
    case nlohmann::json::value_t::string:
        return Kind::String;
    case nlohmann::json::value_t::array:
        return Kind::Array;
    case nlohmann::json::value_t::object:
        return Kind::Object;
    default:
        // `binary` and `discarded` are not reachable from a parsed document.
        return Kind::Invalid;
    }
}

bool JsonValue::AsBool() const
{
    if (GetKind() != Kind::Bool) {
        return false;
    }
    return Resolve(*m_impl->document, m_impl->path)->get<bool>();
}

double JsonValue::AsDouble() const
{
    if (GetKind() != Kind::Number) {
        return 0.0;
    }
    return Resolve(*m_impl->document, m_impl->path)->get<double>();
}

std::int64_t JsonValue::AsInt64() const
{
    if (GetKind() != Kind::Number) {
        return 0;
    }
    const nlohmann::json* value = Resolve(*m_impl->document, m_impl->path);

    // Exact for every integer that fits, which is the property D4's spike established: the
    // hazard is a `double` field in our own structs, not the parser.
    if (value->is_number_unsigned()) {
        const auto raw = value->get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(9223372036854775807LL)) {
            return 9223372036854775807LL;
        }
        return static_cast<std::int64_t>(raw);
    }
    if (value->is_number_integer()) {
        return value->get<std::int64_t>();
    }

    // A float. `1e100` is valid JSON and does not fit an `int64_t`, and converting an
    // out-of-range `double` to an integer is undefined behaviour rather than a clamp - so
    // the range is checked before the cast, and the non-representable result is defined as
    // the saturated bound rather than left to the platform.
    const double raw = value->get<double>();
    if (!(raw >= -9223372036854775808.0)) {   // also false for NaN
        return -9223372036854775807LL - 1;
    }
    if (!(raw < 9223372036854775808.0)) {
        return 9223372036854775807LL;
    }
    return static_cast<std::int64_t>(raw);
}

std::uint64_t JsonValue::AsUInt64() const
{
    if (GetKind() != Kind::Number) {
        return 0;
    }
    const nlohmann::json* value = Resolve(*m_impl->document, m_impl->path);

    if (value->is_number_unsigned()) {
        return value->get<std::uint64_t>();
    }
    if (value->is_number_integer()) {
        const auto raw = value->get<std::int64_t>();
        return raw < 0 ? 0 : static_cast<std::uint64_t>(raw);
    }

    // A float. Checked before the cast for the same reason as `AsInt64`: an out-of-range
    // `double` to `uint64_t` conversion is undefined behaviour, and `1e100` is valid JSON.
    const double raw = value->get<double>();
    if (!(raw > 0.0)) {   // also false for NaN
        return 0;
    }
    if (!(raw < 18446744073709551616.0)) {
        return 18446744073709551615ULL;
    }
    return static_cast<std::uint64_t>(raw);
}

std::string JsonValue::AsString() const
{
    if (GetKind() != Kind::String) {
        return std::string();
    }
    return Resolve(*m_impl->document, m_impl->path)->get<std::string>();
}

JsonValue JsonValue::operator[](const std::string& key) const
{
    if (GetKind() != Kind::Object) {
        return JsonValue();
    }

    const nlohmann::json* value = Resolve(*m_impl->document, m_impl->path);
    if (value->find(key) == value->end()) {
        return JsonValue();
    }

    auto child = std::make_shared<Impl>();
    child->document = m_impl->document;
    child->path = m_impl->path;
    child->path.push_back(key);
    return JsonValue(std::move(child));
}

bool JsonValue::HasKey(const std::string& key) const
{
    if (GetKind() != Kind::Object) {
        return false;
    }
    const nlohmann::json* value = Resolve(*m_impl->document, m_impl->path);
    return value->find(key) != value->end();
}

std::size_t JsonValue::Size() const
{
    // Arrays only, as documented. An object reporting its member count would make a caller
    // treat it as an array-sized value, and `At()` on an object returns invalid anyway - so
    // the two would disagree.
    if (GetKind() != Kind::Array) {
        return 0;
    }
    return Resolve(*m_impl->document, m_impl->path)->size();
}

JsonValue JsonValue::At(std::size_t index) const
{
    if (GetKind() != Kind::Array) {
        return JsonValue();
    }
    const nlohmann::json* value = Resolve(*m_impl->document, m_impl->path);
    if (index >= value->size()) {
        return JsonValue();
    }

    auto child = std::make_shared<Impl>();
    child->document = m_impl->document;
    child->path = m_impl->path;
    child->path.push_back(std::to_string(index));
    return JsonValue(std::move(child));
}

namespace core {

Result<JsonValue> ParseJson(const std::string& text)
{
    // The non-throwing overload. `is_discarded()` is how a parse failure is reported when
    // exceptions are off, and it maps onto our own `MalformedResponse` (D23).
    nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);

    if (parsed.is_discarded()) {
        Error error;
        error.code = ErrorCode::MalformedResponse;
        error.message = "the response body is not valid JSON";
        return Result<JsonValue>::Fail(std::move(error), text);
    }

    auto impl = std::make_shared<JsonValue::Impl>();
    impl->document = std::make_shared<const nlohmann::json>(std::move(parsed));
    return Result<JsonValue>::Ok(JsonValue(std::move(impl)), text);
}

std::string SerializeJson(const JsonValue& value)
{
    if (!value.IsValid()) {
        return std::string();
    }

    // Serialised from the resolved document rather than rebuilt through the public surface.
    //
    // Rebuilding was wrong in two ways: a number went through `AsDouble()`, so an integer
    // like 9007199254740993 came back as 9007199254740992; and an object inside an array
    // serialised to an empty string, which the non-throwing parse then turned into a
    // discarded value - and `dump()` on a discarded value aborts under `JSON_NOEXCEPTION`.
    //
    // This is `core`, so it can reach the impl. Only tests and logging use it - the SDK
    // never re-serialises a response, because `RawBody()` is the original bytes (D7).
    const nlohmann::json* resolved = Resolve(*value.m_impl->document, value.m_impl->path);
    if (resolved == nullptr) {
        return std::string();
    }
    return resolved->dump();
}

} // namespace core
} // namespace lootlocker

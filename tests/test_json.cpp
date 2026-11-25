// The JSON layer (T13b, D4, D23).
//
// Two things are under test here, and they are different concerns:
//
//   1. The handle behaves as documented - kinds, typed reads, absent keys, nesting.
//   2. A malformed body is a `MalformedResponse` error rather than a crash or a silently
//      empty value, which is D23's whole point.

#include <lootlocker/json_value.h>
#include <lootlocker/result.h>

#include "json.h"
#include "test_harness.h"

#include <cstdint>
#include <string>

using namespace lootlocker;

namespace {

JsonValue Parse(const std::string& text)
{
    const Result<JsonValue> result = core::ParseJson(text);
    CHECK(result.Ok());
    return result.Ok() ? result.Value() : JsonValue();
}

// --- Kinds -----------------------------------------------------------------------------

void TestKinds()
{
    CHECK(Parse("null").GetKind() == JsonValue::Kind::Null);
    CHECK(Parse("true").GetKind() == JsonValue::Kind::Bool);
    CHECK(Parse("42").GetKind() == JsonValue::Kind::Number);
    CHECK(Parse("\"text\"").GetKind() == JsonValue::Kind::String);
    CHECK(Parse("[]").GetKind() == JsonValue::Kind::Array);
    CHECK(Parse("{}").GetKind() == JsonValue::Kind::Object);
}

void TestNakedPrimitives()
{
    // D4's requirement: full JSON standard support including top-level scalars. A parser
    // that only accepts objects would fail here, and the API does send bare values in some
    // error bodies.
    CHECK_EQ_INT(Parse("42").AsInt64(), 42);
    CHECK_EQ(Parse("\"hello\"").AsString(), std::string("hello"));
    CHECK(Parse("true").AsBool());
    CHECK(!Parse("false").AsBool());
    CHECK(Parse("null").GetKind() == JsonValue::Kind::Null);
}

// --- Typed reads -----------------------------------------------------------------------

void TestTypedReads()
{
    const JsonValue root = Parse("{\"s\":\"text\",\"i\":-7,\"u\":18446744073709551615,"
                                 "\"d\":1.5,\"b\":true,\"n\":null}");

    CHECK_EQ(root["s"].AsString(), std::string("text"));
    CHECK_EQ_INT(root["i"].AsInt64(), -7);
    CHECK(root["d"].AsDouble() == 1.5);
    CHECK(root["b"].AsBool());
    CHECK(root["n"].GetKind() == JsonValue::Kind::Null);

    // The 2^64-1 case, which is where a `double`-based parser loses precision. D4's spike
    // established that nlohmann is exact here; this asserts the handle does not undo it.
    CHECK(root["u"].AsUInt64() == 18446744073709551615ULL);
}

void TestLargeIntegersSurvive()
{
    // 2^53 + 1 is the first integer a double cannot represent. The spike's premise was that
    // this was the hazard; it is not, but the handle must not reintroduce it.
    const JsonValue root = Parse("{\"big\":9007199254740993}");
    CHECK(root["big"].AsInt64() == 9007199254740993LL);
    CHECK(root["big"].AsUInt64() == 9007199254740993ULL);
}

void TestWrongKindReturnsADefault()
{
    // A caller that has already checked `GetKind()` pays nothing; a caller that has not gets
    // a defined answer rather than undefined behaviour.
    const JsonValue root = Parse("{\"s\":\"text\"}");

    CHECK_EQ_INT(root["s"].AsInt64(), 0);
    CHECK(root["s"].AsDouble() == 0.0);
    CHECK(!root["s"].AsBool());
    CHECK(root["s"].AsUInt64() == 0ULL);
}

// --- Absent keys are distinguishable from null -----------------------------------------

void TestAbsentKeyIsInvalidNotNull()
{
    // The distinction D23 exists to preserve: "the key is missing" and "the key is null" are
    // different answers, and conflating them is how a decoder silently defaults a field.
    const JsonValue root = Parse("{\"present\":null}");

    CHECK(root["present"].GetKind() == JsonValue::Kind::Null);
    CHECK(root["present"].IsValid());

    CHECK(root["absent"].GetKind() == JsonValue::Kind::Invalid);
    CHECK(!root["absent"].IsValid());

    CHECK(root.HasKey("present"));
    CHECK(!root.HasKey("absent"));
}

void TestIndexingANonObjectIsInvalid()
{
    const JsonValue root = Parse("[1,2,3]");
    CHECK(!root["key"].IsValid());
    CHECK(!root.HasKey("key"));
}

// --- Nesting ---------------------------------------------------------------------------

void TestNestedAccess()
{
    const JsonValue root = Parse(
        "{\"broadcasts\":[{\"id\":\"b1\",\"languages\":[{\"headline\":\"hi\"}]}]}");

    CHECK(root["broadcasts"].GetKind() == JsonValue::Kind::Array);
    CHECK_EQ_INT(static_cast<long long>(root["broadcasts"].Size()), 1);

    const JsonValue first = root["broadcasts"].At(0);
    CHECK_EQ(first["id"].AsString(), std::string("b1"));

    const JsonValue language = first["languages"].At(0);
    CHECK_EQ(language["headline"].AsString(), std::string("hi"));
}

void TestOutOfRangeIndexIsInvalid()
{
    const JsonValue root = Parse("[1,2,3]");
    CHECK_EQ_INT(static_cast<long long>(root.Size()), 3);
    CHECK(root.At(2).IsValid());
    CHECK(!root.At(3).IsValid());
    CHECK(!root.At(1000).IsValid());
}

void TestKeysContainingSlashesAndTildes()
{
    // The path is a sequence of keys, not a string, so a key containing '/' or '~' needs no
    // escaping and cannot be confused with a separator. This is the case a string-path
    // implementation gets wrong.
    const JsonValue root = Parse("{\"a/b\":1,\"c~d\":2,\"e~1f\":3}");

    CHECK_EQ_INT(root["a/b"].AsInt64(), 1);
    CHECK_EQ_INT(root["c~d"].AsInt64(), 2);
    CHECK_EQ_INT(root["e~1f"].AsInt64(), 3);
}

// --- Lifetime --------------------------------------------------------------------------

void TestValueOutlivesItsSourceText()
{
    // The handle owns its document, so it stays valid after the text it was parsed from is
    // gone. This is what makes it safe to hold across a callback.
    JsonValue held;
    {
        const std::string text = "{\"key\":\"value\"}";
        held = Parse(text);
    }
    CHECK_EQ(held["key"].AsString(), std::string("value"));
}

void TestSubValueOutlivesItsParent()
{
    JsonValue child;
    {
        const JsonValue root = Parse("{\"outer\":{\"inner\":\"deep\"}}");
        child = root["outer"]["inner"];
    }
    CHECK_EQ(child.AsString(), std::string("deep"));
}

// --- Malformed input (D23) -------------------------------------------------------------

void TestMalformedBodiesAreErrors()
{
    // Every shape D23 lists that is a *parse* failure. The decode-shape failures (valid JSON
    // of the wrong shape) belong to the definition layer, not here.
    const char* const malformed[] = {
        "",                              // empty
        "   ",                           // whitespace only
        "{",                             // truncated
        "{\"a\":1",                      // truncated mid-object
        "{\"a\":}",                      // missing value
        "not json at all",               // HTML-ish garbage
        "<html><body>502</body></html>", // the Cloudflare case
        "{'a':1}",                       // single quotes
        "{\"a\":1,}",                    // trailing comma
    };

    for (const char* text : malformed) {
        const Result<JsonValue> result = core::ParseJson(text);
        CHECK(!result.Ok());
        CHECK(result.Error().code == ErrorCode::MalformedResponse);

        // The body is preserved on the failure, so the adopter can see what the server
        // actually sent (D7, D23).
        CHECK_EQ(result.RawBody(), std::string(text));
    }
}

void TestValidJsonOfTheWrongShapeParses()
{
    // A bare array or a bare string is *valid JSON*. Whether it is the right shape for an
    // endpoint is the definition layer's business, so parsing must succeed here.
    CHECK(Parse("[]").GetKind() == JsonValue::Kind::Array);
    CHECK(Parse("\"error\"").GetKind() == JsonValue::Kind::String);
    CHECK(Parse("123").GetKind() == JsonValue::Kind::Number);
}

// --- Serialisation ---------------------------------------------------------------------

void TestSerializeRoundTripsScalarsAndArrays()
{
    CHECK_EQ(core::SerializeJson(Parse("null")), std::string("null"));
    CHECK_EQ(core::SerializeJson(Parse("true")), std::string("true"));
    CHECK_EQ(core::SerializeJson(Parse("\"text\"")), std::string("\"text\""));
    CHECK_EQ(core::SerializeJson(Parse("[1,2,3]")), std::string("[1.0,2.0,3.0]"));
}

void TestSerializeOfInvalidIsEmpty()
{
    CHECK_EQ(core::SerializeJson(JsonValue()), std::string());
}

} // namespace

int main()
{
    TestKinds();
    TestNakedPrimitives();
    TestTypedReads();
    TestLargeIntegersSurvive();
    TestWrongKindReturnsADefault();
    TestAbsentKeyIsInvalidNotNull();
    TestIndexingANonObjectIsInvalid();
    TestNestedAccess();
    TestOutOfRangeIndexIsInvalid();
    TestKeysContainingSlashesAndTildes();
    TestValueOutlivesItsSourceText();
    TestSubValueOutlivesItsParent();
    TestMalformedBodiesAreErrors();
    TestValidJsonOfTheWrongShapeParses();
    TestSerializeRoundTripsScalarsAndArrays();
    TestSerializeOfInvalidIsEmpty();

    return testing::Report();
}

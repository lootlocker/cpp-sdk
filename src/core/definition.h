#pragma once

// The endpoint definition layer (T13). Internal: `lootlocker::core::` is not on an
// adopter's include path (D18).
//
// A definition is the internal unit of modularity. It carries everything the generic
// executor needs for one endpoint and nothing else: the wire contract (`kMethod`, `kPath`),
// the request and response types, and the three hooks D27 specifies. Retry policy, rate
// limiting, session refresh and logging are deliberately absent - they are the executor's
// job and identical for every endpoint.
//
// `Send<Definition>()` is the only entry point. It is a free function rather than a `Client`
// member for the same reason the modules are free functions: a member would make `core`
// depend on every module's definitions.

#include <lootlocker/client.h>
#include <lootlocker/header_map.h>
#include <lootlocker/json_builder.h>
#include <lootlocker/json_value.h>
#include <lootlocker/request_id.h>
#include <lootlocker/result.h>
#include <lootlocker/version.h>

#include "executor.h"
#include "json.h"
#include "query_parameters.h"

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

namespace lootlocker {
namespace core {

// The response as it arrived, before any decoding.
//
// Passed to `NormalizeResponse` alongside the decoded value, because the decoded form is
// not always enough: classifying a discriminated-union field needs the raw JSON token, not
// a struct (D27).
struct RawResponse {
    int         status_code = 0;
    HeaderMap   headers;
    std::string body;
};

// What a definition must provide. Checked at compile time rather than documented, because
// a missing hook would otherwise be a link error inside a template instantiation, which is
// the least readable failure the SDK can produce.
//
// `NormalizeResponse` is optional: a definition whose response needs no convenience simply
// omits it, and the trait below detects that. The other two are required.
template <typename Def, typename = void>
struct HasNormalizeResponse : std::false_type {};

template <typename Def>
struct HasNormalizeResponse<
    Def, decltype(static_cast<void (*)(typename Def::Response&, const RawResponse&)>(
             &Def::NormalizeResponse))> : std::true_type {};

// Calls `NormalizeResponse` when the definition has one, and does nothing when it does not.
template <typename Def>
typename std::enable_if<HasNormalizeResponse<Def>::value>::type Normalize(
    typename Def::Response& response, const RawResponse& raw)
{
    Def::NormalizeResponse(response, raw);
}

template <typename Def>
typename std::enable_if<!HasNormalizeResponse<Def>::value>::type Normalize(
    typename Def::Response&, const RawResponse&)
{
}

// Whether a request type has an ADL `to_json`, which is what decides if the request carries
// a body (D27, owner 2026-11-25).
//
// The body is a property of the request type, not a hook on the definition, for the same
// reason `from_json` is: a nested type that appears in several bodies - `optionals` in every
// session start - needs one serialiser, and a definition hook would still have to call it.
// One mechanism throughout.
//
// Detected rather than required, so a GET definition needs nothing: no `to_json`, no body,
// no `Content-Type`.
template <typename T, typename = void>
struct HasToJson : std::false_type {};

template <typename T>
struct HasToJson<T, decltype(to_json(std::declval<JsonBuilder&>(), std::declval<const T&>()),
                             void())> : std::true_type {};

// Serialises `request` into a body, or returns an empty string when the type has no
// `to_json`.
template <typename T>
typename std::enable_if<HasToJson<T>::value, std::string>::type BuildBody(const T& request)
{
    JsonBuilder builder;
    to_json(builder, request);
    return builder.ToJson();
}

template <typename T>
typename std::enable_if<!HasToJson<T>::value, std::string>::type BuildBody(const T&)
{
    return std::string();
}

// The callback a caller supplies. Takes the `Result` by value, so a lambda, a
// `std::function` or a member-function wrapper all work.
template <typename Def>
using ResponseCallback = std::function<void(Result<typename Def::Response>)>;

// Builds the URL for one request: base, path, then the definition's query parameters.
//
// Exposed rather than file-local because the tests assert on it directly, and because a
// definition's query construction is the part most likely to be wrong.
template <typename Def>
std::string BuildUrl(const Client& client, const typename Def::Request& request)
{
    std::string url = client.GetConfig().ResolveBaseUrl();
    url += Def::kPath;

    QueryParameters query;
    Def::ConstructQueryParameters(request, query);
    if (!query.Empty()) {
        url.push_back('?');
        url += query.ToQueryString();
    }
    return url;
}

// The headers every request carries, before the definition adds its own.
//
// `LL-Version` is pinned rather than omitted. The backend defaults a missing header to
// `2021-03-01`, the oldest API version, so omitting it would silently opt every adopter
// into the oldest behaviour. Pinning it makes the version the SDK targets explicit and
// makes a future bump a one-line change here.
//
// `LL-SDK-Version` and `LL-Instance-Identifier` match the other SDKs, and the backend's
// tracing middleware reads both: without them every request is attributed to "unknown" and
// a support ticket cannot be tied to a session. The instance identifier is per-client and
// per-process, so two clients in one process are distinguishable (D20).
//
// `x-session-token` is deliberately absent: there is no session yet (T14). It is added by
// the executor once one exists, not by a definition, because it is identical for every
// endpoint.
//
// `is-development` is deliberately NOT here. It is read only on the white-label login path,
// so it belongs in that definition's `ConstructHeaders` rather than on every request.
inline HeaderMap BuildDefaultHeaders(const Client& client)
{
    HeaderMap headers;

    headers.Set("LL-Version", "2021-03-01");
    headers.Set("LL-SDK-Version", GetVersionString());
    headers.Set("LL-Instance-Identifier", client.GetInstanceIdentifier());
    headers.Set("Accept", "application/json");
    headers.Set("User-Agent", std::string("LootLocker-Cpp-SDK/") + GetVersionString());

    return headers;
}

// Whether a definition's request should carry `x-session-token`.
//
// True by default, because most requests are authenticated. A definition opts out with
// `static constexpr bool kRequiresSession = false;` - which an authentication request must,
// because an authentication request is not an authenticated request by definition, and a
// stale token from a previous session has no business being sent to it.
template <typename Def, typename = void>
struct RequiresSession : std::true_type {};

template <typename Def>
struct RequiresSession<Def, decltype(void(Def::kRequiresSession))>
    : std::integral_constant<bool, Def::kRequiresSession> {};

// Whether a definition's response type can legitimately be empty.
//
// The generic `Send` treats an empty body as success, because a 204 is the one legitimate
// empty success (`EndSession`). That is wrong for a definition whose response carries
// required fields: a 200 with an empty body would become an all-default `Session`, and
// `HasSession()` would report true with no token. Such a definition opts out by declaring
// `static constexpr bool kAllowEmptyResponse = false;`.
template <typename Def, typename = void>
struct AllowsEmptyResponse : std::true_type {};

template <typename Def>
struct AllowsEmptyResponse<Def, decltype(void(Def::kAllowEmptyResponse))>
    : std::integral_constant<bool, Def::kAllowEmptyResponse> {};

// What a definition must provide, checked at compile time.
//
// Without this a missing member is a template error inside `Send`, which names a line in
// `definition.h` rather than the definition that is wrong. The asserts turn that into a
// message that says what is missing and where.
//
// This is the answer to "should there be an interface for definitions": the enforcement is
// real, and it costs no vtable and no base class.
template <typename Def>
struct DefinitionTraits {
    static_assert(std::is_convertible<decltype(Def::kMethod), const char*>::value,
                  "a definition must provide `static constexpr const char* kMethod`");
    static_assert(std::is_convertible<decltype(Def::kPath), const char*>::value,
                  "a definition must provide `static constexpr const char* kPath`");
    static_assert(std::is_class<typename Def::Request>::value,
                  "a definition must provide `using Request = ...`");
    static_assert(std::is_class<typename Def::Response>::value,
                  "a definition must provide `using Response = ...`");
};

// Sends one request described by `Def` and delivers the decoded response to `on_done`.
//
// Returns the id the executor assigned, which is the same id sent as `LL-Request-Id`. The
// callback never runs synchronously: it fires on the thread that calls `Client::Update()`
// (D5), including when the request is rejected outright.
//
// **The body comes from the request type's ADL `to_json`, when it has one.** A type without
// one sends no body and no `Content-Type`, which is what keeps every GET definition
// unchanged. `Content-Type` is set here rather than by a definition, because a body without
// it is a request the server rejects and no definition should have to remember.
template <typename Def>
RequestId Send(Client& client, const typename Def::Request& request,
               ResponseCallback<Def> on_done)
{
    PendingRequest pending;
    pending.method  = Def::kMethod;
    pending.url     = BuildUrl<Def>(client, request);
    pending.headers = BuildDefaultHeaders(client);
    pending.body    = BuildBody(request);

    // The definition says whether this request is authenticated. An authentication request
    // is not, so it must not carry a token - and a stale one from a previous session has no
    // business being sent to it.
    pending.requires_session = RequiresSession<Def>::value;

    if (!pending.body.empty()) {
        pending.headers.Set("Content-Type", "application/json");
    }

    // The definition's own headers come last, so an endpoint can override a default rather
    // than being stuck with it.
    Def::ConstructHeaders(request, pending.headers);

    return GetExecutor(client).Submit(
        std::move(pending), [on_done](const Completion& completion) {
            if (!completion.error.Ok()) {
                on_done(Result<typename Def::Response>::Fail(completion.error, completion.body));
                return;
            }

            // A 204 is the one legitimate empty success (`EndSession`), and the executor
            // documents it as such. Parsing an empty body would report `MalformedResponse`
            // for a request that succeeded, so it is handled before the parser sees it.
            //
            // A definition whose response carries required fields opts out with
            // `kAllowEmptyResponse = false`, because for it an empty body is a malformed
            // response rather than a success - a 200 with no body must not become an
            // all-default value that looks valid.
            //
            // `NormalizeResponse` still runs, because a definition may want to fill in
            // derived fields on an empty response too.
            if (completion.status_code == 204 ||
                (completion.body.empty() && AllowsEmptyResponse<Def>::value)) {
                typename Def::Response response;

                RawResponse raw;
                raw.status_code = completion.status_code;
                raw.headers     = completion.headers;
                raw.body        = completion.body;

                Normalize<Def>(response, raw);

                on_done(Result<typename Def::Response>::Ok(std::move(response), completion.body));
                return;
            }

            Result<JsonValue> parsed = ParseJson(completion.body);
            if (!parsed.Ok()) {
                // A body that is not JSON at all. The parse error carries the parser's
                // message, but the transport metadata is the executor's and must survive:
                // `request_id` is what ties the failure to a server log line, and
                // `status_code` is what tells the caller the request was actually answered.
                // Only the code and message are replaced.
                Error error = completion.error;
                error.code        = parsed.Error().code;
                error.message     = parsed.Error().message;
                error.status_code = completion.status_code;
                on_done(Result<typename Def::Response>::Fail(std::move(error), completion.body));
                return;
            }

            typename Def::Response response;
            Result<Empty> decoded = DecodeJson(parsed.Value(), response);
            if (!decoded.Ok()) {
                // Same reasoning: keep the transport metadata, replace only the decode
                // failure.
                Error error = completion.error;
                error.code        = decoded.Error().code;
                error.message     = decoded.Error().message;
                error.status_code = completion.status_code;
                on_done(Result<typename Def::Response>::Fail(std::move(error), completion.body));
                return;
            }

            RawResponse raw;
            raw.status_code = completion.status_code;
            raw.headers     = completion.headers;
            raw.body        = completion.body;

            Normalize<Def>(response, raw);

            on_done(Result<typename Def::Response>::Ok(std::move(response), completion.body));
        });
}

} // namespace core
} // namespace lootlocker

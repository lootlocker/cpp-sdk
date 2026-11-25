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
// `x-session-token` is deliberately absent: there is no session yet (T14). It is added by
// the executor once one exists, not by a definition, because it is identical for every
// endpoint.
inline HeaderMap BuildDefaultHeaders(const Client& client)
{
    HeaderMap headers;

    const Config& config = client.GetConfig();

    headers.Set("LL-Version", "2021-03-01");
    headers.Set("Accept", "application/json");
    headers.Set("User-Agent", std::string("LootLocker-Cpp-SDK/") + GetVersionString());

    // The backend reads `is-development` as a header on the white-label login path, and the
    // session body carries the same flag. Sending it only when true keeps the common case
    // free of a header that means "false".
    if (config.is_development) {
        headers.Set("is-development", "true");
    }

    return headers;
}

// Sends one request described by `Def` and delivers the decoded response to `on_done`.
//
// Returns the id the executor assigned, which is the same id sent as `LL-Request-Id`. The
// callback never runs synchronously: it fires on the thread that calls `Client::Update()`
// (D5), including when the request is rejected outright.
//
// **There is no request body.** Every endpoint in the MVP is a GET, and the spec has no
// body-construction hook. Adding one is T13c's job, together with the `to_json` that would
// fill it - see the note in tasks.md.
template <typename Def>
RequestId Send(Client& client, const typename Def::Request& request,
               ResponseCallback<Def> on_done)
{
    PendingRequest pending;
    pending.method  = Def::kMethod;
    pending.url     = BuildUrl<Def>(client, request);
    pending.headers = BuildDefaultHeaders(client);

    // The definition's own headers come last, so an endpoint can override a default rather
    // than being stuck with it.
    Def::ConstructHeaders(request, pending.headers);

    return GetExecutor(client).Submit(
        std::move(pending), [on_done](const Completion& completion) {
            if (!completion.error.Ok()) {
                on_done(Result<typename Def::Response>::Fail(completion.error, completion.body));
                return;
            }

            Result<JsonValue> parsed = ParseJson(completion.body);
            if (!parsed.Ok()) {
                // A body that is not JSON at all. `ParseJson` already reports
                // `MalformedResponse` with the parser's message, and the raw bytes are
                // carried so the caller can see what arrived (D7, D23).
                on_done(Result<typename Def::Response>::Fail(parsed.Error(), completion.body));
                return;
            }

            typename Def::Response response;
            Result<Empty> decoded = DecodeJson(parsed.Value(), response);
            if (!decoded.Ok()) {
                on_done(Result<typename Def::Response>::Fail(decoded.Error(), completion.body));
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

# LootLocker C++ SDK

A pure C++ SDK for the [LootLocker Game API](https://ref.lootlocker.com/game), so a studio
on an in-house engine, a middleware stack, or an engine we have no SDK for has a path to
LootLocker.

**Status: pre-release.** The design is accepted and the scaffolding is in place; the
features are being built. Nothing here is ready to ship against.

## What it is

- **C++17**, source-only, MIT.
- **No exceptions, no RTTI** in the public contract.
- **No internal threads by default.** You call `Update()`; the SDK does a frame's worth of
  work and returns. A built-in scheduler exists for hosts with no loop of their own, and
  it is opt-in.
- **Modular.** One CMake target per feature module. There is no umbrella target: you name
  the modules you use, and a module depends on `LootLocker::core` and nothing else.

## Requirements

- A C++17 compiler: MSVC 19.11+, GCC 7+, or Clang 4+.
- CMake 3.16+ if you use CMake. There is also a documented no-build-system path.

## Using it

The SDK vendors nlohmann/json as a submodule, so clone recursively:

```sh
git clone --recursive https://github.com/lootlocker/cpp-sdk.git
```

Then:

```cmake
add_subdirectory(third_party/cpp-sdk)

target_link_libraries(my_game PRIVATE
    LootLocker::auth
    LootLocker::broadcasts)
```

Only what you name is compiled and linked. To declare a subset of modules without
`add_subdirectory` deciding for you, pass
`-DLOOTLOCKER_MODULE_ONLY="auth;broadcasts"` — quoted, because an unquoted `;` ends the
`cmake` command on a POSIX shell.

## Driving it

Constructing a `Client` starts no thread and opens no socket. Requests move only when
someone calls `Update()`, which does a frame's worth of work and returns `false` when
nothing is in flight and nothing is queued:

```cpp
lootlocker::Config cfg;
cfg.domain_key   = "1g9glch3";
cfg.game_key     = "prod_1c52468fc6e8420c955e3b6c303ea8cc";
cfg.game_version = "1.2.1";

lootlocker::Client ll(cfg);

// In your loop, once per frame:
ll.Update();
```

`Update()` polls in-flight transfers, applies the retry policy, promotes queued requests
under the concurrency cap, and delivers callbacks — all in one call, on the calling thread.
A `while (ll.Update())` loop therefore terminates once the last callback has fired, which is
what lets a program with no host loop exit.

A bad config does not throw and does not crash. `Client::ConfigError()` reports it, and the
client is inert: `Update()` returns `false` and nothing is ever sent.

### The queue's policy

`QueueConfig` is public and per-client, because a slow platform or a tight memory budget
needs different numbers:

| Field | Default | What it does |
| --- | --- | --- |
| `max_retries` | 5 | Retries after the first attempt |
| `initial_backoff_ms` | 50 | First delay; doubles each retry |
| `backoff_factor` | 2 | Multiplier per retry |
| `max_in_flight` | 50 | Concurrent transfers |
| `max_queue_size` | 5000 | Queued requests before the queue is "full" |
| `choke_warning_threshold` | 500 | When to warn that the queue is backing up |
| `request_timeout_seconds` | 300 | Whole-request budget |
| `deny_when_backed_up` | true | Reject on a full queue instead of stalling |

The retry policy, which is the part that bites:

- **Retried:** 5xx, 429, and a transport failure (no response at all).
- **Not retried:** any other 4xx. A 400 will be a 400 again.
- **`Retry-After` is obeyed exactly** when the server sends one, instead of guessing.
- **A lost response is retried whatever the method.** A POST that timed out may have
  succeeded, and retrying it can double-grant — that is a real risk we accept rather than
  guard against, because the alternative was a per-request `idempotent` flag that moved a
  correctness decision onto the caller. See R25 in the spec.

**There is no request deduplication or grouping.** Two identical submissions are two
requests, and both are sent. The API is not guaranteed to be idempotent: "add 5 gold"
performed five times must yield 25, not 5, and the SDK cannot tell that apart from a
harmless repeated read.

## The transport

The SDK never calls the network directly. Everything goes through `IHttpTransport`, and the
default implementation is libcurl.

### The interface

Three methods, and the contract is **pollable, not blocking**:

```cpp
class IHttpTransport {
public:
    struct Request {
        std::string method;
        std::string url;
        HeaderMap   headers;
        std::string body;
        int         timeout_seconds = 300;
    };

    struct Response {
        int         status_code = 0;   // 0 = no response arrived at all
        HeaderMap   headers;
        std::string body;              // bytes, exactly as received
    };

    using TransferId = std::uint64_t;

    virtual TransferId Start(const Request& request) = 0;
    virtual bool Poll(TransferId id, Response& out) = 0;
    virtual void Cancel(TransferId id) = 0;
};
```

`Start` returns immediately with a handle. `Poll` reports completion without waiting and is
called once per `Update()`. `Cancel` releases the transfer, and must be safe to call for one
that already finished.

A transport that blocks in `Poll` stalls the host's frame for the length of the transfer,
which is the one thing a game SDK must never do.

### Building with libcurl

libcurl is **optional**. It is on by default, but a missing curl is a status message rather
than a configure error:

```sh
cmake -S . -B build -DLOOTLOCKER_WITH_CURL=OFF   # never look for curl
```

When curl is found, `LOOTLOCKER_HAVE_CURL` is defined publicly and `CurlHttpTransport` is
declared in `<lootlocker/curl_http_transport.h>`. When it is not, that header declares
nothing and the SDK still builds — you supply your own transport.

CI installs curl on all three targets, so the default path is compiled rather than being
dead code.

### Overriding it

Implement the interface, own it with a `shared_ptr`, and hand the SDK a `weak_ptr`:

```cpp
class MyTransport : public lootlocker::IHttpTransport {
    // Start / Poll / Cancel
};

auto transport = std::make_shared<MyTransport>();

lootlocker::Config config;
config.transport = transport;   // weak_ptr: you own it, the SDK observes it
```

The SDK uses it for every request. This is the seam a console uses: Sony and Nintendo ship
their own HTTP stacks, and curl needs porting to them.

**Why `weak_ptr` and not a raw pointer.** The adopter owns the object; the SDK only
observes it. A raw pointer would express that in a comment and nothing more, so an object
destroyed before the client would be a dangling dereference. With a `weak_ptr` the SDK
`lock()`s it per use, and if the object is gone it logs the fact and fails the call
gracefully instead of reading freed memory.

**The cost:** an injected object must be owned by a `shared_ptr`. A stack object cannot be
observed this way. That is the price of the safety, and it is why the example above
allocates.

`FakeHttpTransport` in `<lootlocker/fake_http_transport.h>` is a scriptable implementation
for tests. Transfers complete only when you say so, which makes the pollable contract
assertable without sleeping.

### One caveat: DNS

curl's default resolver is synchronous, so the first `Poll` for a hostname that is not
already cached can block for the length of the lookup. The transfer itself is pollable; the
name resolution is not. If that matters, resolve once at startup or inject a transport whose
resolver is asynchronous.

## JSON

The SDK parses JSON with [nlohmann/json](https://github.com/nlohmann/json), vendored as a
submodule pinned to `v3.11.3` and included by exactly one translation unit. **You never see
it.** No public header includes it, and no public type names it.

That is deliberate: it keeps our parser choice out of the API, so changing it later is not a
breaking change for you.

### Reading a field the SDK does not model

Use `RawBody()`. Every result carries the response body exactly as the server sent it, on
success and on failure, so you can parse it with whatever you already have:

```cpp
lootlocker::broadcasts::List(ll, [](lootlocker::Result<std::vector<lootlocker::Broadcast>> r) {
    if (!r.Ok()) {
        // The body is here too, so you can see what the server actually said.
        my_log(r.RawBody());
        return;
    }
    my_own_parser(r.RawBody());   // your parser, your types
});
```

This is the intended answer to "the SDK is missing a field". It is not a workaround.

### Custom decoding for a type

If you need a field decoded differently from the default, write an ADL `from_json` next to
your type. It takes a `lootlocker::JsonValue`, which is a parser-independent handle — it
exposes the shape of the value and nothing about which library produced it:

```cpp
namespace my_game {

struct PlayerStats {
    std::uint64_t total_score = 0;
    std::string   rank;
};

lootlocker::Result<lootlocker::Empty> from_json(const lootlocker::JsonValue& j,
                                                PlayerStats& out)
{
    // `IsUnsigned()` matters: reading a uint64 through AsDouble() loses precision above
    // 2^53, which is the one hazard the JSON spike found.
    if (!j["total_score"].IsUnsigned()) {
        lootlocker::Error e;
        e.code    = lootlocker::ErrorCode::MalformedResponse;
        e.message = "total_score is missing or not an unsigned integer";
        return lootlocker::Result<lootlocker::Empty>::Fail(e);
    }
    out.total_score = j["total_score"].AsUInt64();

    // Every field is validated, not just the numeric one. `AsString()` on a missing or
    // non-string value returns an empty string, so assigning it unchecked would turn "the
    // server did not send this" into "the server sent an empty string" — the silent default
    // the error model exists to prevent.
    if (j["rank"].GetKind() != lootlocker::JsonValue::Kind::String) {
        lootlocker::Error e;
        e.code    = lootlocker::ErrorCode::MalformedResponse;
        e.message = "rank is missing or not a string";
        return lootlocker::Result<lootlocker::Empty>::Fail(e);
    }
    out.rank = j["rank"].AsString();

    return lootlocker::Result<lootlocker::Empty>::Ok(lootlocker::Empty());
}

} // namespace my_game
```

**`from_json` returns a `Result`, not `void`.** That is what lets a decoder say "this field
was absent" instead of silently defaulting it to zero — the failure mode the SDK's error
model exists to prevent. A decoder that has nothing to reject returns `Ok(Empty())`.

`JsonValue` gives you `GetKind()`, `IsInteger()`, `IsUnsigned()`, the typed `As*` reads,
`operator[]`, `HasKey()`, `Size()` and `At()`. An absent key is `Kind::Invalid`, which is
distinct from `Kind::Null` — so "the field is missing" and "the field is null" are different
answers, and you can tell them apart.

### Writing a request body

A request type with an ADL `to_json` sends its serialised form as the body, with
`Content-Type: application/json` set for you. A type without one sends no body, which is
what every GET does.

```cpp
namespace my_game {

struct SessionOptionals {
    std::string timezone;
    std::string player_name;
    bool        auto_create_profile = true;
};

void to_json(lootlocker::JsonBuilder& b, const SessionOptionals& in)
{
    b.Set("timezone", in.timezone);
    b.Set("player_name", in.player_name);
    b.Set("auto_create_profile", in.auto_create_profile);
}

struct GuestSessionRequest {
    std::string      game_key;
    std::string      game_version;
    std::string      player_identifier;
    SessionOptionals optionals;
};

void to_json(lootlocker::JsonBuilder& b, const GuestSessionRequest& in)
{
    b.Set("game_key", in.game_key);
    b.Set("game_version", in.game_version);
    b.Set("player_identifier", in.player_identifier);
    b.Set("optionals", in.optionals);   // nested, through its own to_json
}

} // namespace my_game
```

`JsonBuilder` gives you `Set()` for strings, integers, unsigned integers, doubles and bools,
`SetNull()` for an explicit JSON null, and a `Set()` overload that takes any type with its
own `to_json`. Keys serialise in sorted order, because a JSON object is unordered by
definition.

`JsonBuilder` is a separate type from `JsonValue` on purpose: `JsonValue` is a read handle
over a shared, immutable document, which is what makes it cheap to pass around and safe to
hold across a callback. Writing needs its own type.

### There is no parser seam

Unlike the transport or the state writer, the JSON parser is not injectable. It is internal
machinery rather than an ecosystem-docking point: every adopter needs it, none needs a
different one, and a seam with no consumer is a seam nobody maintains. If you need a
different parser, `RawBody()` gives you the bytes.

## What is in v1

Guest, white label and Steam authentication, plus broadcasts. Presence, offline mode,
caching and the admin API are explicitly out of scope.

## Platforms

Windows, Linux and macOS are verified by CI on every pull request.

**Console support is not verified and must not be claimed.** The PS5 and Switch toolchains
need devkits, which we do not have. The SDK is designed for them — the transport is an
injectable interface precisely so a console can supply its own — but "designed for" is not
"tested on".

## Documentation

The design decisions, the risk register and the public API sketch live in the index repo:

- [`specs-library/specs/discipline/sdk/cpp-sdk/README.md`](https://github.com/lootlocker/index/blob/main/specs-library/specs/discipline/sdk/cpp-sdk/README.md) — the spec
- [`specs-library/specs/discipline/sdk/cpp-sdk/api-sketch.md`](https://github.com/lootlocker/index/blob/main/specs-library/specs/discipline/sdk/cpp-sdk/api-sketch.md) — the public surface

## Contributing

Feature branches come off `dev` and are rebased onto it, never merged into it. `main` is
releases only. See [`AGENTS.md`](AGENTS.md) for the conventions this codebase enforces.

## Licence

MIT. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).

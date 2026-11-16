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
- **A non-idempotent request whose response was lost is never retried.** A POST that timed
  out may have succeeded, and retrying it can double-grant. A 5xx on a POST *is* retried,
  because the server told us it failed. This is set per request with
  `PendingRequest::idempotent`, which defaults to `true` — every write must opt out
  deliberately.

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

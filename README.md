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

Implement the interface and hand it to `Config`:

```cpp
class MyTransport : public lootlocker::IHttpTransport {
    // Start / Poll / Cancel
};

MyTransport transport;

lootlocker::Config config;
config.transport = &transport;   // not owned; must outlive the client
```

The SDK uses it for every request. This is the seam a console uses: Sony and Nintendo ship
their own HTTP stacks, and curl needs porting to them.

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

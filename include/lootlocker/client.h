#pragma once

// The client (D5, D20). The only object an adopter owns.
//
// It is not a singleton, it starts no thread, and it exposes no feature methods: modules
// are namespaces of free functions that take it (D19). Constructing one starts nothing;
// requests move only when someone calls `Update()`.

#include <lootlocker/config.h>
#include <lootlocker/result.h>

#include <memory>
#include <string>

namespace lootlocker {

class Client;
class StateWriter;

namespace core {
class Executor;

// The internal access point a module uses to reach the queue.
//
// Declared here rather than in a `src/` header because a module target has to be able to
// name it, and a module may include `core`'s internals (D18). An adopter cannot use it:
// `src/` is not on their include path, so the definition is unreachable even though the
// declaration is visible.
//
// This is the seam `core::Send<Definition>()` is built on (T13). It is not test
// scaffolding.
Executor& GetExecutor(Client& client);
} // namespace core

class Client {
public:
    // Validates the config and throws nothing: a bad config is reported through
    // `ConfigError()`, because a constructor cannot return a `Result` and the SDK does not
    // throw (D7).
    //
    // A client with an invalid config is inert: `Update()` returns false, every request is
    // rejected with `InvalidConfig`, and nothing is sent. That is deliberate - a game that
    // ships a bad config gets a clear error in its log rather than a crash at startup.
    explicit Client(const Config& config);
    ~Client();

    Client(const Client&)            = delete;   // one owner, no copies
    Client& operator=(const Client&) = delete;

    // Drives the queue. Returns false when nothing is in flight and nothing is queued.
    //
    // Does a frame's worth of work, not one item: it polls, retries, promotes and delivers
    // in one call. Callbacks fire here, on the calling thread (D5).
    bool Update();

    // The config the client was built with, after validation.
    const Config& GetConfig() const;

    // Empty when the config was usable. Set when it was not, and the client is inert.
    const Error& ConfigError() const;

    // Replaces the transport. The client observes it through a `weak_ptr`, so the adopter
    // keeps ownership and a transport that dies is detected rather than dereferenced.
    //
    // Returns false when the replacement was refused because requests are still in flight.
    // An in-flight transfer id is meaningful only to the transport that issued it, so
    // swapping underneath would strand the old transfer. Drain first, then replace.
    bool SetTransport(std::weak_ptr<IHttpTransport> transport);

    // The transport in use, for a test or a wrapper that needs to inspect it. Empty when
    // the client has none, which happens when curl was not built in and nothing was
    // injected.
    std::weak_ptr<IHttpTransport> GetTransport() const;

    // A per-client identifier, sent as `LL-Instance-Identifier`.
    //
    // Generated once when the client is constructed and stable for its lifetime, so the
    // backend can tell two clients in one process apart (D20) and a support ticket can be
    // tied to a session. Not a security token: it identifies a client instance, nothing
    // more.
    const std::string& GetInstanceIdentifier() const;

    // Replaces the state writer. The client observes it through a `weak_ptr`, so the
    // adopter keeps ownership.
    //
    // Unlike `SetTransport`, this is allowed at any time: a state writer has no in-flight
    // state to strand, and swapping one mid-session is a legitimate thing for a game to do
    // when the player signs in to a platform account.
    //
    // An empty or dead `weak_ptr` falls back to the default `FileStateWriter`.
    void SetStateWriter(std::weak_ptr<StateWriter> writer);

    // The state writer in use. Never empty: the client always has one, defaulting to
    // `FileStateWriter`.
    std::weak_ptr<StateWriter> GetStateWriter() const;

private:
    // The correct form: the function is declared in `core` above, so the friend
    // declaration names it qualified rather than declaring a new one in this scope.
    friend core::Executor& core::GetExecutor(Client& client);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lootlocker

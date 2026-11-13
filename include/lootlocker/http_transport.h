#pragma once

// The transport seam (D3).
//
// The contract is POLLABLE, not blocking. `Update()` never waits on the network: it starts
// transfers, polls them, and collects the ones that finished. A transport that blocks
// would stall the host's frame, which is the one thing a game SDK must never do.
//
// The default implementation is libcurl over `curl_multi`, which is already this shape. A
// console injects its own, because Sony and Nintendo ship their own HTTP stacks and curl
// needs porting to them. A host with only a blocking client wraps it in a worker thread
// behind this interface, which is the right place for that decision.

#include <lootlocker/header_map.h>

#include <cstdint>
#include <string>

namespace lootlocker {

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    struct Request {
        std::string method;
        std::string url;
        HeaderMap   headers;
        std::string body;

        // A whole-request budget, not a per-operation one. 0 means "no timeout", which is
        // almost never what a game wants; the executor sets a real value from QueueConfig.
        int timeout_seconds = 300;
    };

    struct Response {
        // 0 when no response arrived at all, which is how a transport failure is
        // distinguished from a 4xx.
        int status_code = 0;

        HeaderMap headers;

        // The bytes exactly as received, never decoded or re-encoded (D23). A body that is
        // not valid UTF-8 must survive to the caller so the error can be reported rather
        // than silently mangled.
        std::string body;
    };

    // A handle for one in-flight transfer. Opaque to the SDK.
    using TransferId = std::uint64_t;

    // Starts a transfer and returns immediately. Must not block.
    //
    // Returns 0 when the transfer could not be started at all, which the executor treats
    // as a transport failure rather than a request that is still running.
    virtual TransferId Start(const Request& request) = 0;

    // Non-blocking. Returns true when the transfer has finished, filling `out`.
    // Returns false while it is still running.
    //
    // Called once per `Update()`. A transport that has nothing to report returns false
    // immediately; it must never wait for the network here.
    virtual bool Poll(TransferId id, Response& out) = 0;

    // Cancels and releases. Called on timeout, on client destruction, and after `Poll`
    // has returned true.
    //
    // Must be safe to call for an id that has already finished, because the executor
    // cannot always know which happened first.
    virtual void Cancel(TransferId id) = 0;
};

} // namespace lootlocker

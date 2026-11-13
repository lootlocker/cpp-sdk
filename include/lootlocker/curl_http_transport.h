#pragma once

// The libcurl transport (D3).
//
// Only compiled when libcurl is found, because a console adopter injects its own transport
// and must not be forced to build curl. `LOOTLOCKER_HAVE_CURL` is defined by CMake when
// the dependency is present; without it this header declares nothing and the SDK still
// builds.
//
// The implementation is `curl_multi`, which is already pollable: `Start` adds an easy
// handle to the multi handle, `Poll` drives it without waiting, and `Cancel` removes it.
// That is why D3 chose curl - the interface shape is the library's native shape rather
// than something bolted on.

#include <lootlocker/http_transport.h>

#if defined(LOOTLOCKER_HAVE_CURL)

#include <cstdint>
#include <map>
#include <string>

namespace lootlocker {

class CurlHttpTransport : public IHttpTransport {
public:
    CurlHttpTransport();
    ~CurlHttpTransport() override;

    // Not copyable: it owns a curl multi handle and per-transfer state.
    CurlHttpTransport(const CurlHttpTransport&) = delete;
    CurlHttpTransport& operator=(const CurlHttpTransport&) = delete;

    TransferId Start(const Request& request) override;
    bool       Poll(TransferId id, Response& out) override;
    void       Cancel(TransferId id) override;

    // True when a transfer died at the curl level rather than returning a status. Exposed
    // so a test can distinguish "no response yet" from "the transfer failed".
    bool HadTransportError() const { return m_had_transport_error; }

private:
    struct Transfer;

    // Drives the multi handle once. Called from `Poll`, never blocks.
    void Pump();

    void*      m_multi = nullptr;   // CURLM*, void* so curl headers stay out of this header
    TransferId m_next_id = 0;
    std::map<TransferId, Transfer*> m_transfers;
    bool       m_had_transport_error = false;
};

} // namespace lootlocker

#endif // LOOTLOCKER_HAVE_CURL

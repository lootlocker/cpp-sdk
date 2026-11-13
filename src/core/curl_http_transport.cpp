// The libcurl transport (D3). Only compiled when CMake found curl.
//
// `curl_multi` is already pollable, so this is a thin adapter rather than a workaround:
// Start adds an easy handle, Poll drives the multi handle once without waiting, Cancel
// removes it. The one thing that needs care is that curl's completion notification is
// separate from the per-handle result, so a transfer is only "done" once both have
// happened.

#include <lootlocker/curl_http_transport.h>

#if defined(LOOTLOCKER_HAVE_CURL)

#include <curl/curl.h>

#include <cstddef>
#include <cstring>
#include <string>

namespace lootlocker {

namespace {

// curl writes the body in chunks; append rather than overwrite.
std::size_t WriteBody(char* data, std::size_t size, std::size_t count, void* user)
{
    const std::size_t bytes = size * count;
    static_cast<std::string*>(user)->append(data, bytes);
    return bytes;
}

// curl hands headers one line at a time, including the status line and the blank line.
std::size_t WriteHeader(char* data, std::size_t size, std::size_t count, void* user)
{
    const std::size_t bytes = size * count;
    HeaderMap* headers = static_cast<HeaderMap*>(user);

    std::string line(data, bytes);

    // The status line ("HTTP/1.1 200 OK") and the terminating blank line are not headers.
    if (line.compare(0, 5, "HTTP/") == 0) {
        return bytes;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return bytes;
    }

    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);

    // Trim the leading space and the trailing CRLF curl includes.
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }

    headers->Set(name, value.substr(begin, end - begin));
    return bytes;
}

} // namespace

struct CurlHttpTransport::Transfer {
    TransferId  id = 0;
    CURL*       easy = nullptr;
    curl_slist* request_headers = nullptr;

    // The request body, owned for the whole transfer. curl borrows the pointer given to
    // CURLOPT_POSTFIELDS, and the caller's Request may be gone by the time curl sends it.
    std::string owned_body;

    Response response;
    bool     finished = false;   // curl said the transfer is done
    bool     failed = false;     // curl-level failure, no usable response
    char     error[CURL_ERROR_SIZE] = {0};
};

namespace {

// libcurl's global state is process-wide, so it must be initialised once and torn down
// only when the last transport is gone. Doing it per instance means destroying one
// transport invalidates every other one's handles - a use-after-free that only shows up
// when two clients exist, which is exactly the case a test would miss.
//
// A plain int rather than std::atomic: the SDK is single-threaded by contract (D5), and
// transports are constructed and destroyed on the thread that calls Update().
int g_curl_users = 0;

void AcquireCurl()
{
    if (g_curl_users == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ++g_curl_users;
}

void ReleaseCurl()
{
    if (g_curl_users > 0) {
        --g_curl_users;
        if (g_curl_users == 0) {
            curl_global_cleanup();
        }
    }
}

} // namespace

CurlHttpTransport::CurlHttpTransport()
{
    AcquireCurl();
    m_multi = curl_multi_init();
}

CurlHttpTransport::~CurlHttpTransport()
{
    for (auto& entry : m_transfers) {
        Transfer* transfer = entry.second;
        if (transfer->easy != nullptr) {
            curl_multi_remove_handle(static_cast<CURLM*>(m_multi), transfer->easy);
            curl_easy_cleanup(transfer->easy);
        }
        if (transfer->request_headers != nullptr) {
            curl_slist_free_all(transfer->request_headers);
        }
        delete transfer;
    }
    m_transfers.clear();

    if (m_multi != nullptr) {
        curl_multi_cleanup(static_cast<CURLM*>(m_multi));
        m_multi = nullptr;
    }

    ReleaseCurl();
}

IHttpTransport::TransferId CurlHttpTransport::Start(const Request& request)
{
    if (m_multi == nullptr) {
        return 0;
    }

    Transfer* transfer = new Transfer();
    transfer->id = ++m_next_id;
    transfer->easy = curl_easy_init();
    if (transfer->easy == nullptr) {
        delete transfer;
        return 0;
    }

    curl_easy_setopt(transfer->easy, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEDATA, &transfer->response.body);
    curl_easy_setopt(transfer->easy, CURLOPT_HEADERFUNCTION, WriteHeader);
    curl_easy_setopt(transfer->easy, CURLOPT_HEADERDATA, &transfer->response.headers);
    curl_easy_setopt(transfer->easy, CURLOPT_ERRORBUFFER, transfer->error);
    curl_easy_setopt(transfer->easy, CURLOPT_NOSIGNAL, 1L);

    // The non-blocking promise covers DNS too, and curl's default resolver is
    // synchronous: the first Poll for a hostname would block the caller of Update() for
    // the length of the lookup. CURLOPT_DNS_SERVERS is not enough - it still resolves
    // synchronously. The only honest options are an async resolver build or resolving off
    // the update thread, and neither is available here.
    //
    // So the contract is narrowed rather than quietly broken: this transport is pollable
    // for the transfer, and the FIRST poll for a new host may block on DNS. The header
    // says so. A game that cannot tolerate that resolves once at startup, or injects a
    // transport whose resolver is async.
    curl_easy_setopt(transfer->easy, CURLOPT_DNS_CACHE_TIMEOUT, 60L);

    // A whole-request budget. 0 means no timeout, which curl expresses as 0L.
    curl_easy_setopt(transfer->easy, CURLOPT_TIMEOUT,
                     static_cast<long>(request.timeout_seconds));

    // Redirects are followed, because the API may move a domain. A cap prevents a loop.
    curl_easy_setopt(transfer->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_MAXREDIRS, 5L);

    // The body must outlive `Start`. CURLOPT_POSTFIELDS borrows the pointer, and the
    // caller is free to destroy its Request the moment Start returns - so the transfer
    // keeps its own copy and points curl at that. CURLOPT_COPYPOSTFIELDS would also work,
    // but it copies again internally and gives no way to free the copy deterministically.
    if (request.method == "POST" || request.method == "PUT") {
        transfer->owned_body = request.body;
        if (request.method == "POST") {
            curl_easy_setopt(transfer->easy, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(transfer->easy, CURLOPT_CUSTOMREQUEST, "PUT");
        }
        curl_easy_setopt(transfer->easy, CURLOPT_POSTFIELDS, transfer->owned_body.c_str());
        curl_easy_setopt(transfer->easy, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(transfer->owned_body.size()));
    } else if (request.method == "DELETE") {
        curl_easy_setopt(transfer->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (request.method != "GET" && !request.method.empty()) {
        // A custom method may still carry a body, so it gets the same treatment.
        transfer->owned_body = request.body;
        curl_easy_setopt(transfer->easy, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!transfer->owned_body.empty()) {
            curl_easy_setopt(transfer->easy, CURLOPT_POSTFIELDS,
                             transfer->owned_body.c_str());
            curl_easy_setopt(transfer->easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(transfer->owned_body.size()));
        }
    }

    for (const HeaderMap::Entry& header : request.headers.Entries()) {
        const std::string line = header.name + ": " + header.value;
        transfer->request_headers =
            curl_slist_append(transfer->request_headers, line.c_str());
    }
    if (transfer->request_headers != nullptr) {
        curl_easy_setopt(transfer->easy, CURLOPT_HTTPHEADER, transfer->request_headers);
    }

    if (curl_multi_add_handle(static_cast<CURLM*>(m_multi), transfer->easy) != CURLM_OK) {
        curl_easy_cleanup(transfer->easy);
        if (transfer->request_headers != nullptr) {
            curl_slist_free_all(transfer->request_headers);
        }
        delete transfer;
        return 0;
    }

    m_transfers[transfer->id] = transfer;
    return transfer->id;
}

void CurlHttpTransport::Pump()
{
    if (m_multi == nullptr) {
        return;
    }

    CURLM* multi = static_cast<CURLM*>(m_multi);

    int running = 0;
    // A zero timeout is what makes this non-blocking: curl does whatever work is ready and
    // returns immediately rather than waiting for the socket.
    curl_multi_perform(multi, &running);

    int queued = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &queued)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }

        CURL* easy = message->easy_handle;
        for (auto& entry : m_transfers) {
            Transfer* transfer = entry.second;
            if (transfer->easy != easy) {
                continue;
            }

            transfer->finished = true;
            if (message->data.result != CURLE_OK) {
                transfer->failed = true;
                m_had_transport_error = true;
            } else {
                long status = 0;
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
                transfer->response.status_code = static_cast<int>(status);
            }
            break;
        }
    }
}

bool CurlHttpTransport::Poll(TransferId id, Response& out)
{
    const auto it = m_transfers.find(id);
    if (it == m_transfers.end()) {
        return false;
    }

    Transfer* transfer = it->second;
    if (!transfer->finished) {
        Pump();
    }
    if (!transfer->finished) {
        return false;
    }

    // A curl-level failure is reported as "no response", which is how the interface
    // distinguishes a transport failure from a 4xx.
    out = transfer->failed ? Response() : transfer->response;
    return true;
}

void CurlHttpTransport::Cancel(TransferId id)
{
    const auto it = m_transfers.find(id);
    if (it == m_transfers.end()) {
        return;
    }

    Transfer* transfer = it->second;
    if (transfer->easy != nullptr) {
        curl_multi_remove_handle(static_cast<CURLM*>(m_multi), transfer->easy);
        curl_easy_cleanup(transfer->easy);
        transfer->easy = nullptr;
    }
    if (transfer->request_headers != nullptr) {
        curl_slist_free_all(transfer->request_headers);
        transfer->request_headers = nullptr;
    }

    delete transfer;
    m_transfers.erase(it);
}

} // namespace lootlocker

#endif // LOOTLOCKER_HAVE_CURL

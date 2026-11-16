// A minimal loopback HTTP server for the end-to-end transport test (T11).
//
// Deliberately hand-rolled on sockets rather than a library: the point of the test is to
// prove the SDK's real transport works against a real socket, and pulling in a server
// framework would add a dependency to the test suite for one file. It speaks just enough
// HTTP/1.1 for the SDK's own requests: one request per connection, `Content-Length` bodies,
// `Connection: close`.
//
// It runs on its own thread and records what it received, so the test can assert on the
// request line, headers and body that actually arrived on the wire - which is the whole
// reason this exists. Asserting on what the SDK *thinks* it sent would prove nothing.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace testing {

// One request as it arrived on the wire.
struct ReceivedRequest {
    std::string method;
    std::string target;   // path plus query, exactly as sent
    std::string version;

    // Header lines in the order they arrived, name and value split on the first colon and
    // trimmed. Kept as a vector rather than a map so the test can assert on order and on
    // duplicates, which a map would hide.
    std::vector<std::pair<std::string, std::string>> headers;

    std::string body;

    // Case-insensitive header lookup. Empty when absent.
    std::string Header(const std::string& name) const
    {
        for (const auto& header : headers) {
            if (header.first.size() != name.size()) {
                continue;
            }
            bool same = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                const char a = header.first[i];
                const char b = name[i];
                const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
                const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
                if (la != lb) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return header.second;
            }
        }
        return std::string();
    }
};

// A canned response, one per request, in order. The last one repeats if more requests
// arrive than were scripted.
struct ScriptedResponse {
    int         status_code = 200;
    std::string reason = "OK";
    std::string content_type = "application/json";
    std::string body;

    // Extra headers, e.g. `Retry-After`.
    std::vector<std::pair<std::string, std::string>> headers;
};

class LoopbackServer {
public:
    LoopbackServer() = default;
    ~LoopbackServer() { Stop(); }

    LoopbackServer(const LoopbackServer&)            = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    // Binds to 127.0.0.1 on an ephemeral port and starts accepting. Returns false when the
    // socket could not be created, which the test reports rather than silently passing.
    bool Start()
    {
#if defined(_WIN32)
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        m_wsa_started = true;
#endif

        m_listen = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, 0));
        if (m_listen < 0) {
            return false;
        }

        // SO_REUSEADDR so a rerun in the same process does not fail on a lingering socket.
        int yes = 1;
        ::setsockopt(static_cast<int>(m_listen), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        // Not `::htonl`: on macOS these are macros (`__DARWIN_OSSwapInt32`), and a macro
        // expansion cannot be qualified with `::`.
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;   // ephemeral

        if (::bind(static_cast<int>(m_listen), reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0) {
            CloseListen();
            return false;
        }

        // Read back the port the OS chose.
        sockaddr_in bound{};
#if defined(_WIN32)
        int length = sizeof(bound);
#else
        socklen_t length = sizeof(bound);
#endif
        if (::getsockname(static_cast<int>(m_listen), reinterpret_cast<sockaddr*>(&bound),
                          &length) != 0) {
            CloseListen();
            return false;
        }
        m_port = ntohs(bound.sin_port);

        if (::listen(static_cast<int>(m_listen), 8) != 0) {
            CloseListen();
            return false;
        }

        m_running = true;
        m_thread = std::thread([this]() { Run(); });
        return true;
    }

    void Stop()
    {
        if (!m_running.exchange(false)) {
            return;
        }

        // `shutdown` before `close`, and this is not belt-and-braces: on Linux, closing a
        // listening socket does NOT unblock a thread already blocked in `accept()`, so
        // `join()` below would hang for ever. Windows and macOS do unblock it, which is
        // exactly why this only showed up on one of the three CI targets.
        //
        // `shutdown` on a listening socket makes the pending `accept` return, which is the
        // portable way to wake it.
        if (m_listen >= 0) {
#if defined(_WIN32)
            ::shutdown(static_cast<SOCKET>(m_listen), SD_BOTH);
#else
            ::shutdown(static_cast<int>(m_listen), SHUT_RDWR);
#endif
        }
        CloseListen();

        if (m_thread.joinable()) {
            m_thread.join();
        }

#if defined(_WIN32)
        if (m_wsa_started) {
            WSACleanup();
            m_wsa_started = false;
        }
#endif
    }

    // The base URL to hand the SDK, e.g. "http://127.0.0.1:54321".
    std::string BaseUrl() const
    {
        return "http://127.0.0.1:" + std::to_string(m_port);
    }

    int Port() const { return m_port; }

    // Queue the responses to serve, in order.
    void Script(std::vector<ScriptedResponse> responses)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_script = std::move(responses);
    }

    // What arrived, in order.
    std::vector<ReceivedRequest> Received() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_received;
    }

    std::size_t ReceivedCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_received.size();
    }

private:
    void CloseListen()
    {
        if (m_listen >= 0) {
#if defined(_WIN32)
            ::closesocket(static_cast<SOCKET>(m_listen));
#else
            ::close(static_cast<int>(m_listen));
#endif
            m_listen = -1;
        }
    }

    void Run()
    {
        while (m_running.load()) {
            const std::intptr_t client =
                static_cast<std::intptr_t>(::accept(static_cast<int>(m_listen), nullptr, nullptr));
            if (client < 0) {
                // Either we are shutting down or the accept failed; both mean stop.
                break;
            }

            HandleOne(client);

#if defined(_WIN32)
            ::closesocket(static_cast<SOCKET>(client));
#else
            ::close(static_cast<int>(client));
#endif
        }
    }

    // Read until the header terminator, then read exactly Content-Length bytes.
    static bool ReadRequest(std::intptr_t client, ReceivedRequest& out)
    {
        std::string buffer;
        char chunk[4096];

        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const int got = ::recv(static_cast<int>(client), chunk, sizeof(chunk), 0);
            if (got <= 0) {
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
            header_end = buffer.find("\r\n\r\n");
        }

        const std::string head = buffer.substr(0, header_end);
        std::string body = buffer.substr(header_end + 4);

        // Request line.
        const std::size_t first_line_end = head.find("\r\n");
        const std::string request_line =
            first_line_end == std::string::npos ? head : head.substr(0, first_line_end);

        const std::size_t sp1 = request_line.find(' ');
        const std::size_t sp2 =
            sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            return false;
        }
        out.method = request_line.substr(0, sp1);
        out.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        out.version = request_line.substr(sp2 + 1);

        // Headers.
        std::size_t pos = first_line_end == std::string::npos ? head.size() : first_line_end + 2;
        while (pos < head.size()) {
            const std::size_t line_end = head.find("\r\n", pos);
            const std::string line =
                line_end == std::string::npos ? head.substr(pos) : head.substr(pos, line_end - pos);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                std::size_t begin = 0;
                while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
                    ++begin;
                }
                std::size_t end = value.size();
                while (end > begin && (value[end - 1] == '\r' || value[end - 1] == '\n')) {
                    --end;
                }
                out.headers.emplace_back(name, value.substr(begin, end - begin));
            }
            if (line_end == std::string::npos) {
                break;
            }
            pos = line_end + 2;
        }

        // Body, by Content-Length. A request with no body has none.
        std::size_t expected = 0;
        const std::string length = out.Header("Content-Length");
        if (!length.empty()) {
            for (const char c : length) {
                if (c < '0' || c > '9') {
                    break;
                }
                expected = expected * 10 + static_cast<std::size_t>(c - '0');
            }
        }

        while (body.size() < expected) {
            const int got = ::recv(static_cast<int>(client), chunk, sizeof(chunk), 0);
            if (got <= 0) {
                break;
            }
            body.append(chunk, static_cast<std::size_t>(got));
        }

        out.body = body.substr(0, expected);
        return true;
    }

    void HandleOne(std::intptr_t client)
    {
        ReceivedRequest request;
        if (!ReadRequest(client, request)) {
            return;
        }

        ScriptedResponse response;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::size_t index = m_received.size();
            m_received.push_back(request);
            if (!m_script.empty()) {
                response = m_script[index < m_script.size() ? index : m_script.size() - 1];
            }
        }

        std::string out = "HTTP/1.1 " + std::to_string(response.status_code) + " "
                          + response.reason + "\r\n";
        if (!response.content_type.empty()) {
            out += "Content-Type: " + response.content_type + "\r\n";
        }
        out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        for (const auto& header : response.headers) {
            out += header.first + ": " + header.second + "\r\n";
        }
        // One request per connection, so the client never has to guess where the response
        // ends.
        out += "Connection: close\r\n\r\n";
        out += response.body;

        std::size_t sent = 0;
        while (sent < out.size()) {
            const int wrote = ::send(static_cast<int>(client), out.data() + sent,
                                     static_cast<int>(out.size() - sent), 0);
            if (wrote <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    std::intptr_t m_listen = -1;
    int           m_port = 0;

    // Windows-only: WSAStartup has to be paired with WSACleanup. Guarded rather than left
    // unguarded because clang warns about an unused private field, and a warning in the
    // test suite is a warning someone will eventually silence by deleting the field.
#if defined(_WIN32)
    bool m_wsa_started = false;
#endif

    std::atomic<bool> m_running{false};
    std::thread       m_thread;

    mutable std::mutex           m_mutex;
    std::vector<ScriptedResponse> m_script;
    std::vector<ReceivedRequest>  m_received;
};

} // namespace testing

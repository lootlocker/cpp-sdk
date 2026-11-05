#pragma once

// The error model (D7).
//
// No exceptions in the public API: a game build disables them, and an SDK that throws is
// unusable there. Every fallible operation returns a `Result<T>` carrying either a value
// or a structured `Error`, and the raw response body is carried on both.

#include <cstdint>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace lootlocker {

// Why an operation failed. `None` means it did not.
//
// These are deliberately coarse: an adopter branches on the *kind* of failure, not on
// every HTTP status. The status is in `Error::status_code` when there was a response.
enum class ErrorCode {
    None = 0,

    // Could not connect, DNS failure, TLS failure, timeout. No response arrived.
    Transport,

    // A response arrived and its status was >= 400.
    Http,

    // A response arrived and could not be decoded (D23). This is never a silent default:
    // a missing field is a decode error, not a zero.
    MalformedResponse,

    // 429 from the server, or the client-side rate limiter held the request.
    RateLimited,

    // Rejected before dispatch because the queue was full.
    QueueFull,

    // The client was destroyed, or the request was cancelled.
    Cancelled,

    // The configuration is unusable: missing domain_key, a game_version that is not
    // semver, and so on. Caught before any request is made.
    InvalidConfig,

    // The platform cannot do this. Example: refreshing a guest session.
    Unsupported,
};

// A human-readable name for a code, for logs. Never null.
const char* ToString(ErrorCode code);

// A structured failure.
//
// `message` is safe to log: it never contains a token, an email or a password (D26).
struct Error {
    ErrorCode   code = ErrorCode::None;

    // The HTTP status, or 0 when no response arrived.
    int status_code = 0;

    // Human-readable, safe to log.
    std::string message;

    // The API's own error code, e.g. "player_banned", when it sends one.
    std::string api_code;

    // For support. Also sent as the LL-Request-Id header.
    std::string request_id;

    // From the API body when present.
    std::string trace_id;

    // From a Retry-After header, in seconds. 0 when absent.
    int retry_after_seconds = 0;

    bool Ok() const { return code == ErrorCode::None; }
};

// A value or an error, never both.
//
// A value type: copyable when T is, movable when T is. It never throws, and `Value()` on
// a failed result is a precondition violation rather than an exception — the same
// contract as `std::optional::operator*`.
//
// `RawBody()` is populated on every result, success or failure (D7). Two uses: logging a
// response that decoded into the wrong shape, and reading a field the SDK does not model
// yet, so "the SDK is missing a field" is not a release-blocking bug.
//
// The value is stored in a union-like slot rather than as a plain `T` member, so a failed
// result does not construct a `T` and `T` does not have to be default-constructible.
template <typename T>
class Result {
public:
    // A successful result. `raw_body` is the response body exactly as received.
    static Result Ok(T value, std::string raw_body = std::string())
    {
        Result r;
        r.m_ok = true;
        new (static_cast<void*>(&r.m_value)) T(std::move(value));
        r.m_raw_body = std::move(raw_body);
        return r;
    }

    // A failed result. `raw_body` is whatever arrived, which may be empty.
    //
    // The type is spelled `lootlocker::Error` rather than `Error` because the accessor
    // below is also named `Error`, and a member function name shadows the type inside the
    // class scope. Qualifying is the fix that keeps the documented API (D7).
    static Result Fail(lootlocker::Error error, std::string raw_body = std::string())
    {
        Result r;
        r.m_ok = false;
        r.m_error = std::move(error);
        r.m_raw_body = std::move(raw_body);
        return r;
    }

    Result() = default;

    Result(const Result& other) : m_ok(other.m_ok), m_error(other.m_error), m_raw_body(other.m_raw_body)
    {
        if (m_ok) {
            new (static_cast<void*>(&m_value)) T(other.Value());
        }
    }

    Result(Result&& other) noexcept
        : m_ok(other.m_ok), m_error(std::move(other.m_error)), m_raw_body(std::move(other.m_raw_body))
    {
        if (m_ok) {
            new (static_cast<void*>(&m_value)) T(std::move(other.Value()));
        }
    }

    Result& operator=(const Result& other)
    {
        if (this != &other) {
            DestroyValue();
            m_ok = other.m_ok;
            m_error = other.m_error;
            m_raw_body = other.m_raw_body;
            if (m_ok) {
                new (static_cast<void*>(&m_value)) T(other.Value());
            }
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept
    {
        if (this != &other) {
            DestroyValue();
            m_ok = other.m_ok;
            m_error = std::move(other.m_error);
            m_raw_body = std::move(other.m_raw_body);
            if (m_ok) {
                new (static_cast<void*>(&m_value)) T(std::move(other.Value()));
            }
        }
        return *this;
    }

    ~Result() { DestroyValue(); }

    bool Ok() const { return m_ok; }
    explicit operator bool() const { return m_ok; }

    // Precondition: Ok().
    const T& Value() const { return *reinterpret_cast<const T*>(&m_value); }
    T& Value() { return *reinterpret_cast<T*>(&m_value); }

    // Precondition: !Ok().
    const lootlocker::Error& Error() const { return m_error; }

    // The response body exactly as the server sent it, on success and on failure.
    const std::string& RawBody() const { return m_raw_body; }

    // Move the body out, leaving RawBody() empty.
    //
    // The clear is explicit rather than relying on the moved-from string being empty: a
    // moved-from std::string is valid but *unspecified*, so the postcondition has to be
    // established rather than assumed.
    std::string TakeRawBody()
    {
        std::string taken = std::move(m_raw_body);
        m_raw_body.clear();
        return taken;
    }

private:
    void DestroyValue()
    {
        if (m_ok) {
            Value().~T();
            m_ok = false;
        }
    }

    bool              m_ok = false;
    lootlocker::Error m_error;
    std::string       m_raw_body;

    // Storage for T, constructed only when m_ok. Aligned and sized for T; never read
    // unless m_ok is true.
    typename std::aligned_storage<sizeof(T), alignof(T)>::type m_value;
};

// `Result<void>` is not a thing here: a `void` specialisation is a second code path
// through every generic helper for no gain. Use `Result<Empty>`.
struct Empty {};

} // namespace lootlocker





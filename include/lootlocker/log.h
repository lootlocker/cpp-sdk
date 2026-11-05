#pragma once

// Logging (D26).
//
// A `Logger` interface injected through `Config`, never a global. A static logger, which
// is what both engine SDKs do, cannot be redirected into a host's log pipeline and cannot
// be silenced per-client, so it is not an option here.
//
// The HTTP entry is the important part: method, URL, both bodies, status, duration and the
// request id. That is what makes a support ticket actionable.
//
// **Obfuscation happens before the sink is called** (D26, R28). A `Logger` implementation
// therefore never sees an unmasked `session_token`, an email or a password, so a logging
// implementation written by an adopter cannot leak one by accident.

#include <cstddef>
#include <cstdint>
#include <string>

namespace lootlocker {

// Ordered by increasing verbosity, which is what makes `level <= minimum` work.
//
// The set follows Unreal's, which is the most complete of the SDKs; Unity's is a subset.
enum class LogLevel {
    NoLogging = 0,
    Fatal,
    Error,
    Warning,
    Display,
    Log,
    Verbose,
    VeryVerbose,
};

// A human-readable name for a level, for log prefixes. Never null.
const char* ToString(LogLevel level);

namespace log {

// One HTTP exchange, as it appears in the log.
//
// A plain aggregate rather than a class with accessors, so it is cheap to build and trivial
// to inspect. Bodies are already obfuscated by the time a logger receives one.
struct HttpEntry {
    std::string method;
    std::string url;

    // The request body as sent, already obfuscated.
    std::string request_body;

    // The response body as received, already obfuscated. Populated on failure too, which
    // is the case the log is usually read for.
    std::string response_body;

    // 0 when no response arrived, which distinguishes a transport failure from a 4xx.
    int status_code = 0;

    // Milliseconds spent on the exchange, excluding time spent queued.
    std::int64_t duration_ms = 0;

    // Which attempt this was, 1-based. A retried request logs each attempt, and this is
    // what tells them apart in the file.
    int attempt = 1;

    // The id the call returned (D25, T11d). Empty when the exchange was not made on behalf
    // of an adopter call, such as an internal ping.
    std::string request_id;

    // The server's own correlation id, from the `LL-Trace-Id` response header. Empty when
    // absent. Support asks for this, so it is logged separately from our id.
    std::string trace_id;
};

// The SDK's own denied field names (D26, R28).
//
// An adopter may add keys but cannot remove ours: a configurable deny-list invites
// someone to turn it off and then file a bug about a leaked token.
const char* const* DeniedKeys();
std::size_t        DeniedKeyCount();

// Add a field name to the deny-list. Idempotent and case-insensitive.
//
// Not thread-safe, like the rest of the setup surface: call it before the first request,
// not from an `Update()` callback.
void AddDeniedKey(const std::string& key);

// Restore the deny-list to the built-in set. Intended for tests.
void ResetDeniedKeys();

bool IsDeniedKey(const std::string& key);

// Mask the values of denied keys in a JSON body, leaving everything else byte-identical.
//
// A scan and splice rather than a parse and re-serialise, so key order, whitespace and
// number formatting survive. That matters because a log is often compared against a
// report, and a re-serialised body is a different document.
//
// A body that is not valid JSON is returned unchanged rather than guessed at: masking is a
// best-effort defence, and rewriting a non-JSON body risks destroying the one artefact
// that would explain the failure.
//
// `visible_from_start` and `visible_from_end` keep that many characters at each end, which
// is what keeps a mask useful for "is this the same token as last time". A value no longer
// than `visible_from_start + visible_from_end + 4` is masked entirely, so a 4-character
// secret is not mostly revealed.
std::string ObfuscateJson(const std::string& body, std::size_t visible_from_start = 3,
                          std::size_t visible_from_end = 3);

// A single line describing an exchange, for a logger that only wants text.
std::string FormatHttpEntry(const HttpEntry& entry);

} // namespace log

// The injection point.
//
// Implementations must not throw (D7) and must not block: this is called on the thread that
// calls `Update()`, so a logger doing synchronous file I/O stalls the frame.
class Logger {
public:
    virtual ~Logger() = default;

    // A message. `level` is never `NoLogging`.
    virtual void Log(LogLevel level, const std::string& message) = 0;

    // One completed HTTP exchange.
    //
    // Not pure: an implementation that only wants text leaves it, and the base
    // implementation renders a summary through `Log`. Making it pure would force every
    // adopter to write the same summary.
    virtual void LogHttp(const log::HttpEntry& entry);
};

namespace log {

// The default logger: writes to stderr through `fprintf`, not iostreams (R13).
//
// There is deliberately no built-in file sink. It is a dozen lines for an adopter who
// wants one, and building it in would mean owning rotation, flush policy and a path
// convention on every platform — none of which belongs in an SDK. The interface is the
// extension point.
class StderrLogger : public Logger {
public:
    explicit StderrLogger(LogLevel minimum = LogLevel::Warning);

    void Log(LogLevel level, const std::string& message) override;

    LogLevel Minimum() const { return m_minimum; }
    void     SetMinimum(LogLevel minimum) { m_minimum = minimum; }

private:
    LogLevel m_minimum;
};

} // namespace log

} // namespace lootlocker

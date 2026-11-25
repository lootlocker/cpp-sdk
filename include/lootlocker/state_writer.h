#pragma once

// Persistence (D11, R18). The seam that lets the SDK remember a session between launches.
//
// The problem this solves: guest auth's whole point is that a player keeps the same account
// across restarts. That requires remembering `player_identifier`, and there is no portable
// way to write a file on every platform this SDK targets — a console has its own save-data
// API, an engine has its own settings store, and a plain application has a filesystem.
//
// So the SDK defines the interface and ships two implementations: `FileStateWriter`, which
// writes one file next to the game, and `NullStateWriter`, which writes nothing. An engine
// or a console injects its own through `Config::state_writer` or `Client::SetStateWriter`.
//
// **Typed accessors rather than strings-only.** A store that only speaks strings forces
// every adopter to write the same conversions and get the same rounding wrong — and the
// `player_id` case is exactly where that bites, because a `uint64` above `INT64_MAX` does
// not survive a round trip through a `double` or a signed integer.
//
// **Implementations must not throw** (D7). A failed write is reported by returning false
// from the getter or by silently doing nothing from the setter; the SDK treats persistence
// as best-effort and never fails a request because a save failed.

#include <cstdint>
#include <memory>
#include <string>

namespace lootlocker {

class StateWriter {
public:
    virtual ~StateWriter() = default;

    // Each getter returns false and leaves `out` untouched when the key is absent or holds
    // a value of a different type. It does not convert: a key written as a string is not
    // readable as an integer, because guessing is how a corrupt store becomes a wrong
    // player id.
    virtual bool GetString(const std::string& key, std::string& out) = 0;
    virtual void SetString(const std::string& key, const std::string& value) = 0;

    virtual bool GetInt64(const std::string& key, std::int64_t& out) = 0;
    virtual void SetInt64(const std::string& key, std::int64_t value) = 0;

    // Unsigned, because `player_id` is a `uint64` on the wire and a valid id above
    // `INT64_MAX` must round-trip exactly (D4).
    virtual bool GetUInt64(const std::string& key, std::uint64_t& out) = 0;
    virtual void SetUInt64(const std::string& key, std::uint64_t value) = 0;

    virtual bool GetDouble(const std::string& key, double& out) = 0;
    virtual void SetDouble(const std::string& key, double value) = 0;

    virtual bool GetBool(const std::string& key, bool& out) = 0;
    virtual void SetBool(const std::string& key, bool value) = 0;

    virtual void Erase(const std::string& key) = 0;
    virtual bool Has(const std::string& key) = 0;
};

// Writes nothing, and reports every key as absent.
//
// For a game that would rather re-authenticate every launch, and for a test that wants the
// no-persistence path. It is a real implementation rather than a null pointer, so the
// client never has to branch on whether a writer exists.
class NullStateWriter : public StateWriter {
public:
    bool GetString(const std::string& key, std::string& out) override;
    void SetString(const std::string& key, const std::string& value) override;

    bool GetInt64(const std::string& key, std::int64_t& out) override;
    void SetInt64(const std::string& key, std::int64_t value) override;

    bool GetUInt64(const std::string& key, std::uint64_t& out) override;
    void SetUInt64(const std::string& key, std::uint64_t value) override;

    bool GetDouble(const std::string& key, double& out) override;
    void SetDouble(const std::string& key, double value) override;

    bool GetBool(const std::string& key, bool& out) override;
    void SetBool(const std::string& key, bool value) override;

    void Erase(const std::string& key) override;
    bool Has(const std::string& key) override;
};

// Writes one file, next to the game by default.
//
// The format is a flat text file of `type:key=value` lines, written whole on every change.
// Deliberately not JSON: the SDK already has a JSON parser, but using it here would mean a
// corrupt save file is a parse error the adopter has to handle, and a flat format degrades
// to "skip the bad line" instead.
//
// **Not atomic across a crash.** A write truncates and rewrites, so a power loss mid-write
// loses the file. That is acceptable for a session token — the cost is one re-authentication
// — and the alternative (write-temp-then-rename) is not portable to every platform this SDK
// targets. Documented rather than hidden.
class FileStateWriter : public StateWriter {
public:
    // `path` is the file to write. Empty means the default, which is
    // `lootlocker-state.txt` in the current working directory.
    //
    // The file is read once, at construction, so a writer is a snapshot plus a write-through
    // cache. A second writer on the same path will not see the first one's later writes.
    explicit FileStateWriter(const std::string& path = std::string());
    ~FileStateWriter() override;

    FileStateWriter(const FileStateWriter&)            = delete;
    FileStateWriter& operator=(const FileStateWriter&) = delete;

    bool GetString(const std::string& key, std::string& out) override;
    void SetString(const std::string& key, const std::string& value) override;

    bool GetInt64(const std::string& key, std::int64_t& out) override;
    void SetInt64(const std::string& key, std::int64_t value) override;

    bool GetUInt64(const std::string& key, std::uint64_t& out) override;
    void SetUInt64(const std::string& key, std::uint64_t value) override;

    bool GetDouble(const std::string& key, double& out) override;
    void SetDouble(const std::string& key, double value) override;

    bool GetBool(const std::string& key, bool& out) override;
    void SetBool(const std::string& key, bool value) override;

    void Erase(const std::string& key) override;
    bool Has(const std::string& key) override;

    // The path in use, after the default was resolved.
    const std::string& GetPath() const;

    // True when the last write succeeded. A writer whose file is unwritable still serves
    // reads from its cache, so a game on a read-only filesystem degrades to
    // re-authenticating rather than failing.
    bool LastWriteSucceeded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lootlocker

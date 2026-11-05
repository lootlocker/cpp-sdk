// The only translation unit that includes the JSON library (D4).
//
// Every other file in the SDK talks to `core::JsonValue`, which is a thin handle over the
// decoded value. That keeps nlohmann out of the public headers and out of every other
// TU's compile time, and it means swapping the parser is a change to this file plus the
// handle's implementation — not a breaking change for adopters.

#include <lootlocker/config_file.h>
#include <lootlocker/result.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace lootlocker {
namespace config_file {

namespace {

using Json = nlohmann::json;

// Read a string field if present. Absent leaves the destination untouched, so a file can
// be layered over a hand-filled struct.
bool ReadString(const Json& j, const char* key, std::string& out, std::string& error)
{
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_string()) {
        error = std::string("\"") + key + "\" must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool ReadBool(const Json& j, const char* key, bool& out, std::string& error)
{
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        error = std::string("\"") + key + "\" must be a boolean";
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool ReadInt(const Json& j, const char* key, int& out, std::string& error)
{
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_number_integer()) {
        error = std::string("\"") + key + "\" must be an integer";
        return false;
    }
    // is_number_integer() is also true for JSON unsigned integers, and get<int>() would
    // narrow without a range check — a huge max_retries could wrap into a different,
    // plausible-looking policy value. Range-check before assigning.
    if (it->is_number_unsigned()) {
        const auto u = it->get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            error = std::string("\"") + key + "\" is out of range for an int";
            return false;
        }
        out = static_cast<int>(u);
        return true;
    }
    const auto n = it->get<std::int64_t>();
    if (n < static_cast<std::int64_t>(std::numeric_limits<int>::min())
        || n > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        error = std::string("\"") + key + "\" is out of range for an int";
        return false;
    }
    out = static_cast<int>(n);
    return true;
}

bool ReadDouble(const Json& j, const char* key, double& out, std::string& error)
{
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_number()) {
        error = std::string("\"") + key + "\" must be a number";
        return false;
    }
    out = it->get<double>();
    return true;
}

// Apply a nested object to a queue config. Absent keys are left untouched.
bool ApplyQueue(const Json& j, QueueConfig& q, std::string& error)
{
    const auto it = j.find("queue");
    if (it == j.end()) {
        return true;
    }
    if (!it->is_object()) {
        error = "\"queue\" must be an object";
        return false;
    }
    return ReadInt(*it, "max_retries", q.max_retries, error)
           && ReadInt(*it, "backoff_factor", q.backoff_factor, error)
           && ReadInt(*it, "initial_backoff_ms", q.initial_backoff_ms, error)
           && ReadInt(*it, "max_in_flight", q.max_in_flight, error)
           && ReadInt(*it, "max_queue_size", q.max_queue_size, error)
           && ReadInt(*it, "choke_warning_threshold", q.choke_warning_threshold, error)
           && ReadInt(*it, "request_timeout_seconds", q.request_timeout_seconds, error)
           && ReadBool(*it, "deny_when_backed_up", q.deny_when_backed_up, error);
}

bool ApplyRateLimit(const Json& j, RateLimitConfig& r, std::string& error)
{
    const auto it = j.find("rate_limit");
    if (it == j.end()) {
        return true;
    }
    if (!it->is_object()) {
        error = "\"rate_limit\" must be an object";
        return false;
    }
    return ReadBool(*it, "enabled", r.enabled, error)
           && ReadInt(*it, "tripwire_window_seconds", r.tripwire_window_seconds, error)
           && ReadInt(*it, "max_requests_per_tripwire", r.max_requests_per_tripwire, error)
           && ReadInt(*it, "seconds_per_bucket", r.seconds_per_bucket, error)
           && ReadDouble(*it, "moving_average_fraction", r.moving_average_fraction, error)
           && ReadInt(*it, "moving_average_time_frames", r.moving_average_time_frames, error);
}

Result<Empty> ApplyJson(const Json& j, Config& config, const std::string& raw)
{
    if (!j.is_object()) {
        Error e;
        e.code = ErrorCode::MalformedResponse;
        e.message = "the config file must contain a JSON object";
        return Result<Empty>::Fail(std::move(e), raw);
    }

    std::string error;
    const bool ok = ReadString(j, "domain_key", config.domain_key, error)
                    && ReadString(j, "game_key", config.game_key, error)
                    && ReadString(j, "game_api_key", config.game_api_key, error)
                    && ReadString(j, "game_version", config.game_version, error)
                    && ReadBool(j, "is_development", config.is_development, error)
                    && ReadString(j, "base_url", config.base_url, error)
                    && ReadString(j, "timezone", config.timezone, error)
                    && ApplyQueue(j, config.queue, error)
                    && ApplyRateLimit(j, config.rate_limit, error);

    if (!ok) {
        Error e;
        e.code = ErrorCode::MalformedResponse;
        e.message = error;
        return Result<Empty>::Fail(std::move(e), raw);
    }

    // The file parsed, but the resulting config may still be unusable. The public contract
    // promises InvalidConfig for that case, so validate before reporting success.
    const Error invalid = ValidateConfig(config);
    if (!invalid.Ok()) {
        return Result<Empty>::Fail(invalid, raw);
    }

    return Result<Empty>::Ok(Empty{}, raw);
}

} // namespace

Result<Empty> LoadFromString(const std::string& json, Config& config)
{
    // Non-throwing parse: the third argument `allow_exceptions = false` makes the library
    // return a discarded value instead of throwing. This matters because the SDK is built
    // with exceptions disabled (D7) — a try/catch here would not even compile cleanly
    // under /EHs-c-.
    const Json parsed = Json::parse(json, nullptr, /*allow_exceptions=*/false);

    if (parsed.is_discarded()) {
        Error err;
        err.code = ErrorCode::MalformedResponse;
        err.message = "the config file is not valid JSON";
        return Result<Empty>::Fail(std::move(err), json);
    }

    // The original text is carried on every result, success or failure (D7), so an adopter
    // can inspect what was actually in the file.
    return ApplyJson(parsed, config, json);
}

Result<Empty> Load(const std::string& path, Config& config)
{
    // Plain C stdio rather than iostreams: the spec forbids iostreams in core, and this is
    // the only file read in the SDK.
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        Error e;
        e.code = ErrorCode::Transport;
        e.message = "could not open the config file: " + path;
        return Result<Empty>::Fail(std::move(e));
    }

    std::string contents;
    char chunk[4096];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        contents.append(chunk, read);
    }
    const bool read_failed = std::ferror(file) != 0;
    std::fclose(file);

    if (read_failed) {
        Error e;
        e.code = ErrorCode::Transport;
        e.message = "could not read the config file: " + path;
        return Result<Empty>::Fail(std::move(e));
    }

    return LoadFromString(contents, config);
}

} // namespace config_file
} // namespace lootlocker


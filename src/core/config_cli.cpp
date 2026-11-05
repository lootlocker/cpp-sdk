#include <lootlocker/config_cli.h>
#include <lootlocker/result.h>

#include <string>
#include <vector>

namespace lootlocker {
namespace config_cli {

namespace {

// A recognised flag and where its value goes.
struct Flag {
    const char* name;
    std::string* target;
};

// Parse "--key=value" or "--key value". Returns false when the flag is recognised but has
// no value, which is an error rather than a silent no-op.
bool ApplyOne(const std::string& arg, const std::string& next, bool has_next, Config& config,
              bool& consumed_next, std::string& error)
{
    // Strip the leading "--".
    if (arg.size() < 3 || arg[0] != '-' || arg[1] != '-') {
        return true;   // not a long flag; ignore
    }
    std::string key = arg.substr(2);
    std::string value;
    bool has_inline_value = false;

    const std::size_t eq = key.find('=');
    if (eq != std::string::npos) {
        value = key.substr(eq + 1);
        key = key.substr(0, eq);
        has_inline_value = true;
    }

    // --is-development is a bare boolean flag: presence means true.
    if (key == "is-development") {
        config.is_development = true;
        return true;
    }

    const Flag flags[] = {
        {"domain-key", &config.domain_key},
        {"game-key", &config.game_key},
        {"game-api-key", &config.game_api_key},
        {"game-version", &config.game_version},
        {"base-url", &config.base_url},
        {"timezone", &config.timezone},
    };

    std::string* target = nullptr;
    for (const Flag& f : flags) {
        if (key == f.name) {
            target = f.target;
            break;
        }
    }

    if (target == nullptr) {
        return true;   // unknown flag; ignore, because the host has its own flags
    }

    if (!has_inline_value) {
        // A following argument that is itself a flag is not a value. Without this,
        // `--domain-key --game-key xyz` would store "--game-key" as the domain key and
        // silently drop "xyz".
        if (!has_next || next.empty() || next.rfind("--", 0) == 0) {
            error = "--" + key + " needs a value";
            return false;
        }
        value = next;
        consumed_next = true;
    }

    *target = value;
    return true;
}

} // namespace

Result<Empty> Apply(const std::vector<std::string>& args, Config& config)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        const bool has_next = (i + 1) < args.size();
        const std::string next = has_next ? args[i + 1] : std::string();
        bool consumed_next = false;
        std::string error;

        if (!ApplyOne(args[i], next, has_next, config, consumed_next, error)) {
            Error e;
            e.code = ErrorCode::InvalidConfig;
            e.message = error;
            return Result<Empty>::Fail(std::move(e));
        }

        if (consumed_next) {
            ++i;
        }
    }

    return Result<Empty>::Ok(Empty{});
}

Result<Empty> Apply(int argc, const char* const* argv, Config& config)
{
    std::vector<std::string> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 0);
    for (int i = 0; i < argc; ++i) {
        if (argv[i] != nullptr) {
            args.emplace_back(argv[i]);
        }
    }
    return Apply(args, config);
}

} // namespace config_cli
} // namespace lootlocker

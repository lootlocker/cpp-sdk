#pragma once

// The command line parser (D9).
//
// A thin, optional helper that *populates* a `Config`. It is never called implicitly. The
// adopter calls it, or fills the struct by hand, or both in either order — precedence is
// theirs, and calling this last means the command line wins.
//
// It ships in `core` rather than as a separate target (D9 sub-question, answered
// 2026-11-04): `core` is the module every adopter links anyway, and splitting the parser
// out risks an adopter accidentally leaving out something they need. It is namespaced and
// opt-in at the call site, so a game that never calls `Apply` pays nothing at runtime.

#include <lootlocker/config.h>
#include <lootlocker/result.h>

#include <string>
#include <vector>

namespace lootlocker {
namespace config_cli {

// Apply `--key value` and `--key=value` arguments to `config`.
//
// Recognised keys mirror the `Config` fields:
//   --domain-key, --game-key, --game-api-key, --game-version,
//   --is-development, --base-url, --timezone
//
// Unknown arguments are ignored rather than rejected: a game's own command line will have
// its own flags, and failing on them would make this helper unusable in a real program.
// A recognised key with a missing value is an `InvalidConfig` error.
//
// Returns an `Empty` result on success.
Result<Empty> Apply(int argc, const char* const* argv, Config& config);

// The same, for a vector. Convenience for tests and for hosts that already have one.
Result<Empty> Apply(const std::vector<std::string>& args, Config& config);

} // namespace config_cli
} // namespace lootlocker

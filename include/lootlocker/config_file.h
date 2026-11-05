#pragma once

// The JSON config file loader (D9).
//
// A thin, optional helper that *populates* a `Config`. It is never called implicitly: the
// SDK reads no file on its own. The adopter calls this, or fills the struct by hand, or
// both in either order — precedence is theirs.
//
// The file format is JSON, not INI, because the config has nested sections (queue,
// rate_limit) and INI cannot express them.

#include <lootlocker/config.h>
#include <lootlocker/result.h>

#include <string>

namespace lootlocker {
namespace config_file {

// Load `path` into `config`, overwriting only the fields the file actually contains.
//
// Returns an `Empty` result on success. On failure the error is one of:
//   - `Transport`         — the file could not be read
//   - `MalformedResponse` — the file is not valid JSON, or a field has the wrong type
//   - `InvalidConfig`     — the file parsed but the resulting config is unusable
//
// A field that is absent from the file is left untouched, so a caller can layer a file
// over a hand-filled struct.
Result<Empty> Load(const std::string& path, Config& config);

// Load from a JSON string rather than a file. Same contract, same errors minus the read.
Result<Empty> LoadFromString(const std::string& json, Config& config);

} // namespace config_file
} // namespace lootlocker

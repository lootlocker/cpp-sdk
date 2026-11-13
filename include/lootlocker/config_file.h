#pragma once

// The JSON config file loader (D9).
//
// A thin, optional helper that *populates* a `Config`. It is never called implicitly: the
// SDK reads no file on its own. The adopter calls this, or fills the struct by hand, or
// both in either order — precedence is theirs.
//
// The file format is JSON, not INI, because the config has nested sections (queue,
// rate_limit) and INI cannot express them.
//
// **A file may be plain JSON or obfuscated Base64** (D26). The loader tries JSON first,
// because a JSON document is never valid Base64, then falls back to deobfuscating. See
// `config_obfuscation.h` for what "obfuscated" means — it is not encryption, and the key
// ships in the binary.

#include <lootlocker/config.h>
#include <lootlocker/result.h>

#include <string>

namespace lootlocker {
namespace config_file {

// Load `path` into `config`, overwriting only the fields the file actually contains.
//
// Returns an `Empty` result on success. On failure the error is one of:
//   - `Transport`         — the file could not be read
//   - `MalformedResponse` — the file is neither valid JSON nor obfuscated Base64; or it
//                           parsed but a field has the wrong type; or it decoded from
//                           Base64 but the result is not valid JSON
//   - `InvalidConfig`     — the file parsed but the resulting config is unusable
//
// A field that is absent from the file is left untouched, so a caller can layer a file
// over a hand-filled struct.
//
// `RawBody()` on the result is the file's original text — the Base64 when the file was
// obfuscated, not the decoded JSON. An adopter can decode Base64 themselves; they cannot
// recover the original bytes once the loader replaces them.
Result<Empty> Load(const std::string& path, Config& config);

// Load from a string rather than a file. Same contract, same errors minus the read.
Result<Empty> LoadFromString(const std::string& json, Config& config);

} // namespace config_file
} // namespace lootlocker

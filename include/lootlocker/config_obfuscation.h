#pragma once

// Config obfuscation (D26, R29).
//
// **This is obfuscation, not encryption.** The key is compiled into the binary, so anyone
// with a debugger can recover it and read the config. Its only purpose is to stop a player
// casually reading an API key out of a text file. Do not use it to protect anything you
// would mind losing, and do not describe it as encryption in your own documentation —
// R29 exists because the other SDKs do exactly that.
//
// **Why not AES.** Both engine SDKs use AES with a hardcoded key, and they are not
// compatible with each other: Unity is AES-128-CBC with an IV prefix, Unreal is
// AES-256-ECB with PKCS7. Matching either would mean adding a crypto implementation to
// `core`, which every adopter links — against D2's source-only drop-in promise and the
// spec's refusal to require a package manager. A vendored AES would also be a
// security-shaped dependency that invites the reader to believe the result is secure,
// which is the exact misreading R29 is about.
//
// So this is a stream cipher with a fixed key: honest about being obfuscation, small, no
// dependency, and no pretence. If an adopter needs real protection, the answer is OS key
// storage or an injected seam, not a stronger cipher with a key in the binary.

#include <lootlocker/result.h>

#include <string>

namespace lootlocker {
namespace config_obfuscation {

// Obfuscate `plaintext` and return it Base64-encoded.
//
// The output is safe to store in a text file: Base64 is ASCII, has no newlines, and
// survives a round trip through any editor.
std::string Obfuscate(const std::string& plaintext);

// Recover the plaintext from `Obfuscate`'s output.
//
// Returns `MalformedResponse` when the input is not valid Base64. It never returns a
// partial result: a config that half-decodes is worse than one that fails, because the
// failure is visible.
Result<std::string> Deobfuscate(const std::string& base64);

// True when `text` is well-formed Base64.
//
// Exposed because the loader uses it to decide whether a file is plain JSON or obfuscated,
// and an adopter writing their own loader needs the same test.
bool IsValidBase64(const std::string& text);

// Base64 without a dependency. Exposed for the same reason as `IsValidBase64`.
std::string Base64Encode(const std::string& bytes);
Result<std::string> Base64Decode(const std::string& text);

} // namespace config_obfuscation
} // namespace lootlocker

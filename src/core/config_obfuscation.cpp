// Config obfuscation (D26, R29). See the header for why this is not AES.
//
// The construction is a fixed-key XOR stream over the plaintext, then Base64. The key is
// deliberately not secret and deliberately not derived from anything: pretending otherwise
// would be the lie R29 is about.

#include <lootlocker/config_obfuscation.h>

#include <cstddef>
#include <string>

namespace lootlocker {
namespace config_obfuscation {

namespace {

// Not a secret. See the header: the key ships in the binary, so its only job is to make
// the file not-plain-text. A longer key would not change that.
const unsigned char kKey[] = {
    0x81, 0x71, 0xF7, 0xD6, 0xE5, 0xC4, 0xB3, 0xA2,
    0x8A, 0x9B, 0xAC, 0xBD, 0xCE, 0xDF, 0xE0, 0xF1,
};

const std::size_t kKeyLength = sizeof(kKey) / sizeof(kKey[0]);

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// -1 for anything that is not a Base64 digit.
int Base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

} // namespace

std::string Base64Encode(const std::string& bytes)
{
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const unsigned int triple =
            (static_cast<unsigned int>(static_cast<unsigned char>(bytes[i])) << 16) |
            (static_cast<unsigned int>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
            static_cast<unsigned int>(static_cast<unsigned char>(bytes[i + 2]));
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }

    const std::size_t remaining = bytes.size() - i;
    if (remaining == 1) {
        const unsigned int triple =
            static_cast<unsigned int>(static_cast<unsigned char>(bytes[i])) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const unsigned int triple =
            (static_cast<unsigned int>(static_cast<unsigned char>(bytes[i])) << 16) |
            (static_cast<unsigned int>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

bool IsValidBase64(const std::string& text)
{
    if (text.empty()) {
        return false;
    }

    // A Base64 string is a multiple of four characters, and padding only appears at the
    // end. Checking the length first means the decoder never has to reason about a
    // truncated final group.
    if (text.size() % 4 != 0) {
        return false;
    }

    std::size_t padding = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '=') {
            ++padding;
            // Padding is only legal in the last two positions.
            if (i < text.size() - 2) {
                return false;
            }
            continue;
        }
        if (padding > 0) {
            return false; // a digit after padding
        }
        if (Base64Value(c) < 0) {
            return false;
        }
    }
    return padding <= 2;
}

Result<std::string> Base64Decode(const std::string& text)
{
    if (!IsValidBase64(text)) {
        Error e;
        e.code = ErrorCode::MalformedResponse;
        e.message = "the value is not valid Base64";
        return Result<std::string>::Fail(std::move(e), text);
    }

    std::string out;
    out.reserve((text.size() / 4) * 3);

    for (std::size_t i = 0; i < text.size(); i += 4) {
        int values[4] = {0, 0, 0, 0};
        int digits = 4;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=') {
                values[k] = 0;
                if (digits == 4) {
                    digits = k;
                }
            } else {
                values[k] = Base64Value(c);
            }
        }

        const unsigned int triple =
            (static_cast<unsigned int>(values[0]) << 18) |
            (static_cast<unsigned int>(values[1]) << 12) |
            (static_cast<unsigned int>(values[2]) << 6) |
            static_cast<unsigned int>(values[3]);

        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (digits > 2) {
            out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        }
        if (digits > 3) {
            out.push_back(static_cast<char>(triple & 0xFF));
        }
    }

    return Result<std::string>::Ok(std::move(out), text);
}

std::string Obfuscate(const std::string& plaintext)
{
    std::string masked;
    masked.reserve(plaintext.size());
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(plaintext[i]);
        masked.push_back(static_cast<char>(byte ^ kKey[i % kKeyLength]));
    }
    return Base64Encode(masked);
}

Result<std::string> Deobfuscate(const std::string& base64)
{
    Result<std::string> decoded = Base64Decode(base64);
    if (!decoded.Ok()) {
        return decoded;
    }

    const std::string& masked = decoded.Value();
    std::string plaintext;
    plaintext.reserve(masked.size());
    for (std::size_t i = 0; i < masked.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(masked[i]);
        plaintext.push_back(static_cast<char>(byte ^ kKey[i % kKeyLength]));
    }

    return Result<std::string>::Ok(std::move(plaintext), base64);
}

} // namespace config_obfuscation
} // namespace lootlocker

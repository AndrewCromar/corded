// Checking that a release is ours before a server installs it.
//
// Each release archive is published with a signature made by the release key,
// whose private half lives only in the build system. A server carries the
// public half and installs nothing whose signature does not match.
#pragma once

#include "corded/common/bytes.hpp"

#include <sodium.h>

#include <optional>
#include <string>
#include <string_view>

namespace corded::release {

// The 32-byte Ed25519 public key inside the text openssl writes
// ("-----BEGIN PUBLIC KEY-----" and so on), or inside its base64 line alone.
inline std::optional<Bytes> public_key_from_pem(std::string_view pem) {
    std::string body;
    size_t at = 0;
    while (at < pem.size()) {
        size_t end = pem.find('\n', at);
        if (end == std::string_view::npos) end = pem.size();
        std::string_view line = pem.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find("-----") == std::string_view::npos) body.append(line);
        at = end + 1;
    }
    Bytes der(body.size());
    size_t len = 0;
    if (sodium_base642bin(der.data(), der.size(), body.data(), body.size(), " \t", &len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
        return std::nullopt;
    der.resize(len);
    // A fixed 12-byte header that says "Ed25519 public key", then the key.
    static const uint8_t header[] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
    if (der.size() != sizeof header + crypto_sign_PUBLICKEYBYTES) return std::nullopt;
    if (!std::equal(std::begin(header), std::end(header), der.begin())) return std::nullopt;
    return Bytes(der.begin() + sizeof header, der.end());
}

// Whether `signature` is the release key's signature over exactly these bytes.
inline bool signed_by(ByteView archive, ByteView signature, ByteView public_key) {
    return signature.size() == crypto_sign_BYTES && public_key.size() == crypto_sign_PUBLICKEYBYTES &&
           crypto_sign_verify_detached(signature.data(), archive.data(), archive.size(), public_key.data()) == 0;
}

// "v0.5.0-3-gabc1234" and "v0.5.0" are the same release for this purpose:
// what `git describe` adds after the tag is dropped.
inline std::string release_of(std::string_view version) {
    size_t dash = version.find('-');
    return std::string(version.substr(0, dash));
}

}  // namespace corded::release

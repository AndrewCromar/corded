// Ed25519 helpers. Every signed message is a context string followed by its parts.
#pragma once

#include "corded/common/bytes.hpp"

#include <initializer_list>

namespace corded {

inline Bytes signed_message(std::string_view ctx, std::initializer_list<ByteView> parts) {
    Bytes msg;
    append(msg, ctx);
    msg.push_back(0);
    for (auto p : parts) append(msg, p);
    return msg;
}

inline bool verify_sig(ByteView pk, ByteView msg, ByteView sig) {
    if (pk.size() != crypto_sign_PUBLICKEYBYTES || sig.size() != crypto_sign_BYTES) return false;
    return crypto_sign_verify_detached(sig.data(), msg.data(), msg.size(), pk.data()) == 0;
}

// sk is the 64-byte libsodium secret key.
inline Bytes sign(ByteView sk, ByteView msg) {
    if (sk.size() != crypto_sign_SECRETKEYBYTES) throw std::invalid_argument("bad signing key");
    Bytes sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, msg.data(), msg.size(), sk.data());
    return sig;
}

}  // namespace corded

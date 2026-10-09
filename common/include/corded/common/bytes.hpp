// Byte helpers shared by the core and the server.
#pragma once

#include <sodium.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace corded {

using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;
using Key32 = std::array<uint8_t, 32>;

inline Bytes to_bytes(ByteView v) { return Bytes(v.begin(), v.end()); }
inline Bytes to_bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }
inline std::string to_string(ByteView v) { return std::string(v.begin(), v.end()); }

inline Key32 to_key32(ByteView v) {
    if (v.size() != 32) throw std::invalid_argument("expected 32 bytes");
    Key32 k;
    std::memcpy(k.data(), v.data(), 32);
    return k;
}

inline void append(Bytes& out, ByteView v) { out.insert(out.end(), v.begin(), v.end()); }
inline void append(Bytes& out, std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }

inline std::string b64(ByteView v) {
    std::string out(sodium_base64_encoded_len(v.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING),
                    '\0');
    sodium_bin2base64(out.data(), out.size(), v.data(), v.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::strlen(out.c_str()));
    return out;
}

inline std::optional<Bytes> unb64(std::string_view s) {
    Bytes out(s.size());
    size_t len = 0;
    if (sodium_base642bin(out.data(), out.size(), s.data(), s.size(), nullptr, &len, nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
        return std::nullopt;
    out.resize(len);
    return out;
}

inline std::string hex(ByteView v) {
    std::string out(v.size() * 2 + 1, '\0');
    sodium_bin2hex(out.data(), out.size(), v.data(), v.size());
    out.resize(v.size() * 2);
    return out;
}

inline Bytes random_bytes(size_t n) {
    Bytes b(n);
    randombytes_buf(b.data(), n);
    return b;
}

// Minimal binary writer and reader for state that is stored in the vault.
class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void raw(ByteView v) { append(buf_, v); }
    void blob(ByteView v) {
        u32(static_cast<uint32_t>(v.size()));
        raw(v);
    }
    Bytes take() { return std::move(buf_); }
    const Bytes& data() const { return buf_; }

private:
    Bytes buf_;
};

class Reader {
public:
    explicit Reader(ByteView v) : v_(v) {}
    uint8_t u8() {
        need(1);
        return v_[pos_++];
    }
    uint32_t u32() {
        need(4);
        uint32_t r = 0;
        for (int i = 0; i < 4; ++i) r |= static_cast<uint32_t>(v_[pos_++]) << (8 * i);
        return r;
    }
    ByteView raw(size_t n) {
        need(n);
        auto r = v_.subspan(pos_, n);
        pos_ += n;
        return r;
    }
    Key32 key32() { return to_key32(raw(32)); }
    Bytes blob() {
        uint32_t n = u32();
        return to_bytes(raw(n));
    }
    ByteView rest() { return raw(v_.size() - pos_); }
    bool done() const { return pos_ == v_.size(); }

private:
    void need(size_t n) const {
        if (v_.size() - pos_ < n) throw std::runtime_error("truncated input");
    }
    ByteView v_;
    size_t pos_ = 0;
};

}  // namespace corded

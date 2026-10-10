// Frame encoding: u32 little-endian length, then a FlatBuffers Frame.
#pragma once

#include "corded/common/bytes.hpp"
#include "corded_generated.h"

#include <memory>
#include <optional>

namespace corded {

inline constexpr uint32_t kMaxFrameBytes = 1024 * 1024;
inline constexpr uint16_t kProtocolVersion = 1;
// A file travels in pieces of this size, each well inside one frame.
inline constexpr uint32_t kBlobChunkBytes = 256 * 1024;

// Signature context strings. Every signed message starts with one of these.
inline constexpr std::string_view kCtxAuth = "corded/v1/auth";
inline constexpr std::string_view kCtxDeviceCert = "corded/v1/device-cert";
inline constexpr std::string_view kCtxSignedPrekey = "corded/v1/signed-prekey";

namespace err {
inline constexpr uint16_t Malformed = 100;
inline constexpr uint16_t BadSignature = 200;
inline constexpr uint16_t UnknownDevice = 201;
inline constexpr uint16_t NotAuthenticated = 202;
inline constexpr uint16_t NameTaken = 203;
inline constexpr uint16_t RegistrationClosed = 204;
inline constexpr uint16_t Kicked = 205;
inline constexpr uint16_t Forbidden = 300;
inline constexpr uint16_t NotFound = 400;
inline constexpr uint16_t Internal = 500;
}  // namespace err

// Serialise a frame including its length prefix.
inline std::shared_ptr<Bytes> encode_frame(const wire::FrameT& frame) {
    flatbuffers::FlatBufferBuilder fbb(256);
    fbb.Finish(wire::Frame::Pack(fbb, &frame));
    auto out = std::make_shared<Bytes>();
    uint32_t n = fbb.GetSize();
    out->reserve(n + 4);
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(n >> (8 * i)));
    out->insert(out->end(), fbb.GetBufferPointer(), fbb.GetBufferPointer() + n);
    return out;
}

inline uint32_t decode_length(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

// Verify and unpack a frame body (without the length prefix). Untrusted input.
inline std::optional<wire::FrameT> decode_frame(ByteView body) {
    flatbuffers::Verifier verifier(body.data(), body.size());
    if (!wire::VerifyFrameBuffer(verifier)) return std::nullopt;
    wire::FrameT out;
    wire::GetFrame(body.data())->UnPackTo(&out);
    if (out.body.type == wire::FrameBody_NONE) return std::nullopt;
    return out;
}

template <typename T>
wire::FrameT make_frame(uint32_t request_id, T&& body) {
    wire::FrameT f;
    f.request_id = request_id;
    f.body.Set(std::forward<T>(body));
    return f;
}

inline wire::FrameT error_frame(uint32_t request_id, uint16_t code, std::string message) {
    wire::ErrorT e;
    e.code = code;
    e.message = std::move(message);
    return make_frame(request_id, std::move(e));
}

}  // namespace corded

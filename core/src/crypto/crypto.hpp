// End-to-end encryption for pairwise sessions: X3DH key agreement followed by
// the Double Ratchet, built only from libsodium primitives.
//
// PROTOTYPE: this code has not been reviewed or audited.
#pragma once

#include "corded/common/bytes.hpp"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace corded::crypto {

class CryptoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct KeyPair {  // X25519
    Key32 pk{};
    Key32 sk{};
};

struct SignKeyPair {  // Ed25519, sk in libsodium's 64-byte form
    Key32 pk{};
    std::array<uint8_t, 64> sk{};
};

KeyPair generate_dh();
SignKeyPair generate_sign();
Key32 dh(const Key32& sk, const Key32& pk);  // throws on a degenerate result
Bytes hkdf(ByteView salt, ByteView ikm, std::string_view info, size_t len);
Bytes aead_encrypt(const Key32& key, ByteView nonce24, ByteView plaintext, ByteView ad);
std::optional<Bytes> aead_decrypt(const Key32& key, ByteView nonce24, ByteView ciphertext,
                                  ByteView ad);

// Pads to a bucket size so ciphertext length reveals little about the content.
Bytes pad(ByteView plaintext);
std::optional<Bytes> unpad(ByteView padded);

// This device's long-term keys.
struct Identity {
    SignKeyPair user;    // user identity key; its public half is the user id
    SignKeyPair device;  // device signing key; its public half is the device id
    KeyPair dh;          // device DH identity key
    Bytes cert;          // user signature over (device id, dh key)

    static Identity generate();
};

bool verify_device_cert(ByteView user_id, ByteView device_id, ByteView dh_key, ByteView cert);

// What the server hands out so a session can be started with an offline peer.
struct PeerBundle {
    Key32 user_id{}, device_id{}, dh_key{};
    Bytes cert;
    uint32_t spk_id = 0;
    Key32 spk{};
    Bytes spk_sig;
    bool has_otk = false;
    uint32_t otk_id = 0;
    Key32 otk{};

    bool verify() const;
};

// Where the responder finds the secret halves of its published prekeys.
struct PrekeySource {
    virtual ~PrekeySource() = default;
    virtual std::optional<KeyPair> signed_prekey(uint32_t id) = 0;
    // One-time prekeys are consumed: the implementation must delete on take.
    virtual std::optional<KeyPair> take_one_time(uint32_t id) = 0;
};

// One Double Ratchet session.
struct Ratchet {
    KeyPair dhs;
    std::optional<Key32> dhr;
    Key32 rk{};
    std::optional<Key32> cks, ckr;
    uint32_t ns = 0, nr = 0, pn = 0;
    std::map<std::pair<Key32, uint32_t>, Key32> skipped;
    Bytes ad_base;  // initiator DH identity key, then responder's

    // The initiator's ephemeral key identifies the session to both sides.
    Key32 x3dh_ek{};
    // Initiator only: attach the X3DH header until the peer has answered.
    bool pending_prekey = false;
    uint32_t spk_id = 0, otk_id = 0;
    bool has_otk = false;

    void write(Writer& w) const;
    static Ratchet read(Reader& r);
};

// All sessions with one peer device. Several can exist when both sides start a
// conversation at the same moment; the one most recently received on is used
// for sending.
struct PeerSessions {
    Key32 user_id{}, device_id{}, dh_key{};
    std::vector<Ratchet> sessions;
    size_t active = 0;

    bool empty() const { return sessions.empty(); }
    Bytes serialize() const;
    static PeerSessions parse(ByteView data);
};

// Starts a session as the initiator (X3DH) and appends it to `peer`.
void start_session(const Identity& me, const PeerBundle& bundle, PeerSessions& peer);

// `context` is bound into the authenticated data: the room id and event id.
Bytes encrypt(const Identity& me, PeerSessions& peer, ByteView context, ByteView plaintext);

// Returns the plaintext, or nullopt if the message does not authenticate. On
// failure `peer` is left exactly as it was. `peer.user_id` and `peer.device_id`
// must be set from the transport's view of the sender before calling.
std::optional<Bytes> decrypt(const Identity& me, PrekeySource& prekeys, PeerSessions& peer,
                             ByteView context, ByteView message);

}  // namespace corded::crypto

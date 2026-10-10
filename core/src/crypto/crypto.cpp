#include "crypto.hpp"

#include "corded/common/frame.hpp"
#include "corded/common/sig.hpp"

#include <cctype>
#include <cstdio>

namespace corded::crypto {

namespace {

constexpr std::string_view kInfoX3dh = "corded/v1/x3dh";
constexpr std::string_view kInfoRoot = "corded/v1/dr/root";
constexpr std::string_view kInfoMsg = "corded/v1/dr/msg";
constexpr std::string_view kAdPrefix = "corded/v1/dr";

constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypePrekey = 1;
constexpr uint8_t kTypeNormal = 2;

constexpr uint32_t kMaxSkip = 1000;
constexpr size_t kMaxSkippedStored = 1000;
constexpr size_t kMaxSessionsPerPeer = 5;

struct Header {
    Key32 dh{};
    uint32_t pn = 0, n = 0;

    Bytes bytes() const {
        Writer w;
        w.raw(dh);
        w.u32(pn);
        w.u32(n);
        return w.take();
    }
};

// Derives the next chain key and the message key for this step.
std::pair<Key32, Key32> kdf_ck(const Key32& ck) {
    Key32 mk, next;
    const uint8_t one = 0x01, two = 0x02;
    crypto_auth_hmacsha256(mk.data(), &one, 1, ck.data());
    crypto_auth_hmacsha256(next.data(), &two, 1, ck.data());
    return {next, mk};
}

// Returns (new root key, new chain key).
std::pair<Key32, Key32> kdf_rk(const Key32& rk, const Key32& dh_out) {
    Bytes out = hkdf(rk, dh_out, kInfoRoot, 64);
    auto result = std::pair{to_key32(ByteView(out).subspan(0, 32)),
                            to_key32(ByteView(out).subspan(32, 32))};
    sodium_memzero(out.data(), out.size());
    return result;
}

Bytes make_ad(const Ratchet& s, ByteView context, const Header& h) {
    Bytes ad;
    append(ad, kAdPrefix);
    append(ad, s.ad_base);
    Writer w;
    w.blob(context);
    append(ad, w.data());
    append(ad, h.bytes());
    return ad;
}

Bytes seal(const Key32& mk, ByteView plaintext, ByteView ad) {
    Bytes kn = hkdf({}, mk, kInfoMsg, 32 + 24);
    Key32 key = to_key32(ByteView(kn).subspan(0, 32));
    Bytes padded = pad(plaintext);
    Bytes ct = aead_encrypt(key, ByteView(kn).subspan(32, 24), padded, ad);
    sodium_memzero(kn.data(), kn.size());
    sodium_memzero(key.data(), key.size());
    sodium_memzero(padded.data(), padded.size());
    return ct;
}

std::optional<Bytes> open(const Key32& mk, ByteView ciphertext, ByteView ad) {
    Bytes kn = hkdf({}, mk, kInfoMsg, 32 + 24);
    Key32 key = to_key32(ByteView(kn).subspan(0, 32));
    auto padded = aead_decrypt(key, ByteView(kn).subspan(32, 24), ciphertext, ad);
    sodium_memzero(kn.data(), kn.size());
    sodium_memzero(key.data(), key.size());
    if (!padded) return std::nullopt;
    return unpad(*padded);
}

void skip_until(Ratchet& s, uint32_t until) {
    if (!s.ckr) return;
    if (until < s.nr) return;
    if (until - s.nr > kMaxSkip) throw CryptoError("too many skipped messages");
    while (s.nr < until) {
        auto [next, mk] = kdf_ck(*s.ckr);
        s.ckr = next;
        s.skipped[{*s.dhr, s.nr}] = mk;
        ++s.nr;
        if (s.skipped.size() > kMaxSkippedStored) s.skipped.erase(s.skipped.begin());
    }
}

void dh_ratchet(Ratchet& s, const Key32& their_dh) {
    s.pn = s.ns;
    s.ns = 0;
    s.nr = 0;
    s.dhr = their_dh;
    auto [rk1, ckr] = kdf_rk(s.rk, dh(s.dhs.sk, their_dh));
    s.rk = rk1;
    s.ckr = ckr;
    s.dhs = generate_dh();
    auto [rk2, cks] = kdf_rk(s.rk, dh(s.dhs.sk, their_dh));
    s.rk = rk2;
    s.cks = cks;
}

// Works on a copy; the caller commits the copy only on success.
std::optional<Bytes> ratchet_decrypt(Ratchet& s, ByteView context, const Header& h,
                                     ByteView ciphertext) {
    Bytes ad = make_ad(s, context, h);
    if (auto it = s.skipped.find({h.dh, h.n}); it != s.skipped.end()) {
        auto pt = open(it->second, ciphertext, ad);
        if (pt) s.skipped.erase(it);
        return pt;
    }
    if (!s.dhr || *s.dhr != h.dh) {
        skip_until(s, h.pn);
        dh_ratchet(s, h.dh);
    }
    if (!s.ckr) return std::nullopt;
    if (h.n < s.nr) return std::nullopt;  // already used, or a replay
    skip_until(s, h.n);
    auto [next, mk] = kdf_ck(*s.ckr);
    s.ckr = next;
    ++s.nr;
    return open(mk, ciphertext, ad);
}

Bytes x3dh_secret(std::initializer_list<Key32> dhs) {
    Bytes ikm(32, 0xFF);
    for (const auto& d : dhs) append(ikm, d);
    Bytes zero_salt(32, 0);
    Bytes sk = hkdf(zero_salt, ikm, kInfoX3dh, 32);
    sodium_memzero(ikm.data(), ikm.size());
    return sk;
}

}  // namespace

KeyPair generate_dh() {
    KeyPair kp;
    randombytes_buf(kp.sk.data(), kp.sk.size());
    crypto_scalarmult_base(kp.pk.data(), kp.sk.data());
    return kp;
}

SignKeyPair generate_sign() {
    SignKeyPair kp;
    crypto_sign_keypair(kp.pk.data(), kp.sk.data());
    return kp;
}

Key32 dh(const Key32& sk, const Key32& pk) {
    Key32 out;
    if (crypto_scalarmult(out.data(), sk.data(), pk.data()) != 0)
        throw CryptoError("degenerate key agreement");
    return out;
}

Bytes hkdf(ByteView salt, ByteView ikm, std::string_view info, size_t len) {
    uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
    crypto_kdf_hkdf_sha256_extract(prk, salt.data(), salt.size(), ikm.data(), ikm.size());
    Bytes out(len);
    if (crypto_kdf_hkdf_sha256_expand(out.data(), out.size(), info.data(), info.size(), prk) != 0)
        throw CryptoError("hkdf failed");
    sodium_memzero(prk, sizeof prk);
    return out;
}

Bytes aead_encrypt(const Key32& key, ByteView nonce24, ByteView plaintext, ByteView ad) {
    if (nonce24.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES)
        throw CryptoError("bad nonce size");
    Bytes out(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long n = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data(), &n, plaintext.data(), plaintext.size(),
                                               ad.data(), ad.size(), nullptr, nonce24.data(),
                                               key.data());
    out.resize(n);
    return out;
}

std::optional<Bytes> aead_decrypt(const Key32& key, ByteView nonce24, ByteView ciphertext,
                                  ByteView ad) {
    if (nonce24.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES ||
        ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES)
        return std::nullopt;
    Bytes out(ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long n = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(out.data(), &n, nullptr, ciphertext.data(),
                                                   ciphertext.size(), ad.data(), ad.size(),
                                                   nonce24.data(), key.data()) != 0)
        return std::nullopt;
    out.resize(n);
    return out;
}

Bytes pad(ByteView plaintext) {
    size_t need = plaintext.size() + 1;
    size_t bucket = 256;
    while (bucket < need && bucket < 4096) bucket *= 2;
    if (bucket < need) bucket = ((need + 4095) / 4096) * 4096;
    Bytes out(bucket, 0);
    if (!plaintext.empty()) std::memcpy(out.data(), plaintext.data(), plaintext.size());
    out[plaintext.size()] = 0x80;
    return out;
}

std::optional<Bytes> unpad(ByteView padded) {
    size_t i = padded.size();
    while (i > 0 && padded[i - 1] == 0) --i;
    if (i == 0 || padded[i - 1] != 0x80) return std::nullopt;
    return to_bytes(padded.subspan(0, i - 1));
}

Identity Identity::generate() {
    Identity id;
    id.user = generate_sign();
    id.device = generate_sign();
    id.dh = generate_dh();
    id.cert = sign(id.user.sk, signed_message(kCtxDeviceCert, {id.device.pk, id.dh.pk}));
    return id;
}

namespace {

// Thirty digits for one identity key, from an iterated hash so that searching
// for a look-alike key is expensive.
std::string key_digits(const Key32& user) {
    constexpr std::string_view kLabel = "corded/v1/safety-number";
    uint8_t h[crypto_hash_sha512_BYTES];
    Bytes in;
    append(in, kLabel);
    append(in, user);
    crypto_hash_sha512(h, in.data(), in.size());
    for (int i = 0; i < 5200; ++i) {
        in.assign(h, h + sizeof h);
        append(in, user);
        crypto_hash_sha512(h, in.data(), in.size());
    }
    std::string out;
    for (int group = 0; group < 6; ++group) {
        uint64_t v = 0;
        for (int i = 0; i < 5; ++i) v = (v << 8) | h[group * 5 + i];
        char buf[8];
        std::snprintf(buf, sizeof buf, "%05u", static_cast<unsigned>(v % 100000));
        out += buf;
    }
    return out;
}

}  // namespace

std::string safety_number(const Key32& user_a, const Key32& user_b) {
    std::string a = key_digits(user_a), b = key_digits(user_b);
    std::string all = a < b ? a + b : b + a;  // same order for both people
    std::string out;
    for (size_t i = 0; i < all.size(); i += 5) {
        if (i) out += ' ';
        out += all.substr(i, 5);
    }
    return out;
}

Identity Identity::from_seed(const Key32& seed) {
    Identity id;
    crypto_sign_seed_keypair(id.user.pk.data(), id.user.sk.data(), seed.data());
    id.device = generate_sign();
    id.dh = generate_dh();
    id.cert = sign(id.user.sk, signed_message(kCtxDeviceCert, {id.device.pk, id.dh.pk}));
    return id;
}

Key32 Identity::seed() const {
    Key32 out;
    crypto_sign_ed25519_sk_to_seed(out.data(), user.sk.data());
    return out;
}

namespace {

// Crockford's base32: no I, L, O or U, so it survives being read aloud or typed.
constexpr std::string_view kBase32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::array<uint8_t, 2> recovery_check(const Key32& seed) {
    constexpr std::string_view label = "corded/v1/recovery-key";
    Bytes in;
    append(in, label);
    append(in, seed);
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, in.data(), in.size());
    return {h[0], h[1]};
}

}  // namespace

std::string encode_recovery_key(const Key32& seed) {
    Bytes payload(seed.begin(), seed.end());
    auto check = recovery_check(seed);
    payload.insert(payload.end(), check.begin(), check.end());
    std::string out;
    uint32_t acc = 0;
    int bits = 0, count = 0;
    auto put = [&](uint32_t v) {
        if (count && count % 5 == 0) out += '-';
        out += kBase32[v & 31];
        ++count;
    };
    for (uint8_t b : payload) {
        acc = (acc << 8) | b;
        bits += 8;
        while (bits >= 5) {
            put(acc >> (bits - 5));
            bits -= 5;
        }
    }
    if (bits > 0) put(acc << (5 - bits));
    sodium_memzero(payload.data(), payload.size());
    return out;
}

std::optional<Key32> decode_recovery_key(std::string_view text) {
    Bytes payload;
    uint32_t acc = 0;
    int bits = 0, symbols = 0;
    for (char raw : text) {
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        auto pos = kBase32.find(ch);
        if (pos == std::string_view::npos) return std::nullopt;
        acc = (acc << 5) | static_cast<uint32_t>(pos);
        bits += 5;
        ++symbols;
        if (bits >= 8) {
            payload.push_back(static_cast<uint8_t>(acc >> (bits - 8)));
            bits -= 8;
        }
    }
    if (symbols != 55 || payload.size() != 34) return std::nullopt;
    Key32 seed = to_key32(ByteView(payload).subspan(0, 32));
    auto check = recovery_check(seed);
    bool good = payload[32] == check[0] && payload[33] == check[1];
    sodium_memzero(payload.data(), payload.size());
    if (!good) return std::nullopt;
    return seed;
}

bool verify_device_cert(ByteView user_id, ByteView device_id, ByteView dh_key, ByteView cert) {
    if (device_id.size() != 32 || dh_key.size() != 32) return false;
    return verify_sig(user_id, signed_message(kCtxDeviceCert, {device_id, dh_key}), cert);
}

bool PeerBundle::verify() const {
    return verify_device_cert(user_id, device_id, dh_key, cert) &&
           verify_sig(device_id, signed_message(kCtxSignedPrekey, {spk}), spk_sig);
}

void Ratchet::write(Writer& w) const {
    w.raw(dhs.pk);
    w.raw(dhs.sk);
    w.u8(dhr ? 1 : 0);
    if (dhr) w.raw(*dhr);
    w.raw(rk);
    w.u8(cks ? 1 : 0);
    if (cks) w.raw(*cks);
    w.u8(ckr ? 1 : 0);
    if (ckr) w.raw(*ckr);
    w.u32(ns);
    w.u32(nr);
    w.u32(pn);
    w.u32(static_cast<uint32_t>(skipped.size()));
    for (const auto& [k, mk] : skipped) {
        w.raw(k.first);
        w.u32(k.second);
        w.raw(mk);
    }
    w.blob(ad_base);
    w.raw(x3dh_ek);
    w.u8(pending_prekey ? 1 : 0);
    w.u32(spk_id);
    w.u32(otk_id);
    w.u8(has_otk ? 1 : 0);
}

Ratchet Ratchet::read(Reader& r) {
    Ratchet s;
    s.dhs.pk = r.key32();
    s.dhs.sk = r.key32();
    if (r.u8()) s.dhr = r.key32();
    s.rk = r.key32();
    if (r.u8()) s.cks = r.key32();
    if (r.u8()) s.ckr = r.key32();
    s.ns = r.u32();
    s.nr = r.u32();
    s.pn = r.u32();
    uint32_t count = r.u32();
    if (count > kMaxSkippedStored + 1) throw CryptoError("corrupt session");
    for (uint32_t i = 0; i < count; ++i) {
        Key32 k = r.key32();
        uint32_t n = r.u32();
        s.skipped[{k, n}] = r.key32();
    }
    s.ad_base = r.blob();
    s.x3dh_ek = r.key32();
    s.pending_prekey = r.u8() != 0;
    s.spk_id = r.u32();
    s.otk_id = r.u32();
    s.has_otk = r.u8() != 0;
    return s;
}

Bytes PeerSessions::serialize() const {
    Writer w;
    w.u8(1);  // format version
    w.raw(user_id);
    w.raw(device_id);
    w.raw(dh_key);
    w.u32(static_cast<uint32_t>(active));
    w.u32(static_cast<uint32_t>(sessions.size()));
    for (const auto& s : sessions) s.write(w);
    return w.take();
}

PeerSessions PeerSessions::parse(ByteView data) {
    Reader r(data);
    if (r.u8() != 1) throw CryptoError("unknown session format");
    PeerSessions p;
    p.user_id = r.key32();
    p.device_id = r.key32();
    p.dh_key = r.key32();
    p.active = r.u32();
    uint32_t count = r.u32();
    if (count > kMaxSessionsPerPeer) throw CryptoError("corrupt session list");
    for (uint32_t i = 0; i < count; ++i) p.sessions.push_back(Ratchet::read(r));
    if (!p.sessions.empty() && p.active >= p.sessions.size()) p.active = p.sessions.size() - 1;
    return p;
}

namespace {

void add_session(PeerSessions& peer, Ratchet s, bool make_active) {
    if (peer.sessions.size() >= kMaxSessionsPerPeer) {
        peer.sessions.erase(peer.sessions.begin());
        if (peer.active > 0) --peer.active;
    }
    peer.sessions.push_back(std::move(s));
    if (make_active) peer.active = peer.sessions.size() - 1;
}

}  // namespace

void start_session(const Identity& me, const PeerBundle& b, PeerSessions& peer) {
    if (!b.verify()) throw CryptoError("prekey bundle failed verification");
    KeyPair ek = generate_dh();
    Key32 dh1 = dh(me.dh.sk, b.spk);
    Key32 dh2 = dh(ek.sk, b.dh_key);
    Key32 dh3 = dh(ek.sk, b.spk);
    Bytes sk = b.has_otk ? x3dh_secret({dh1, dh2, dh3, dh(ek.sk, b.otk)})
                         : x3dh_secret({dh1, dh2, dh3});

    Ratchet s;
    s.dhs = generate_dh();
    s.dhr = b.spk;
    auto [rk, cks] = kdf_rk(to_key32(sk), dh(s.dhs.sk, b.spk));
    s.rk = rk;
    s.cks = cks;
    append(s.ad_base, me.dh.pk);
    append(s.ad_base, b.dh_key);
    s.x3dh_ek = ek.pk;
    s.pending_prekey = true;
    s.spk_id = b.spk_id;
    s.has_otk = b.has_otk;
    s.otk_id = b.otk_id;

    sodium_memzero(sk.data(), sk.size());
    sodium_memzero(ek.sk.data(), ek.sk.size());

    peer.user_id = b.user_id;
    peer.device_id = b.device_id;
    peer.dh_key = b.dh_key;
    add_session(peer, std::move(s), true);
}

Bytes encrypt(const Identity& me, PeerSessions& peer, ByteView context, ByteView plaintext) {
    if (peer.sessions.empty()) throw CryptoError("no session with peer");
    Ratchet& s = peer.sessions[peer.active];
    if (!s.cks) throw CryptoError("session cannot send yet");

    auto [next, mk] = kdf_ck(*s.cks);
    s.cks = next;
    Header h{s.dhs.pk, s.pn, s.ns};
    ++s.ns;

    Writer w;
    w.u8(kVersion);
    w.u8(s.pending_prekey ? kTypePrekey : kTypeNormal);
    if (s.pending_prekey) {
        w.raw(me.dh.pk);
        w.blob(me.cert);
        w.raw(s.x3dh_ek);
        w.u32(s.spk_id);
        w.u8(s.has_otk ? 1 : 0);
        w.u32(s.otk_id);
    }
    w.raw(h.bytes());
    w.raw(seal(mk, plaintext, make_ad(s, context, h)));
    sodium_memzero(mk.data(), mk.size());
    return w.take();
}

std::optional<Bytes> decrypt(const Identity& me, PrekeySource& prekeys, PeerSessions& peer,
                             ByteView context, ByteView message) {
    try {
        Reader r(message);
        if (r.u8() != kVersion) return std::nullopt;
        uint8_t type = r.u8();
        if (type != kTypePrekey && type != kTypeNormal) return std::nullopt;

        std::optional<Ratchet> fresh;  // a responder session to create if this decrypts
        Key32 their_dh{}, their_ek{};
        if (type == kTypePrekey) {
            their_dh = r.key32();
            Bytes cert = r.blob();
            their_ek = r.key32();
            uint32_t spk_id = r.u32();
            bool has_otk = r.u8() != 0;
            uint32_t otk_id = r.u32();

            if (!verify_device_cert(peer.user_id, peer.device_id, their_dh, cert))
                return std::nullopt;
            // A peer we already know must keep the same identity key.
            if (!peer.sessions.empty() && peer.dh_key != their_dh) return std::nullopt;

            bool known = false;
            for (const auto& s : peer.sessions)
                if (s.x3dh_ek == their_ek) known = true;
            if (!known) {
                auto spk = prekeys.signed_prekey(spk_id);
                if (!spk) return std::nullopt;
                Key32 dh1 = dh(spk->sk, their_dh);
                Key32 dh2 = dh(me.dh.sk, their_ek);
                Key32 dh3 = dh(spk->sk, their_ek);
                Bytes sk;
                if (has_otk) {
                    auto otk = prekeys.take_one_time(otk_id);
                    if (!otk) return std::nullopt;
                    sk = x3dh_secret({dh1, dh2, dh3, dh(otk->sk, their_ek)});
                } else {
                    sk = x3dh_secret({dh1, dh2, dh3});
                }
                Ratchet s;
                s.dhs = *spk;
                s.rk = to_key32(sk);
                append(s.ad_base, their_dh);
                append(s.ad_base, me.dh.pk);
                s.x3dh_ek = their_ek;
                sodium_memzero(sk.data(), sk.size());
                fresh = std::move(s);
            }
        }

        Header h;
        h.dh = r.key32();
        h.pn = r.u32();
        h.n = r.u32();
        ByteView ciphertext = r.rest();

        if (fresh) {
            auto pt = ratchet_decrypt(*fresh, context, h, ciphertext);
            if (!pt) return std::nullopt;
            peer.dh_key = their_dh;
            add_session(peer, std::move(*fresh), true);
            return pt;
        }

        // Try the active session first, then the others.
        std::vector<size_t> order;
        if (!peer.sessions.empty()) order.push_back(peer.active);
        for (size_t i = peer.sessions.size(); i-- > 0;)
            if (i != peer.active) order.push_back(i);
        for (size_t i : order) {
            if (type == kTypePrekey && peer.sessions[i].x3dh_ek != their_ek) continue;
            Ratchet copy = peer.sessions[i];
            std::optional<Bytes> pt;
            try {
                pt = ratchet_decrypt(copy, context, h, ciphertext);
            } catch (const CryptoError&) {
                continue;
            }
            if (!pt) continue;
            copy.pending_prekey = false;  // the peer has our session now
            peer.sessions[i] = std::move(copy);
            peer.active = i;
            return pt;
        }
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;  // truncated or malformed input
    }
}

}  // namespace corded::crypto

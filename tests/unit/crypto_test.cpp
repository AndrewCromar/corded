#include "crypto/crypto.hpp"

#include "corded/common/frame.hpp"
#include "corded/common/sig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <random>

using namespace corded;
using namespace corded::crypto;

namespace {

// A device with its published prekeys kept in memory.
struct Device : PrekeySource {
    Identity id = Identity::generate();
    std::map<uint32_t, KeyPair> signed_keys, one_time;
    uint32_t next_id = 1;

    PeerBundle bundle(bool with_otk = true) {
        PeerBundle b;
        b.user_id = id.user.pk;
        b.device_id = id.device.pk;
        b.dh_key = id.dh.pk;
        b.cert = id.cert;
        KeyPair spk = generate_dh();
        b.spk_id = next_id++;
        signed_keys[b.spk_id] = spk;
        b.spk = spk.pk;
        b.spk_sig = sign(id.device.sk, signed_message(kCtxSignedPrekey, {spk.pk}));
        if (with_otk) {
            KeyPair otk = generate_dh();
            b.has_otk = true;
            b.otk_id = next_id++;
            one_time[b.otk_id] = otk;
            b.otk = otk.pk;
        }
        return b;
    }
    std::optional<KeyPair> signed_prekey(uint32_t i) override {
        auto it = signed_keys.find(i);
        return it == signed_keys.end() ? std::nullopt : std::optional(it->second);
    }
    std::optional<KeyPair> take_one_time(uint32_t i) override {
        auto it = one_time.find(i);
        if (it == one_time.end()) return std::nullopt;
        KeyPair kp = it->second;
        one_time.erase(it);
        return kp;
    }
    // The receiving side's view of a peer before any session exists.
    PeerSessions peer_of(const Device& other) const {
        PeerSessions p;
        p.user_id = other.id.user.pk;
        p.device_id = other.id.device.pk;
        return p;
    }
};

Bytes B(std::string_view s) { return to_bytes(s); }
const Bytes ctx = B("room-and-event-id");

}  // namespace

TEST_CASE("padding round-trips and lands on bucket sizes") {
    REQUIRE(sodium_init() >= 0);
    for (size_t n : {0u, 1u, 100u, 255u, 256u, 1000u, 4095u, 4096u, 10000u}) {
        Bytes pt(n, 0x41);
        Bytes padded = pad(pt);
        REQUIRE(padded.size() > n);
        bool bucket = padded.size() == 256 || padded.size() == 512 || padded.size() == 1024 ||
                      padded.size() == 2048 || padded.size() % 4096 == 0;
        REQUIRE(bucket);
        REQUIRE(unpad(padded) == pt);
    }
    REQUIRE_FALSE(unpad(Bytes(256, 0)).has_value());
}

TEST_CASE("a conversation works in both directions") {
    Device alice, bob;
    PeerSessions a_bob, b_alice = bob.peer_of(alice);
    start_session(alice.id, bob.bundle(), a_bob);

    Bytes m1 = encrypt(alice.id, a_bob, ctx, B("hello bob"));
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, m1) == B("hello bob"));
    REQUIRE(bob.one_time.empty());  // the one-time prekey was consumed

    Bytes m2 = encrypt(bob.id, b_alice, ctx, B("hello alice"));
    REQUIRE(decrypt(alice.id, alice, a_bob, ctx, m2) == B("hello alice"));

    for (int i = 0; i < 20; ++i) {
        Bytes a = encrypt(alice.id, a_bob, ctx, B("a" + std::to_string(i)));
        REQUIRE(decrypt(bob.id, bob, b_alice, ctx, a) == B("a" + std::to_string(i)));
        Bytes b = encrypt(bob.id, b_alice, ctx, B("b" + std::to_string(i)));
        REQUIRE(decrypt(alice.id, alice, a_bob, ctx, b) == B("b" + std::to_string(i)));
    }
}

TEST_CASE("sessions start without a one-time prekey") {
    Device alice, bob;
    PeerSessions a_bob, b_alice = bob.peer_of(alice);
    start_session(alice.id, bob.bundle(false), a_bob);
    Bytes m = encrypt(alice.id, a_bob, ctx, B("no otk"));
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, m) == B("no otk"));
}

TEST_CASE("several messages before the first reply all decrypt") {
    Device alice, bob;
    PeerSessions a_bob, b_alice = bob.peer_of(alice);
    start_session(alice.id, bob.bundle(), a_bob);
    std::vector<Bytes> msgs;
    for (int i = 0; i < 5; ++i) msgs.push_back(encrypt(alice.id, a_bob, ctx, B("m" + std::to_string(i))));
    for (int i = 0; i < 5; ++i)
        REQUIRE(decrypt(bob.id, bob, b_alice, ctx, msgs[i]) == B("m" + std::to_string(i)));
    REQUIRE(b_alice.sessions.size() == 1);
}

TEST_CASE("out-of-order delivery, replays and tampering") {
    Device alice, bob;
    PeerSessions a_bob, b_alice = bob.peer_of(alice);
    start_session(alice.id, bob.bundle(), a_bob);
    Bytes first = encrypt(alice.id, a_bob, ctx, B("first"));
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, first).has_value());
    Bytes reply = encrypt(bob.id, b_alice, ctx, B("reply"));
    REQUIRE(decrypt(alice.id, alice, a_bob, ctx, reply).has_value());

    Bytes m0 = encrypt(alice.id, a_bob, ctx, B("zero"));
    Bytes m1 = encrypt(alice.id, a_bob, ctx, B("one"));
    Bytes m2 = encrypt(alice.id, a_bob, ctx, B("two"));

    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, m2) == B("two"));
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, m0) == B("zero"));
    // A replay must fail and must not disturb the session.
    REQUIRE_FALSE(decrypt(bob.id, bob, b_alice, ctx, m2).has_value());
    REQUIRE_FALSE(decrypt(bob.id, bob, b_alice, ctx, m0).has_value());

    Bytes tampered = m1;
    tampered.back() ^= 1;
    REQUIRE_FALSE(decrypt(bob.id, bob, b_alice, ctx, tampered).has_value());
    // The same ciphertext presented for a different room or event must fail.
    REQUIRE_FALSE(decrypt(bob.id, bob, b_alice, B("another-context"), m1).has_value());
    // After those failures the genuine message still decrypts.
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, m1) == B("one"));

    REQUIRE_FALSE(decrypt(bob.id, bob, b_alice, ctx, Bytes{}).has_value());
    REQUIRE_FALSE(decrypt(bob.id, bob, b_alice, ctx, Bytes(10, 7)).has_value());
}

TEST_CASE("a forged bundle or certificate is rejected") {
    Device alice, bob, mallory;
    PeerBundle b = bob.bundle();
    b.spk = generate_dh().pk;  // signature no longer matches
    PeerSessions s;
    REQUIRE_THROWS_AS(start_session(alice.id, b, s), CryptoError);

    PeerBundle swapped = bob.bundle();
    swapped.dh_key = mallory.id.dh.pk;  // not covered by bob's certificate
    REQUIRE_THROWS_AS(start_session(alice.id, swapped, s), CryptoError);

    // Mallory sends a first message claiming to be Alice.
    PeerSessions m_bob;
    start_session(mallory.id, bob.bundle(), m_bob);
    Bytes forged = encrypt(mallory.id, m_bob, ctx, B("i am alice"));
    PeerSessions b_alice = bob.peer_of(alice);
    REQUIRE_FALSE(decrypt(bob.id, bob, b_alice, ctx, forged).has_value());
    REQUIRE(b_alice.sessions.empty());
}

TEST_CASE("session state survives serialisation") {
    Device alice, bob;
    PeerSessions a_bob, b_alice = bob.peer_of(alice);
    start_session(alice.id, bob.bundle(), a_bob);
    Bytes m = encrypt(alice.id, a_bob, ctx, B("one"));
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, m).has_value());
    Bytes skipped = encrypt(alice.id, a_bob, ctx, B("skipped"));
    Bytes later = encrypt(alice.id, a_bob, ctx, B("later"));
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, later).has_value());

    a_bob = PeerSessions::parse(a_bob.serialize());
    b_alice = PeerSessions::parse(b_alice.serialize());
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, skipped) == B("skipped"));
    Bytes back = encrypt(bob.id, b_alice, ctx, B("back"));
    REQUIRE(decrypt(alice.id, alice, a_bob, ctx, back) == B("back"));
}

TEST_CASE("both sides starting at once still converge") {
    Device alice, bob;
    PeerSessions a_bob, b_alice;
    start_session(alice.id, bob.bundle(), a_bob);
    start_session(bob.id, alice.bundle(), b_alice);
    Bytes from_a = encrypt(alice.id, a_bob, ctx, B("from a"));
    Bytes from_b = encrypt(bob.id, b_alice, ctx, B("from b"));
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, from_a) == B("from a"));
    REQUIRE(decrypt(alice.id, alice, a_bob, ctx, from_b) == B("from b"));
    for (int i = 0; i < 10; ++i) {
        Bytes a = encrypt(alice.id, a_bob, ctx, B("a"));
        REQUIRE(decrypt(bob.id, bob, b_alice, ctx, a) == B("a"));
        Bytes b = encrypt(bob.id, b_alice, ctx, B("b"));
        REQUIRE(decrypt(alice.id, alice, a_bob, ctx, b) == B("b"));
    }
}

TEST_CASE("a compromised state stops working after the ratchet turns") {
    Device alice, bob;
    PeerSessions a_bob, b_alice = bob.peer_of(alice);
    start_session(alice.id, bob.bundle(), a_bob);
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, encrypt(alice.id, a_bob, ctx, B("1"))).has_value());
    REQUIRE(decrypt(alice.id, alice, a_bob, ctx, encrypt(bob.id, b_alice, ctx, B("2"))).has_value());

    PeerSessions stolen = PeerSessions::parse(b_alice.serialize());  // attacker copies Bob's state
    // One full round trip between the real parties.
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, encrypt(alice.id, a_bob, ctx, B("3"))).has_value());
    REQUIRE(decrypt(alice.id, alice, a_bob, ctx, encrypt(bob.id, b_alice, ctx, B("4"))).has_value());
    Bytes secret = encrypt(alice.id, a_bob, ctx, B("after healing"));
    REQUIRE_FALSE(decrypt(bob.id, bob, stolen, ctx, secret).has_value());
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, secret) == B("after healing"));
}

TEST_CASE("randomised conversation with loss, reordering and duplicates") {
    Device alice, bob;
    PeerSessions a_bob, b_alice = bob.peer_of(alice);
    start_session(alice.id, bob.bundle(), a_bob);
    REQUIRE(decrypt(bob.id, bob, b_alice, ctx, encrypt(alice.id, a_bob, ctx, B("init"))).has_value());

    std::mt19937 rng(12345);
    struct Pending { bool to_bob; Bytes ct, pt; };
    std::vector<Pending> in_flight;
    int delivered = 0;
    for (int step = 0; step < 4000; ++step) {
        int action = static_cast<int>(rng() % 10);
        if (action < 5) {
            bool from_alice = rng() % 2 == 0;
            Bytes pt = B("msg " + std::to_string(step));
            Bytes ct = from_alice ? encrypt(alice.id, a_bob, ctx, pt) : encrypt(bob.id, b_alice, ctx, pt);
            if (rng() % 20 != 0) in_flight.push_back({from_alice, ct, pt});  // 5% are lost
        } else if (!in_flight.empty()) {
            size_t i = rng() % std::min<size_t>(in_flight.size(), 8);  // bounded reordering
            Pending p = in_flight[i];
            in_flight.erase(in_flight.begin() + static_cast<long>(i));
            auto got = p.to_bob ? decrypt(bob.id, bob, b_alice, ctx, p.ct)
                                : decrypt(alice.id, alice, a_bob, ctx, p.ct);
            REQUIRE(got == p.pt);
            ++delivered;
            if (rng() % 10 == 0) {  // a duplicate must be rejected
                auto again = p.to_bob ? decrypt(bob.id, bob, b_alice, ctx, p.ct)
                                      : decrypt(alice.id, alice, a_bob, ctx, p.ct);
                REQUIRE_FALSE(again.has_value());
            }
        }
    }
    REQUIRE(delivered > 1000);
}

TEST_CASE("safety numbers match on both sides and differ between people") {
    Device alice, bob, carol;
    std::string ab = safety_number(alice.id.user.pk, bob.id.user.pk);
    REQUIRE(ab == safety_number(bob.id.user.pk, alice.id.user.pk));
    REQUIRE(ab != safety_number(alice.id.user.pk, carol.id.user.pk));
    REQUIRE(ab.size() == 12 * 5 + 11);
    for (size_t i = 0; i < ab.size(); ++i) {
        if (i % 6 == 5) REQUIRE(ab[i] == ' ');
        else REQUIRE((ab[i] >= '0' && ab[i] <= '9'));
    }
}

TEST_CASE("a recovery key recreates the same person on another device") {
    Identity first = Identity::generate();
    std::string key = encode_recovery_key(first.seed());
    // 55 symbols in groups of five.
    REQUIRE(key.size() == 55 + 10);
    auto seed = decode_recovery_key(key);
    REQUIRE(seed.has_value());
    Identity second = Identity::from_seed(*seed);
    REQUIRE(second.user.pk == first.user.pk);        // the same person
    REQUIRE(second.device.pk != first.device.pk);    // a different device
    REQUIRE(second.dh.pk != first.dh.pk);
    REQUIRE(verify_device_cert(first.user.pk, second.device.pk, second.dh.pk, second.cert));

    // Forgiving about how it is typed, strict about what it says.
    std::string sloppy;
    for (char ch : key) sloppy += ch == '-' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    REQUIRE(decode_recovery_key(sloppy) == seed);
    std::string wrong = key;
    wrong[0] = wrong[0] == 'A' ? 'B' : 'A';
    REQUIRE_FALSE(decode_recovery_key(wrong).has_value());
    REQUIRE_FALSE(decode_recovery_key(key.substr(0, 40)).has_value());
    REQUIRE_FALSE(decode_recovery_key("").has_value());
    REQUIRE_FALSE(decode_recovery_key("not a recovery key at all!").has_value());
    REQUIRE(encode_recovery_key(Identity::generate().seed()) != key);
}

#include "corded/common/release.hpp"

TEST_CASE("a release is accepted only with the release key's signature") {
    REQUIRE(sodium_init() >= 0);
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);
    // The public key as openssl writes it.
    corded::Bytes der = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
    der.insert(der.end(), pk, pk + sizeof pk);
    std::string line(sodium_base64_encoded_len(der.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(line.data(), line.size(), der.data(), der.size(), sodium_base64_VARIANT_ORIGINAL);
    line.resize(std::strlen(line.c_str()));
    auto key = corded::release::public_key_from_pem("-----BEGIN PUBLIC KEY-----\n" + line + "\n-----END PUBLIC KEY-----\n");
    REQUIRE(key.has_value());
    REQUIRE(std::equal(key->begin(), key->end(), pk));
    REQUIRE(corded::release::public_key_from_pem(line).has_value());  // the bare line works too
    REQUIRE_FALSE(corded::release::public_key_from_pem("not a key").has_value());

    corded::Bytes archive(5000, 0x42), signature(crypto_sign_BYTES);
    crypto_sign_detached(signature.data(), nullptr, archive.data(), archive.size(), sk);
    REQUIRE(corded::release::signed_by(archive, signature, *key));
    archive[100] ^= 1;  // one bit of the archive changed
    REQUIRE_FALSE(corded::release::signed_by(archive, signature, *key));
    archive[100] ^= 1;
    signature[0] ^= 1;
    REQUIRE_FALSE(corded::release::signed_by(archive, signature, *key));
    REQUIRE_FALSE(corded::release::signed_by(archive, corded::Bytes(10), *key));

    // A signature made by openssl, as the build system makes them, with a throwaway key.
    {
        auto theirs = corded::release::public_key_from_pem("MCowBQYDK2VwAyEAg6Fv/KvCVW84bfNgEkuFrM76LnVeIpKFH2Zhus+yaNg=");
        REQUIRE(theirs.has_value());
        std::string text = "corded release fixture: these bytes are signed by a throwaway key\n";
        std::string encoded = "dW8PEKCeCcI5rlXgnFVjmbuAXVWqdXqpok0qzxAeIYou9SLUrqQkOBZFg+LMMeVkn0rqfwrvRMndOOhQEOj2Ag==";
        corded::Bytes made(crypto_sign_BYTES);
        size_t len = 0;
        REQUIRE(sodium_base642bin(made.data(), made.size(), encoded.data(), encoded.size(), nullptr, &len, nullptr,
                                  sodium_base64_VARIANT_ORIGINAL) == 0);
        REQUIRE(len == crypto_sign_BYTES);
        REQUIRE(corded::release::signed_by(corded::Bytes(text.begin(), text.end()), made, *theirs));
    }
    REQUIRE(corded::release::release_of("v0.5.0-3-gabc1234") == "v0.5.0");
    REQUIRE(corded::release::release_of("v0.5.0") == "v0.5.0");
}

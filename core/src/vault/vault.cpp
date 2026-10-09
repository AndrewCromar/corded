#include "vault/vault.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace corded {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view kWrapAd = "corded/v1/vault-key";

std::string header_path(const std::string& dir) { return dir + "/vault.hdr"; }
std::string db_path(const std::string& dir) { return dir + "/vault.db"; }

Key32 derive_kek(ByteView passphrase, ByteView salt, uint64_t ops, uint64_t mem) {
    Key32 kek;
    if (salt.size() != crypto_pwhash_SALTBYTES) throw VaultError("corrupt vault header");
    if (crypto_pwhash(kek.data(), kek.size(), reinterpret_cast<const char*>(passphrase.data()),
                      passphrase.size(), salt.data(), ops, static_cast<size_t>(mem),
                      crypto_pwhash_ALG_ARGON2ID13) != 0)
        throw VaultError("key derivation failed (out of memory?)");
    return kek;
}

EventRow read_event(db::Statement& st) {
    EventRow e;
    e.room_id = st.blob(0);
    e.event_id = st.blob(1);
    if (!st.is_null(2)) e.seq = st.u64(2);
    e.type = st.text(3);
    e.type_version = static_cast<uint16_t>(st.i64(4));
    e.sender_user = st.blob(5);
    e.sender_device = st.blob(6);
    e.origin_ts = st.u64(7);
    e.server_ts = st.u64(8);
    e.state_key = st.text(9);
    e.content = st.text(10);
    e.fallback_text = st.text(11);
    e.status = st.text(12);
    e.rel_kind = st.text(13);
    e.rel_target = st.blob(14);
    e.rel_key = st.text(15);
    return e;
}

constexpr const char* kEventColumns =
    "room_id, event_id, seq, type, type_version, sender_user, sender_device, origin_ts, "
    "server_ts, state_key, content, fallback_text, status, rel_kind, rel_target, rel_key";

}  // namespace

bool Vault::exists(const std::string& dir) {
    return fs::exists(header_path(dir)) && fs::exists(db_path(dir));
}

void Vault::create(const std::string& dir, ByteView passphrase, const std::string& username,
                   bool fast_kdf) {
    if (exists(dir)) throw VaultError("a vault already exists here");
    fs::create_directories(dir);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);

    uint64_t ops = fast_kdf ? crypto_pwhash_OPSLIMIT_MIN : crypto_pwhash_OPSLIMIT_MODERATE;
    uint64_t mem = fast_kdf ? crypto_pwhash_MEMLIMIT_MIN : crypto_pwhash_MEMLIMIT_MODERATE;
    Bytes salt = random_bytes(crypto_pwhash_SALTBYTES);
    Bytes nonce = random_bytes(24);
    Key32 vault_key;
    randombytes_buf(vault_key.data(), vault_key.size());
    Key32 kek = derive_kek(passphrase, salt, ops, mem);
    Bytes wrapped = crypto::aead_encrypt(kek, nonce, vault_key, to_bytes(kWrapAd));
    sodium_memzero(kek.data(), kek.size());

    // Remove any half-made database from an earlier failed attempt.
    std::error_code ec;
    fs::remove(db_path(dir), ec);
    open_database(dir, vault_key);
    sodium_memzero(vault_key.data(), vault_key.size());
    migrate();

    identity_ = crypto::Identity::generate();
    username_ = username;
    {
        db::Transaction tx(db_);
        auto st = db_.prepare("INSERT INTO identity (id, username, user_pk, user_sk, device_pk, "
                              "device_sk, dh_pk, dh_sk, cert) VALUES (1,?,?,?,?,?,?,?,?)");
        st.bind(1, username)
            .bind(2, identity_.user.pk)
            .bind(3, ByteView(identity_.user.sk))
            .bind(4, identity_.device.pk)
            .bind(5, ByteView(identity_.device.sk))
            .bind(6, identity_.dh.pk)
            .bind(7, identity_.dh.sk)
            .bind(8, identity_.cert)
            .exec();
        tx.commit();
    }

    // The header goes last, written atomically, so a crash never leaves a
    // header that points at a database without an identity.
    json hdr = {{"version", 1},       {"salt", b64(salt)},   {"opslimit", ops},
                {"memlimit", mem},    {"nonce", b64(nonce)}, {"wrapped_key", b64(wrapped)}};
    std::string tmp = header_path(dir) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << hdr.dump(2) << "\n";
        if (!out) throw VaultError("cannot write vault header");
    }
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    fs::rename(tmp, header_path(dir));
}

void Vault::unlock(const std::string& dir, ByteView passphrase) {
    if (!exists(dir)) throw VaultError("no vault found");
    json hdr;
    try {
        std::ifstream in(header_path(dir), std::ios::binary);
        hdr = json::parse(in);
    } catch (const std::exception&) {
        throw VaultError("corrupt vault header");
    }
    auto salt = unb64(hdr.value("salt", ""));
    auto nonce = unb64(hdr.value("nonce", ""));
    auto wrapped = unb64(hdr.value("wrapped_key", ""));
    if (!salt || !nonce || !wrapped || hdr.value("version", 0) != 1)
        throw VaultError("corrupt vault header");

    Key32 kek = derive_kek(passphrase, *salt, hdr.value("opslimit", uint64_t{0}),
                           hdr.value("memlimit", uint64_t{0}));
    auto key = crypto::aead_decrypt(kek, *nonce, *wrapped, to_bytes(kWrapAd));
    sodium_memzero(kek.data(), kek.size());
    if (!key || key->size() != 32) throw WrongPassphrase();
    Key32 vault_key = to_key32(*key);
    sodium_memzero(key->data(), key->size());

    open_database(dir, vault_key);
    sodium_memzero(vault_key.data(), vault_key.size());
    migrate();
    load_identity();
}

void Vault::lock() {
    db_.close();
    sodium_memzero(&identity_.user.sk, sizeof identity_.user.sk);
    sodium_memzero(&identity_.device.sk, sizeof identity_.device.sk);
    sodium_memzero(&identity_.dh.sk, sizeof identity_.dh.sk);
}

void Vault::open_database(const std::string& dir, const Key32& vault_key) {
    db_.open(db_path(dir));
    std::string key_hex = hex(vault_key);
    int rc = sqlite3_key(db_.raw(), key_hex.data(), static_cast<int>(key_hex.size()));
    sodium_memzero(key_hex.data(), key_hex.size());
    if (rc != SQLITE_OK) {
        db_.close();
        throw VaultError("cannot set the database key");
    }
    try {
        db_.exec("SELECT count(*) FROM sqlite_master;");
        db_.exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA secure_delete=ON;");
    } catch (const db::Error&) {
        db_.close();
        throw VaultError("the vault database cannot be decrypted");
    }
}

void Vault::migrate() {
    db_.exec(R"sql(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS identity (
    id INTEGER PRIMARY KEY CHECK (id = 1), username TEXT NOT NULL,
    user_pk BLOB NOT NULL, user_sk BLOB NOT NULL,
    device_pk BLOB NOT NULL, device_sk BLOB NOT NULL,
    dh_pk BLOB NOT NULL, dh_sk BLOB NOT NULL, cert BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS prekeys (
    key_id INTEGER PRIMARY KEY, kind INTEGER NOT NULL,
    public_key BLOB NOT NULL, secret_key BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
    peer_user BLOB PRIMARY KEY, peer_device BLOB NOT NULL, state BLOB NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS rooms (
    room_id BLOB PRIMARY KEY, acked_seq INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS members (
    room_id BLOB NOT NULL, user_id BLOB NOT NULL, username TEXT NOT NULL,
    PRIMARY KEY (room_id, user_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS events (
    room_id BLOB NOT NULL, event_id BLOB NOT NULL, seq INTEGER,
    type TEXT NOT NULL, type_version INTEGER NOT NULL,
    sender_user BLOB NOT NULL, sender_device BLOB NOT NULL,
    origin_ts INTEGER NOT NULL, server_ts INTEGER NOT NULL DEFAULT 0,
    state_key TEXT NOT NULL DEFAULT '', content TEXT NOT NULL DEFAULT '',
    fallback_text TEXT NOT NULL DEFAULT '', status TEXT NOT NULL,
    rel_kind TEXT NOT NULL DEFAULT '', rel_target BLOB, rel_key TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (room_id, event_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS events_by_seq ON events(room_id, seq);
-- Generic index of relations: knows nothing about what a kind means, so new
-- features (threads, reactions, edits) need no schema change.
CREATE TABLE IF NOT EXISTS relations (
    room_id BLOB NOT NULL, target_event_id BLOB NOT NULL, kind TEXT NOT NULL,
    event_id BLOB NOT NULL, rel_key TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (room_id, target_event_id, kind, event_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS outbox (
    local_id INTEGER PRIMARY KEY AUTOINCREMENT, room_id BLOB NOT NULL, event_id BLOB NOT NULL
);
)sql");
    db_.exec("CREATE TABLE IF NOT EXISTS verified_users ("
             "user_id BLOB PRIMARY KEY, verified_at INTEGER NOT NULL) WITHOUT ROWID");
    // Added after the first prototype: rooms can carry a name.
    try {
        db_.exec("SELECT name FROM rooms LIMIT 0");
    } catch (const db::Error&) {
        db_.exec("ALTER TABLE rooms ADD COLUMN name TEXT NOT NULL DEFAULT ''");
    }
}

void Vault::load_identity() {
    auto st = db_.prepare("SELECT username, user_pk, user_sk, device_pk, device_sk, dh_pk, dh_sk, "
                          "cert FROM identity WHERE id = 1");
    if (!st.step()) throw VaultError("the vault has no identity");
    username_ = st.text(0);
    auto copy = [](const Bytes& src, auto& dst) {
        if (src.size() != dst.size()) throw VaultError("corrupt identity");
        std::memcpy(dst.data(), src.data(), dst.size());
    };
    copy(st.blob(1), identity_.user.pk);
    copy(st.blob(2), identity_.user.sk);
    copy(st.blob(3), identity_.device.pk);
    copy(st.blob(4), identity_.device.sk);
    copy(st.blob(5), identity_.dh.pk);
    copy(st.blob(6), identity_.dh.sk);
    identity_.cert = st.blob(7);
}

std::optional<std::string> Vault::meta(const std::string& key) {
    auto st = db_.prepare("SELECT value FROM meta WHERE key = ?");
    st.bind(1, key);
    if (!st.step()) return std::nullopt;
    return st.text(0);
}

void Vault::set_meta(const std::string& key, const std::string& value) {
    auto st = db_.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)");
    st.bind(1, key).bind(2, value).exec();
}

crypto::KeyPair Vault::add_prekey(int kind, uint32_t id) {
    crypto::KeyPair kp = crypto::generate_dh();
    auto st = db_.prepare("INSERT INTO prekeys (key_id, kind, public_key, secret_key) "
                          "VALUES (?,?,?,?)");
    st.bind(1, id).bind(2, kind).bind(3, kp.pk).bind(4, kp.sk).exec();
    set_meta("prekey_next_id", std::to_string(id + 1));
    return kp;
}

uint32_t Vault::next_prekey_id() {
    auto st = db_.prepare("SELECT COALESCE(MAX(key_id), 0) + 1 FROM prekeys");
    st.step();
    uint32_t next = static_cast<uint32_t>(st.i64(0));
    // Ids are never reused, even after one-time prekeys are consumed.
    if (auto stored = meta("prekey_next_id")) next = std::max(next, static_cast<uint32_t>(std::stoul(*stored)));
    return next;
}

std::optional<crypto::KeyPair> Vault::signed_prekey(uint32_t id) {
    auto st = db_.prepare("SELECT public_key, secret_key FROM prekeys WHERE key_id = ? AND kind = 0");
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return crypto::KeyPair{to_key32(st.blob(0)), to_key32(st.blob(1))};
}

std::optional<crypto::KeyPair> Vault::take_one_time(uint32_t id) {
    std::optional<crypto::KeyPair> kp;
    {
        auto st = db_.prepare("SELECT public_key, secret_key FROM prekeys "
                              "WHERE key_id = ? AND kind = 1");
        st.bind(1, id);
        if (!st.step()) return std::nullopt;
        kp = crypto::KeyPair{to_key32(st.blob(0)), to_key32(st.blob(1))};
    }
    auto del = db_.prepare("DELETE FROM prekeys WHERE key_id = ? AND kind = 1");
    del.bind(1, id).exec();
    return kp;
}

std::optional<crypto::PeerSessions> Vault::load_sessions(ByteView peer_user) {
    auto st = db_.prepare("SELECT state FROM sessions WHERE peer_user = ?");
    st.bind(1, peer_user);
    if (!st.step()) return std::nullopt;
    return crypto::PeerSessions::parse(st.blob(0));
}

void Vault::save_sessions(const crypto::PeerSessions& peer) {
    auto st = db_.prepare("INSERT OR REPLACE INTO sessions (peer_user, peer_device, state) "
                          "VALUES (?,?,?)");
    st.bind(1, peer.user_id).bind(2, peer.device_id).bind(3, peer.serialize()).exec();
}

bool Vault::is_verified(ByteView user_id) {
    auto st = db_.prepare("SELECT 1 FROM verified_users WHERE user_id = ?");
    st.bind(1, user_id);
    return st.step();
}

void Vault::set_verified(ByteView user_id, bool verified) {
    if (verified) {
        auto st = db_.prepare("INSERT OR IGNORE INTO verified_users (user_id, verified_at) "
                              "VALUES (?, strftime('%s','now'))");
        st.bind(1, user_id).exec();
    } else {
        auto st = db_.prepare("DELETE FROM verified_users WHERE user_id = ?");
        st.bind(1, user_id).exec();
    }
}

void Vault::upsert_room(ByteView room_id, const std::vector<MemberRow>& members) {
    auto st = db_.prepare("INSERT OR IGNORE INTO rooms (room_id) VALUES (?)");
    st.bind(1, room_id).exec();
    if (members.empty()) return;
    auto del = db_.prepare("DELETE FROM members WHERE room_id = ?");
    del.bind(1, room_id).exec();
    for (const auto& m : members) {
        auto ins = db_.prepare("INSERT OR REPLACE INTO members (room_id, user_id, username) "
                               "VALUES (?,?,?)");
        ins.bind(1, room_id).bind(2, m.user_id).bind(3, m.username).exec();
    }
}

std::optional<RoomRow> Vault::room(ByteView room_id) {
    RoomRow r;
    {
        auto st = db_.prepare("SELECT acked_seq, name FROM rooms WHERE room_id = ?");
        st.bind(1, room_id);
        if (!st.step()) return std::nullopt;
        r.room_id = to_bytes(room_id);
        r.acked_seq = st.u64(0);
        r.name = st.text(1);
    }
    auto st = db_.prepare("SELECT user_id, username FROM members WHERE room_id = ? ORDER BY username");
    st.bind(1, room_id);
    while (st.step()) r.members.push_back({st.blob(0), st.text(1)});
    return r;
}

std::vector<RoomRow> Vault::rooms() {
    std::vector<Bytes> ids;
    {
        auto st = db_.prepare("SELECT room_id FROM rooms");
        while (st.step()) ids.push_back(st.blob(0));
    }
    std::vector<RoomRow> out;
    for (const auto& id : ids)
        if (auto r = room(id)) out.push_back(std::move(*r));
    return out;
}

void Vault::advance_cursor(ByteView room_id, uint64_t seq) {
    auto st = db_.prepare("UPDATE rooms SET acked_seq = MAX(acked_seq, ?) WHERE room_id = ?");
    st.bind(1, seq).bind(2, room_id).exec();
}

void Vault::set_room_name(ByteView room_id, const std::string& name) {
    auto st = db_.prepare("UPDATE rooms SET name = ? WHERE room_id = ?");
    st.bind(1, name).bind(2, room_id).exec();
}

bool Vault::insert_event(const EventRow& e) {
    auto st = db_.prepare(
        "INSERT OR IGNORE INTO events (room_id, event_id, seq, type, type_version, sender_user, "
        "sender_device, origin_ts, server_ts, state_key, content, fallback_text, status, rel_kind, "
        "rel_target, rel_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    st.bind(1, e.room_id).bind(2, e.event_id);
    if (e.seq) st.bind(3, *e.seq);
    else st.bind_null(3);
    st.bind(4, e.type).bind(5, static_cast<int>(e.type_version)).bind(6, e.sender_user)
        .bind(7, e.sender_device).bind(8, e.origin_ts).bind(9, e.server_ts).bind(10, e.state_key)
        .bind(11, e.content).bind(12, e.fallback_text).bind(13, e.status).bind(14, e.rel_kind);
    if (e.rel_target.empty()) st.bind_null(15);
    else st.bind(15, e.rel_target);
    st.bind(16, e.rel_key).exec();
    if (db_.changes() == 0) return false;
    if (!e.rel_kind.empty() && !e.rel_target.empty()) {
        auto rel = db_.prepare("INSERT OR IGNORE INTO relations (room_id, target_event_id, kind, "
                               "event_id, rel_key) VALUES (?,?,?,?,?)");
        rel.bind(1, e.room_id).bind(2, e.rel_target).bind(3, e.rel_kind).bind(4, e.event_id)
            .bind(5, e.rel_key).exec();
    }
    return true;
}

bool Vault::has_event(ByteView room_id, ByteView event_id) {
    auto st = db_.prepare("SELECT 1 FROM events WHERE room_id = ? AND event_id = ?");
    st.bind(1, room_id).bind(2, event_id);
    return st.step();
}

std::optional<EventRow> Vault::event(ByteView room_id, ByteView event_id) {
    std::string sql = std::string("SELECT ") + kEventColumns +
                      " FROM events WHERE room_id = ? AND event_id = ?";
    auto st = db_.prepare(sql.c_str());
    st.bind(1, room_id).bind(2, event_id);
    if (!st.step()) return std::nullopt;
    return read_event(st);
}

void Vault::confirm_event(ByteView room_id, ByteView event_id, uint64_t seq, uint64_t server_ts) {
    auto st = db_.prepare("UPDATE events SET seq = ?, server_ts = ?, status = 'ok' "
                          "WHERE room_id = ? AND event_id = ?");
    st.bind(1, seq).bind(2, server_ts).bind(3, room_id).bind(4, event_id).exec();
}

void Vault::set_event_status(ByteView room_id, ByteView event_id, const std::string& status) {
    auto st = db_.prepare("UPDATE events SET status = ? WHERE room_id = ? AND event_id = ?");
    st.bind(1, status).bind(2, room_id).bind(3, event_id).exec();
}

std::vector<EventRow> Vault::timeline(ByteView room_id, uint32_t limit) {
    // Newest `limit` events, returned oldest first. Unsent events sort last.
    std::string sql = std::string("SELECT ") + kEventColumns +
                      " FROM events WHERE room_id = ? "
                      "ORDER BY (seq IS NULL) DESC, seq DESC, origin_ts DESC LIMIT ?";
    auto st = db_.prepare(sql.c_str());
    st.bind(1, room_id).bind(2, limit);
    std::vector<EventRow> out;
    while (st.step()) out.push_back(read_event(st));
    std::reverse(out.begin(), out.end());
    return out;
}

void Vault::outbox_push(ByteView room_id, ByteView event_id) {
    auto st = db_.prepare("INSERT INTO outbox (room_id, event_id) VALUES (?, ?)");
    st.bind(1, room_id).bind(2, event_id).exec();
}

std::vector<OutboxRow> Vault::outbox() {
    std::vector<OutboxRow> out;
    auto st = db_.prepare("SELECT local_id, room_id, event_id FROM outbox ORDER BY local_id");
    while (st.step()) out.push_back({st.i64(0), st.blob(1), st.blob(2)});
    return out;
}

void Vault::outbox_remove(int64_t local_id) {
    auto st = db_.prepare("DELETE FROM outbox WHERE local_id = ?");
    st.bind(1, local_id).exec();
}

}  // namespace corded

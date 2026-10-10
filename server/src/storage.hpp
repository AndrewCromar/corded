// cordedd storage: SQLite in WAL mode. The server only ever stores public
// keys, membership and ciphertext.
#pragma once

#include "corded/common/db.hpp"
#include "corded/common/frame.hpp"
#include "corded/common/perms.hpp"

#include <set>

#include <chrono>
#include <optional>
#include <vector>

namespace corded::server {

inline uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// users.access_level
inline constexpr int kKicked = -2, kBanned = -1, kUser = 0;
// rooms.kind
inline constexpr int kChannel = 0, kDirect = 1, kGroup = 2;

struct DeviceRow {
    Bytes device_id, user_id, dh_key, cert;
    std::string username;
    int access_level = kUser;
};

struct StoredEvent {
    uint64_t seq = 0;
    uint64_t server_ts = 0;
    bool existed = false;
};

class Storage {
public:
    explicit Storage(const std::string& path) : db_(path) {
        db_.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
        db_.exec(R"sql(
CREATE TABLE IF NOT EXISTS users (
    user_id BLOB PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS devices (
    device_id BLOB PRIMARY KEY, user_id BLOB NOT NULL REFERENCES users(user_id),
    dh_key BLOB NOT NULL, cert BLOB NOT NULL, created_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS devices_by_user ON devices(user_id);
CREATE TABLE IF NOT EXISTS signed_prekeys (
    device_id BLOB PRIMARY KEY REFERENCES devices(device_id),
    key_id INTEGER NOT NULL, public_key BLOB NOT NULL, signature BLOB NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS one_time_prekeys (
    device_id BLOB NOT NULL REFERENCES devices(device_id),
    key_id INTEGER NOT NULL, public_key BLOB NOT NULL,
    PRIMARY KEY (device_id, key_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS rooms (
    room_id BLOB PRIMARY KEY, created_at INTEGER NOT NULL, next_seq INTEGER NOT NULL DEFAULT 1
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS memberships (
    room_id BLOB NOT NULL REFERENCES rooms(room_id), user_id BLOB NOT NULL REFERENCES users(user_id),
    PRIMARY KEY (room_id, user_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS memberships_by_user ON memberships(user_id);
CREATE TABLE IF NOT EXISTS room_events (
    room_id BLOB NOT NULL REFERENCES rooms(room_id), seq INTEGER NOT NULL,
    recipient_device BLOB NOT NULL, event_id BLOB NOT NULL,
    sender_user BLOB NOT NULL, sender_device BLOB NOT NULL,
    server_ts INTEGER NOT NULL, ciphertext BLOB NOT NULL,
    PRIMARY KEY (room_id, seq, recipient_device)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS room_events_by_id ON room_events(room_id, event_id);
CREATE TABLE IF NOT EXISTS sections (
    section_id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, position INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS scheduled_events (
    room_id BLOB NOT NULL, event_id BLOB NOT NULL, sender_user BLOB NOT NULL, sender_device BLOB NOT NULL,
    send_at INTEGER NOT NULL, frame BLOB NOT NULL,
    PRIMARY KEY (room_id, event_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS pending_acks (
    device_id BLOB NOT NULL, room_id BLOB NOT NULL, event_id BLOB NOT NULL,
    seq INTEGER NOT NULL, server_ts INTEGER NOT NULL,
    PRIMARY KEY (device_id, room_id, event_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS event_payloads (
    room_id BLOB NOT NULL, seq INTEGER NOT NULL, shared BLOB NOT NULL,
    PRIMARY KEY (room_id, seq)
) WITHOUT ROWID;
)sql");
        try {
            db_.exec("SELECT caps FROM devices LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE devices ADD COLUMN caps INTEGER NOT NULL DEFAULT 0");
        }
        try {
            db_.exec("SELECT access_level FROM users LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE users ADD COLUMN access_level INTEGER NOT NULL DEFAULT 0");
        }
        db_.exec(R"sql(
CREATE TABLE IF NOT EXISTS server_info (key TEXT PRIMARY KEY, value BLOB NOT NULL) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS roles (
    role_id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, position INTEGER NOT NULL,
    permissions INTEGER NOT NULL, is_everyone INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS member_roles (
    user_id BLOB NOT NULL REFERENCES users(user_id), role_id INTEGER NOT NULL REFERENCES roles(role_id),
    PRIMARY KEY (user_id, role_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS invites (
    code_hash BLOB PRIMARY KEY, created_by BLOB NOT NULL, uses_left INTEGER, expires_at INTEGER
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS channel_overrides (
    room_id BLOB NOT NULL REFERENCES rooms(room_id), role_id INTEGER NOT NULL REFERENCES roles(role_id),
    allow INTEGER NOT NULL, deny INTEGER NOT NULL,
    PRIMARY KEY (room_id, role_id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS blobs (
    blob_id BLOB PRIMARY KEY, owner BLOB NOT NULL, total INTEGER NOT NULL, stored INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
) WITHOUT ROWID;
)sql");
        // Added after the first prototype: two-person chats are marked, so a
        // group that shrinks to two people is not mistaken for one.
        try {
            db_.exec("SELECT is_direct FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN is_direct INTEGER NOT NULL DEFAULT 0");
            db_.exec("UPDATE rooms SET is_direct = 1 WHERE "
                     "(SELECT COUNT(*) FROM memberships m WHERE m.room_id = rooms.room_id) = 2");
        }
        try {
            db_.exec("SELECT kind FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN kind INTEGER NOT NULL DEFAULT 2");
            db_.exec("ALTER TABLE rooms ADD COLUMN name TEXT NOT NULL DEFAULT ''");
            db_.exec("UPDATE rooms SET kind = 1 WHERE is_direct = 1");
        }
        try {
            db_.exec("SELECT archived FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN archived INTEGER NOT NULL DEFAULT 0");
        }
        try {
            db_.exec("SELECT section FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN section INTEGER NOT NULL DEFAULT 0");
        }
        try {
            db_.exec("SELECT featured FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN featured INTEGER NOT NULL DEFAULT 0");
        }
        try {
            db_.exec("SELECT channel_type FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN channel_type TEXT NOT NULL DEFAULT ''");
        }
        try {
            db_.exec("SELECT nsfw FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN nsfw INTEGER NOT NULL DEFAULT 0");
        }
        try {
            db_.exec("SELECT expires_at FROM room_events LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE room_events ADD COLUMN expires_at INTEGER");
        }
        db_.exec("CREATE INDEX IF NOT EXISTS room_events_expiry ON room_events(expires_at) "
                 "WHERE expires_at IS NOT NULL");
        try {
            db_.exec("SELECT nickname FROM users LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE users ADD COLUMN nickname TEXT NOT NULL DEFAULT ''");
        }
        // A community always has the @everyone role and at least one channel.
        {
            auto st = db_.prepare("SELECT 1 FROM roles WHERE is_everyone = 1");
            if (!st.step()) {
                auto ins = db_.prepare("INSERT INTO roles (name, position, permissions, is_everyone) "
                                       "VALUES ('@everyone', 0, ?, 1)");
                ins.bind(1, perm::Default).exec();
            }
        }
        if (channels().empty()) create_channel("general");
    }

    std::optional<DeviceRow> find_device(ByteView device_id) {
        auto st = db_.prepare("SELECT d.device_id, d.user_id, d.dh_key, d.cert, u.username, "
                              "u.access_level FROM devices d JOIN users u ON u.user_id = d.user_id "
                              "WHERE d.device_id = ?");
        st.bind(1, device_id);
        if (!st.step()) return std::nullopt;
        return DeviceRow{st.blob(0), st.blob(1), st.blob(2), st.blob(3), st.text(4),
                         static_cast<int>(st.i64(5))};
    }

    std::optional<DeviceRow> device_of_user(ByteView user_id) {
        auto st = db_.prepare("SELECT device_id FROM devices WHERE user_id = ? "
                              "ORDER BY created_at LIMIT 1");
        st.bind(1, user_id);
        if (!st.step()) return std::nullopt;
        return find_device(st.blob(0));
    }

    // Returns 0 on success or an error code.
    uint64_t device_created_at(ByteView device_id) {
        auto st = db_.prepare("SELECT created_at FROM devices WHERE device_id = ?");
        st.bind(1, device_id);
        return st.step() ? st.u64(0) : 0;
    }
    // Forgets one device: its keys and whatever was waiting for it.
    void remove_device(ByteView device_id) {
        db::Transaction tx(db_);
        for (const char* sql : {"DELETE FROM room_events WHERE recipient_device = ?",
                                "DELETE FROM one_time_prekeys WHERE device_id = ?",
                                "DELETE FROM signed_prekeys WHERE device_id = ?",
                                "DELETE FROM devices WHERE device_id = ?"}) {
            auto st = db_.prepare(sql);
            st.bind(1, device_id).exec();
        }
        tx.commit();
    }

    // What a device's client said it understands when it last signed in.
    void set_device_caps(ByteView device_id, uint32_t caps) {
        auto st = db_.prepare("UPDATE devices SET caps = ? WHERE device_id = ?");
        st.bind(1, static_cast<int64_t>(caps)).bind(2, device_id).exec();
    }
    uint32_t device_caps(ByteView device_id) {
        auto st = db_.prepare("SELECT caps FROM devices WHERE device_id = ?");
        st.bind(1, device_id);
        return st.step() ? static_cast<uint32_t>(st.i64(0)) : 0;
    }

    std::vector<DeviceRow> devices_of_user(ByteView user_id) {
        std::vector<Bytes> ids;
        {
            auto st = db_.prepare("SELECT device_id FROM devices WHERE user_id = ? ORDER BY created_at");
            st.bind(1, user_id);
            while (st.step()) ids.push_back(st.blob(0));
        }
        std::vector<DeviceRow> out;
        for (const auto& id : ids)
            if (auto d = find_device(id)) out.push_back(std::move(*d));
        return out;
    }

    uint16_t register_user(const wire::RegisterT& r) {
        db::Transaction tx(db_);
        {
            auto st = db_.prepare("SELECT user_id FROM users WHERE username = ?");
            st.bind(1, r.username);
            if (st.step() && st.blob(0) != r.user_id) return err::NameTaken;
        }
        {
            auto st = db_.prepare("INSERT OR IGNORE INTO users (user_id, username, created_at) "
                                  "VALUES (?, ?, ?)");
            st.bind(1, r.user_id).bind(2, r.username).bind(3, now_ms()).exec();
        }
        {
            auto st = db_.prepare("INSERT OR IGNORE INTO devices "
                                  "(device_id, user_id, dh_key, cert, created_at) VALUES (?,?,?,?,?)");
            st.bind(1, r.device_id).bind(2, r.user_id).bind(3, r.dh_key).bind(4, r.cert)
                .bind(5, now_ms()).exec();
        }
        tx.commit();
        return 0;
    }

    void put_prekeys(ByteView device_id, const wire::PublishPrekeysT& p) {
        db::Transaction tx(db_);
        auto st = db_.prepare("INSERT OR REPLACE INTO signed_prekeys "
                              "(device_id, key_id, public_key, signature) VALUES (?,?,?,?)");
        st.bind(1, device_id).bind(2, p.spk_id).bind(3, p.spk).bind(4, p.spk_sig).exec();
        for (const auto& k : p.otks) {
            if (!k || k->key.size() != 32) continue;
            auto ins = db_.prepare("INSERT OR IGNORE INTO one_time_prekeys "
                                   "(device_id, key_id, public_key) VALUES (?,?,?)");
            ins.bind(1, device_id).bind(2, k->id).bind(3, k->key).exec();
        }
        tx.commit();
    }

    // Hands out (and deletes) one one-time prekey if any remain.
    std::optional<wire::BundleT> take_bundle(ByteView user_id, ByteView device_id = {}) {
        auto dev = device_id.empty() ? device_of_user(user_id) : find_device(device_id);
        if (!dev || dev->user_id.size() != user_id.size() ||
            !std::equal(dev->user_id.begin(), dev->user_id.end(), user_id.begin()))
            return std::nullopt;
        db::Transaction tx(db_);
        wire::BundleT b;
        b.user_id = dev->user_id;
        b.device_id = dev->device_id;
        b.dh_key = dev->dh_key;
        b.cert = dev->cert;
        {
            auto st = db_.prepare("SELECT key_id, public_key, signature FROM signed_prekeys "
                                  "WHERE device_id = ?");
            st.bind(1, dev->device_id);
            if (!st.step()) return std::nullopt;
            b.spk_id = static_cast<uint32_t>(st.i64(0));
            b.spk = st.blob(1);
            b.spk_sig = st.blob(2);
        }
        {
            auto st = db_.prepare("SELECT key_id, public_key FROM one_time_prekeys "
                                  "WHERE device_id = ? ORDER BY key_id LIMIT 1");
            st.bind(1, dev->device_id);
            if (st.step()) {
                b.has_otk = true;
                b.otk_id = static_cast<uint32_t>(st.i64(0));
                b.otk = st.blob(1);
            }
        }
        if (b.has_otk) {
            auto del = db_.prepare("DELETE FROM one_time_prekeys WHERE device_id = ? AND key_id = ?");
            del.bind(1, dev->device_id).bind(2, b.otk_id).exec();
        }
        tx.commit();
        return b;
    }

    std::optional<wire::UserInfoT> lookup_user(std::string_view username) {
        auto st = db_.prepare("SELECT user_id, username FROM users WHERE username = ?");
        st.bind(1, username);
        if (!st.step()) return std::nullopt;
        wire::UserInfoT u;
        u.user_id = st.blob(0);
        u.username = st.text(1);
        return u;
    }

    int access_level(ByteView user_id) {
        auto st = db_.prepare("SELECT access_level FROM users WHERE user_id = ?");
        st.bind(1, user_id);
        return st.step() ? static_cast<int>(st.i64(0)) : kUser;
    }

    void set_access_level(ByteView user_id, int level) {
        auto st = db_.prepare("UPDATE users SET access_level = ? WHERE user_id = ?");
        st.bind(1, level).bind(2, user_id).exec();
    }

    void set_nickname(ByteView user_id, const std::string& nickname) {
        auto st = db_.prepare("UPDATE users SET nickname = ? WHERE user_id = ?");
        st.bind(1, nickname).bind(2, user_id).exec();
    }

    // Deletes an account outright: its devices, keys, memberships, roles and
    // the messages waiting for it. The username becomes free again.
    void remove_account(ByteView user_id) {
        db::Transaction tx(db_);
        for (const char* sql : {
                 "DELETE FROM room_events WHERE recipient_device IN (SELECT device_id FROM devices WHERE user_id = ?)",
                 "DELETE FROM one_time_prekeys WHERE device_id IN (SELECT device_id FROM devices WHERE user_id = ?)",
                 "DELETE FROM signed_prekeys WHERE device_id IN (SELECT device_id FROM devices WHERE user_id = ?)",
                 "DELETE FROM devices WHERE user_id = ?",
                 "DELETE FROM member_roles WHERE user_id = ?",
                 "DELETE FROM memberships WHERE user_id = ?",
                 "DELETE FROM users WHERE user_id = ?"}) {
            auto st = db_.prepare(sql);
            st.bind(1, user_id).exec();
        }
        tx.commit();
    }

    // Removes someone from the community. They may join again, subject to the
    // registration policy, and start with no roles.
    void kick(ByteView user_id) {
        db::Transaction tx(db_);
        set_access_level(user_id, kKicked);
        auto roles_del = db_.prepare("DELETE FROM member_roles WHERE user_id = ?");
        roles_del.bind(1, user_id).exec();
        auto groups = db_.prepare("DELETE FROM memberships WHERE user_id = ? AND room_id IN "
                                  "(SELECT room_id FROM rooms WHERE kind = 2)");
        groups.bind(1, user_id).exec();
        tx.commit();
    }

    bool user_exists(ByteView user_id) {
        auto st = db_.prepare("SELECT 1 FROM users WHERE user_id = ?");
        st.bind(1, user_id);
        return st.step();
    }

    // ------------------------------------------------------------ community

    std::optional<Bytes> info(const std::string& key) {
        auto st = db_.prepare("SELECT value FROM server_info WHERE key = ?");
        st.bind(1, key);
        if (!st.step()) return std::nullopt;
        return st.blob(0);
    }
    void set_info(const std::string& key, ByteView value) {
        auto st = db_.prepare("INSERT OR REPLACE INTO server_info (key, value) VALUES (?, ?)");
        st.bind(1, key).bind(2, value).exec();
    }
    Bytes owner() { return info("owner").value_or(Bytes{}); }
    bool is_owner(ByteView user_id) {
        Bytes o = owner();
        return !o.empty() && o.size() == user_id.size() && std::equal(o.begin(), o.end(), user_id.begin());
    }

    std::vector<wire::RoleT> roles() {
        std::vector<wire::RoleT> out;
        auto st = db_.prepare("SELECT role_id, name, position, permissions, is_everyone FROM roles "
                              "ORDER BY position DESC, role_id");
        while (st.step()) {
            wire::RoleT r;
            r.role_id = static_cast<uint32_t>(st.i64(0));
            r.name = st.text(1);
            r.position = static_cast<int32_t>(st.i64(2));
            r.permissions = st.u64(3);
            r.is_everyone = st.i64(4) != 0;
            out.push_back(std::move(r));
        }
        return out;
    }
    std::optional<wire::RoleT> role(uint32_t role_id) {
        for (auto& r : roles())
            if (r.role_id == role_id) return r;
        return std::nullopt;
    }
    uint32_t create_role(const std::string& name, uint64_t permissions) {
        auto st = db_.prepare("INSERT INTO roles (name, position, permissions) VALUES "
                              "(?, (SELECT COALESCE(MAX(position), 0) + 1 FROM roles), ?)");
        st.bind(1, name).bind(2, permissions).exec();
        return static_cast<uint32_t>(db_.last_insert_rowid());
    }
    void update_role(uint32_t role_id, const std::string& name, uint64_t permissions) {
        auto st = db_.prepare("UPDATE roles SET name = ?, permissions = ? WHERE role_id = ?");
        st.bind(1, name).bind(2, permissions).bind(3, role_id).exec();
    }
    void delete_role(uint32_t role_id) {
        db::Transaction tx(db_);
        for (const char* sql : {"DELETE FROM member_roles WHERE role_id = ?",
                                "DELETE FROM channel_overrides WHERE role_id = ?",
                                "DELETE FROM roles WHERE role_id = ? AND is_everyone = 0"}) {
            auto st = db_.prepare(sql);
            st.bind(1, role_id).exec();
        }
        tx.commit();
    }
    void assign_role(ByteView user_id, uint32_t role_id, bool assign) {
        auto st = db_.prepare(assign ? "INSERT OR IGNORE INTO member_roles (user_id, role_id) VALUES (?, ?)"
                                     : "DELETE FROM member_roles WHERE user_id = ? AND role_id = ?");
        st.bind(1, user_id).bind(2, role_id).exec();
    }
    // The roles a member holds, always including @everyone.
    std::vector<wire::RoleT> roles_of(ByteView user_id) {
        std::vector<wire::RoleT> out;
        auto st = db_.prepare("SELECT role_id, name, position, permissions, is_everyone FROM roles "
                              "WHERE is_everyone = 1 OR role_id IN "
                              "(SELECT role_id FROM member_roles WHERE user_id = ?)");
        st.bind(1, user_id);
        while (st.step()) {
            wire::RoleT r;
            r.role_id = static_cast<uint32_t>(st.i64(0));
            r.name = st.text(1);
            r.position = static_cast<int32_t>(st.i64(2));
            r.permissions = st.u64(3);
            r.is_everyone = st.i64(4) != 0;
            out.push_back(std::move(r));
        }
        return out;
    }
    // Rank used for "can only act on people below you". The owner outranks all.
    int64_t rank(ByteView user_id) {
        if (is_owner(user_id)) return INT64_MAX;
        int64_t top = 0;
        for (const auto& r : roles_of(user_id)) top = std::max<int64_t>(top, r.position);
        return top;
    }
    // Server-wide permissions.
    uint64_t permissions(ByteView user_id) {
        if (is_owner(user_id)) return perm::All;
        uint64_t bits = 0;
        for (const auto& r : roles_of(user_id)) bits |= r.permissions;
        return (bits & perm::Administrator) ? perm::All : bits;
    }
    // Permissions inside one channel, after that channel's exceptions.
    uint64_t permissions(ByteView user_id, ByteView room_id) {
        uint64_t bits = permissions(user_id);
        if (bits & perm::Administrator) return perm::All;
        uint64_t allow = 0, deny = 0;
        for (const auto& r : roles_of(user_id)) {
            auto st = db_.prepare("SELECT allow, deny FROM channel_overrides WHERE room_id = ? AND role_id = ?");
            st.bind(1, room_id).bind(2, r.role_id);
            if (!st.step()) continue;
            if (r.is_everyone) {
                bits = (bits & ~st.u64(1)) | st.u64(0);
            } else {
                allow |= st.u64(0);
                deny |= st.u64(1);
            }
        }
        return (bits & ~deny) | allow;
    }
    struct OverrideRow {
        uint32_t role_id = 0;
        uint64_t allow = 0, deny = 0;
    };
    std::vector<OverrideRow> overrides(ByteView room_id) {
        std::vector<OverrideRow> out;
        auto st = db_.prepare("SELECT role_id, allow, deny FROM channel_overrides WHERE room_id = ?");
        st.bind(1, room_id);
        while (st.step()) out.push_back({static_cast<uint32_t>(st.u64(0)), st.u64(1), st.u64(2)});
        return out;
    }

    void set_override(ByteView room_id, uint32_t role_id, uint64_t allow, uint64_t deny) {
        if (allow == 0 && deny == 0) {
            auto st = db_.prepare("DELETE FROM channel_overrides WHERE room_id = ? AND role_id = ?");
            st.bind(1, room_id).bind(2, role_id).exec();
            return;
        }
        auto st = db_.prepare("INSERT OR REPLACE INTO channel_overrides (room_id, role_id, allow, deny) "
                              "VALUES (?,?,?,?)");
        st.bind(1, room_id).bind(2, role_id).bind(3, allow).bind(4, deny).exec();
    }

    // Invite codes are stored only as hashes.
    static Bytes hash_code(std::string_view code) {
        Bytes h(crypto_hash_sha256_BYTES);
        crypto_hash_sha256(h.data(), reinterpret_cast<const uint8_t*>(code.data()), code.size());
        return h;
    }
    void add_invite(std::string_view code, ByteView created_by, uint32_t max_uses, uint64_t expires_at) {
        auto st = db_.prepare("INSERT INTO invites (code_hash, created_by, uses_left, expires_at) "
                              "VALUES (?,?,?,?)");
        st.bind(1, hash_code(code)).bind(2, created_by);
        if (max_uses) st.bind(3, max_uses);
        else st.bind_null(3);
        if (expires_at) st.bind(4, expires_at);
        else st.bind_null(4);
        st.exec();
    }
    bool invite_valid(std::string_view code) {
        auto st = db_.prepare("SELECT 1 FROM invites WHERE code_hash = ? AND (uses_left IS NULL OR "
                              "uses_left > 0) AND (expires_at IS NULL OR expires_at > ?)");
        st.bind(1, hash_code(code)).bind(2, now_ms());
        return st.step();
    }
    void use_invite(std::string_view code) {
        auto st = db_.prepare("UPDATE invites SET uses_left = uses_left - 1 WHERE code_hash = ? "
                              "AND uses_left IS NOT NULL");
        st.bind(1, hash_code(code)).exec();
    }
    bool revoke_invite(std::string_view code) {
        auto st = db_.prepare("DELETE FROM invites WHERE code_hash = ?");
        st.bind(1, hash_code(code)).exec();
        return db_.changes() > 0;
    }

    // Everyone who belongs to the community (not banned, not kicked).
    std::vector<Bytes> member_ids() {
        std::vector<Bytes> out;
        auto st = db_.prepare("SELECT user_id FROM users WHERE access_level >= 0 ORDER BY username");
        while (st.step()) out.push_back(st.blob(0));
        return out;
    }
    std::unique_ptr<wire::MemberT> member(ByteView user_id) {
        auto st = db_.prepare("SELECT username, nickname FROM users WHERE user_id = ?");
        st.bind(1, user_id);
        if (!st.step()) return nullptr;
        auto m = std::make_unique<wire::MemberT>();
        m->user_id = to_bytes(user_id);
        m->username = st.text(0);
        m->nickname = st.text(1);
        m->is_owner = is_owner(user_id);
        m->is_admin = (permissions(user_id) & perm::Administrator) != 0;
        for (const auto& r : roles_of(user_id))
            if (!r.is_everyone) m->roles.push_back(r.role_id);
        return m;
    }

    int kind(ByteView room_id) {
        auto st = db_.prepare("SELECT kind FROM rooms WHERE room_id = ?");
        st.bind(1, room_id);
        return st.step() ? static_cast<int>(st.i64(0)) : -1;
    }
    std::vector<Bytes> channels() {
        std::vector<Bytes> out;
        auto st = db_.prepare("SELECT room_id FROM rooms WHERE kind = 0 ORDER BY created_at, name");
        while (st.step()) out.push_back(st.blob(0));
        return out;
    }
    // The type says how clients lay the channel out: messages, or a task list.
    Bytes create_channel(const std::string& name, const std::string& type = "") {
        Bytes room_id = random_bytes(16);
        auto st = db_.prepare("INSERT INTO rooms (room_id, created_at, is_direct, kind, name, channel_type) "
                              "VALUES (?, ?, 0, 0, ?, ?)");
        st.bind(1, room_id).bind(2, now_ms()).bind(3, name).bind(4, type).exec();
        return room_id;
    }
    void set_channel_archived(ByteView room_id, bool archived) {
        auto st = db_.prepare("UPDATE rooms SET archived = ? WHERE room_id = ? AND kind = 0");
        st.bind(1, static_cast<int64_t>(archived ? 1 : 0)).bind(2, room_id).exec();
    }
    bool is_archived(ByteView room_id) {
        auto st = db_.prepare("SELECT archived FROM rooms WHERE room_id = ?");
        st.bind(1, room_id);
        return st.step() && st.i64(0) != 0;
    }
    // ---- sections: named groups of channels ----
    std::vector<wire::SectionT> sections() {
        std::vector<wire::SectionT> out;
        auto st = db_.prepare("SELECT section_id, name, position FROM sections ORDER BY position, section_id");
        while (st.step()) {
            wire::SectionT s;
            s.section_id = static_cast<uint32_t>(st.i64(0));
            s.name = st.text(1);
            s.position = static_cast<int32_t>(st.i64(2));
            out.push_back(std::move(s));
        }
        return out;
    }
    bool section_exists(uint32_t id) {
        auto st = db_.prepare("SELECT 1 FROM sections WHERE section_id = ?");
        st.bind(1, static_cast<int64_t>(id));
        return st.step();
    }
    uint32_t section_count() {
        auto st = db_.prepare("SELECT COUNT(*) FROM sections");
        return st.step() ? static_cast<uint32_t>(st.i64(0)) : 0;
    }
    // A new section goes after the ones there are.
    void create_section(const std::string& name) {
        auto st = db_.prepare("INSERT INTO sections (name, position) "
                              "VALUES (?, COALESCE((SELECT MAX(position) FROM sections), 0) + 1)");
        st.bind(1, name).exec();
    }
    void update_section(uint32_t id, const std::string& name, int32_t position) {
        auto st = db_.prepare("UPDATE sections SET name = ?, position = ? WHERE section_id = ?");
        st.bind(1, name).bind(2, static_cast<int64_t>(position)).bind(3, static_cast<int64_t>(id)).exec();
    }
    void remove_section(uint32_t id) {
        db::Transaction tx(db_);
        auto clear = db_.prepare("UPDATE rooms SET section = 0 WHERE section = ?");
        clear.bind(1, static_cast<int64_t>(id)).exec();
        auto del = db_.prepare("DELETE FROM sections WHERE section_id = ?");
        del.bind(1, static_cast<int64_t>(id)).exec();
        tx.commit();
    }
    void set_channel_section(ByteView room_id, uint32_t section) {
        auto st = db_.prepare("UPDATE rooms SET section = ? WHERE room_id = ? AND kind = 0");
        st.bind(1, static_cast<int64_t>(section)).bind(2, room_id).exec();
    }

    void set_channel_featured(ByteView room_id, bool featured) {
        auto st = db_.prepare("UPDATE rooms SET featured = ? WHERE room_id = ? AND kind = 0");
        st.bind(1, static_cast<int64_t>(featured ? 1 : 0)).bind(2, room_id).exec();
    }
    void set_channel_nsfw(ByteView room_id, bool nsfw) {
        auto st = db_.prepare("UPDATE rooms SET nsfw = ? WHERE room_id = ? AND kind = 0");
        st.bind(1, static_cast<int64_t>(nsfw ? 1 : 0)).bind(2, room_id).exec();
    }
    void rename_channel(ByteView room_id, const std::string& name) {
        auto st = db_.prepare("UPDATE rooms SET name = ? WHERE room_id = ? AND kind = 0");
        st.bind(1, name).bind(2, room_id).exec();
    }
    // Removes direct chats and groups that have nobody left in them. A chat
    // with one person left stays: their client would erase its copy of the
    // conversation if the room vanished, and that is theirs to decide.
    int prune_empty_rooms() {
        std::vector<Bytes> gone;
        {
            auto st = db_.prepare(
                "SELECT r.room_id FROM rooms r WHERE r.kind != 0 AND "
                "(SELECT COUNT(*) FROM memberships m WHERE m.room_id = r.room_id) = 0");
            while (st.step()) gone.push_back(st.blob(0));
        }
        db::Transaction tx(db_);
        for (const auto& room_id : gone)
            for (const char* sql : {"DELETE FROM room_events WHERE room_id = ?",
                                    "DELETE FROM memberships WHERE room_id = ?",
                                    "DELETE FROM rooms WHERE room_id = ? AND kind != 0"}) {
                auto st = db_.prepare(sql);
                st.bind(1, room_id).exec();
            }
        tx.commit();
        return static_cast<int>(gone.size());
    }

    void delete_channel(ByteView room_id) {
        db::Transaction tx(db_);
        for (const char* sql : {"DELETE FROM room_events WHERE room_id = ?",
                                "DELETE FROM channel_overrides WHERE room_id = ?",
                                "DELETE FROM rooms WHERE room_id = ? AND kind = 0"}) {
            auto st = db_.prepare(sql);
            st.bind(1, room_id).exec();
        }
        tx.commit();
    }
    bool channel_name_taken(const std::string& name) {
        auto st = db_.prepare("SELECT 1 FROM rooms WHERE kind = 0 AND name = ?");
        st.bind(1, name);
        return st.step();
    }

    // A channel's members are whoever can view it; other rooms list theirs.
    bool is_member(ByteView room_id, ByteView user_id) {
        if (kind(room_id) == kChannel)
            return access_level(user_id) >= 0 && (permissions(user_id, room_id) & perm::ViewChannel);
        auto st = db_.prepare("SELECT 1 FROM memberships WHERE room_id = ? AND user_id = ?");
        st.bind(1, room_id).bind(2, user_id);
        return st.step();
    }

    wire::RoomInfoT room_info(ByteView room_id) {
        wire::RoomInfoT info;
        info.room_id = to_bytes(room_id);
        {
            auto st = db_.prepare(
                "SELECT created_at, kind, name, nsfw, archived, channel_type, featured, section FROM rooms "
                "WHERE room_id = ?");
            st.bind(1, room_id);
            if (st.step()) {
                info.created_at = st.u64(0);
                info.kind = static_cast<uint8_t>(st.i64(1));
                info.is_direct = info.kind == kDirect;
                info.name = st.text(2);
                info.nsfw = st.i64(3) != 0;
                info.archived = st.i64(4) != 0;
                info.channel_type = st.text(5);
                info.featured = st.i64(6) != 0;
                info.section = static_cast<uint32_t>(st.i64(7));
            }
        }
        if (info.kind == kChannel) {
            for (const auto& user : member_ids())
                if (permissions(user, room_id) & perm::ViewChannel)
                    if (auto m = member(user)) info.members.push_back(std::move(m));
            return info;
        }
        std::vector<Bytes> ids;
        {
            auto st = db_.prepare("SELECT u.user_id FROM memberships m JOIN users u ON u.user_id = m.user_id "
                                  "WHERE m.room_id = ? ORDER BY u.username");
            st.bind(1, room_id);
            while (st.step()) ids.push_back(st.blob(0));
        }
        for (const auto& id : ids)
            if (auto m = member(id)) info.members.push_back(std::move(m));
        return info;
    }

    // Channels the user can view, then their direct messages and groups.
    std::vector<Bytes> rooms_of_user(ByteView user_id) {
        std::vector<Bytes> out;
        for (const auto& ch : channels())
            if (permissions(user_id, ch) & perm::ViewChannel) out.push_back(ch);
        auto st = db_.prepare("SELECT room_id FROM memberships WHERE user_id = ?");
        st.bind(1, user_id);
        while (st.step()) out.push_back(st.blob(0));
        return out;
    }

    // Two-person rooms are unique per pair; asking again returns the same room.
    std::optional<Bytes> find_direct_room(ByteView a, ByteView b) {
        auto st = db_.prepare(
            "SELECT m1.room_id FROM memberships m1 JOIN memberships m2 ON m1.room_id = m2.room_id "
            "JOIN rooms r ON r.room_id = m1.room_id "
            "WHERE m1.user_id = ? AND m2.user_id = ? AND r.is_direct = 1 LIMIT 1");
        st.bind(1, a).bind(2, b);
        if (!st.step()) return std::nullopt;
        return st.blob(0);
    }

    Bytes create_room(const std::vector<Bytes>& members) {
        Bytes room_id = random_bytes(16);
        db::Transaction tx(db_);
        auto st = db_.prepare("INSERT INTO rooms (room_id, created_at, is_direct, kind) VALUES (?, ?, ?, ?)");
        st.bind(1, room_id).bind(2, now_ms()).bind(3, members.size() == 2 ? 1 : 0)
            .bind(4, members.size() == 2 ? kDirect : kGroup).exec();
        for (const auto& m : members) {
            auto ins = db_.prepare("INSERT OR IGNORE INTO memberships (room_id, user_id) VALUES (?,?)");
            ins.bind(1, room_id).bind(2, m).exec();
        }
        tx.commit();
        return room_id;
    }

    bool is_direct(ByteView room_id) {
        auto st = db_.prepare("SELECT is_direct FROM rooms WHERE room_id = ?");
        st.bind(1, room_id);
        return st.step() && st.i64(0) != 0;
    }

    void add_member(ByteView room_id, ByteView user_id) {
        auto st = db_.prepare("INSERT OR IGNORE INTO memberships (room_id, user_id) VALUES (?, ?)");
        st.bind(1, room_id).bind(2, user_id).exec();
    }

    void remove_member(ByteView room_id, ByteView user_id) {
        auto st = db_.prepare("DELETE FROM memberships WHERE room_id = ? AND user_id = ?");
        st.bind(1, room_id).bind(2, user_id).exec();
    }

    // Assigns the next sequence number and stores one row per recipient device.
    // A retry of an event id that is already stored returns the original result.
    StoredEvent store_event(const wire::SendRoomEventT& ev, ByteView sender_user,
                            ByteView sender_device) {
        db::Transaction tx(db_);
        {
            auto st = db_.prepare("SELECT seq, server_ts FROM room_events "
                                  "WHERE room_id = ? AND event_id = ? LIMIT 1");
            st.bind(1, ev.room_id).bind(2, ev.event_id);
            if (st.step()) return StoredEvent{st.u64(0), st.u64(1), true};
        }
        StoredEvent out;
        out.server_ts = now_ms();
        {
            auto st = db_.prepare("UPDATE rooms SET next_seq = next_seq + 1 WHERE room_id = ? "
                                  "RETURNING next_seq - 1");
            st.bind(1, ev.room_id);
            if (!st.step()) throw db::Error("room vanished");
            out.seq = st.u64(0);
            st.exec();
        }
        for (const auto& r : ev.recipients) {
            if (!r) continue;
            auto ins = db_.prepare("INSERT INTO room_events (room_id, seq, recipient_device, "
                                   "event_id, sender_user, sender_device, server_ts, ciphertext, "
                                   "expires_at) VALUES (?,?,?,?,?,?,?,?,?)");
            ins.bind(1, ev.room_id).bind(2, out.seq).bind(3, r->device_id).bind(4, ev.event_id)
                .bind(5, sender_user).bind(6, sender_device).bind(7, out.server_ts)
                .bind(8, r->ciphertext);
            if (ev.expires_at) ins.bind(9, ev.expires_at);
            else ins.bind_null(9);
            ins.exec();
        }
        // The part everyone shares is kept once, not once per device.
        if (!ev.shared.empty() && !ev.recipients.empty()) {
            auto ins = db_.prepare("INSERT INTO event_payloads (room_id, seq, shared) VALUES (?,?,?)");
            ins.bind(1, ev.room_id).bind(2, out.seq).bind(3, ev.shared).exec();
        }
        tx.commit();
        return out;
    }

    // ---- scheduled messages: encrypted events kept aside until their time ----
    struct ScheduledRow {
        Bytes room_id, event_id, sender_user, sender_device, frame;
        uint64_t send_at = 0;
    };
    // False if an event with that id is already waiting.
    bool schedule_event(ByteView room_id, ByteView event_id, ByteView sender_user, ByteView sender_device,
                        uint64_t send_at, ByteView frame) {
        auto st = db_.prepare("INSERT OR IGNORE INTO scheduled_events (room_id, event_id, sender_user, "
                              "sender_device, send_at, frame) VALUES (?,?,?,?,?,?)");
        st.bind(1, room_id).bind(2, event_id).bind(3, sender_user).bind(4, sender_device).bind(5, send_at)
            .bind(6, frame).exec();
        return db_.changes() > 0;
    }
    uint32_t scheduled_count(ByteView sender_user) {
        auto st = db_.prepare("SELECT COUNT(*) FROM scheduled_events WHERE sender_user = ?");
        st.bind(1, sender_user);
        return st.step() ? static_cast<uint32_t>(st.i64(0)) : 0;
    }
    std::vector<ScheduledRow> scheduled_due(uint64_t now) {
        std::vector<ScheduledRow> out;
        auto st = db_.prepare("SELECT room_id, event_id, sender_user, sender_device, frame, send_at "
                              "FROM scheduled_events WHERE send_at <= ? ORDER BY send_at LIMIT 100");
        st.bind(1, now);
        while (st.step())
            out.push_back(ScheduledRow{st.blob(0), st.blob(1), st.blob(2), st.blob(3), st.blob(4), st.u64(5)});
        return out;
    }
    // True if it was waiting and the asker is who scheduled it.
    bool cancel_scheduled(ByteView room_id, ByteView event_id, ByteView sender_user) {
        auto st = db_.prepare("DELETE FROM scheduled_events WHERE room_id = ? AND event_id = ? AND sender_user = ?");
        st.bind(1, room_id).bind(2, event_id).bind(3, sender_user).exec();
        return db_.changes() > 0;
    }
    void forget_scheduled(ByteView room_id, ByteView event_id) {
        auto st = db_.prepare("DELETE FROM scheduled_events WHERE room_id = ? AND event_id = ?");
        st.bind(1, room_id).bind(2, event_id).exec();
    }
    // The sender was not connected when its scheduled event went out: it is
    // told where the event landed the next time it signs in.
    void add_pending_ack(ByteView device_id, ByteView room_id, ByteView event_id, uint64_t seq, uint64_t server_ts) {
        auto st = db_.prepare("INSERT OR REPLACE INTO pending_acks (device_id, room_id, event_id, seq, server_ts) "
                              "VALUES (?,?,?,?,?)");
        st.bind(1, device_id).bind(2, room_id).bind(3, event_id).bind(4, seq).bind(5, server_ts).exec();
    }
    std::vector<wire::SendOkT> take_pending_acks(ByteView device_id) {
        std::vector<wire::SendOkT> out;
        {
            auto st = db_.prepare("SELECT room_id, event_id, seq, server_ts FROM pending_acks WHERE device_id = ?");
            st.bind(1, device_id);
            while (st.step()) {
                wire::SendOkT ok;
                ok.room_id = st.blob(0);
                ok.event_id = st.blob(1);
                ok.seq = st.u64(2);
                ok.server_ts = st.u64(3);
                out.push_back(std::move(ok));
            }
        }
        auto del = db_.prepare("DELETE FROM pending_acks WHERE device_id = ?");
        del.bind(1, device_id).exec();
        return out;
    }

    // ---- files: the bytes live on disk, one file each; this is the ledger ----
    struct BlobRow {
        Bytes owner;
        uint64_t total = 0, stored = 0, created_at = 0;
        bool complete() const { return stored == total; }
    };
    std::optional<BlobRow> blob(ByteView id) {
        auto st = db_.prepare("SELECT owner, total, stored, created_at FROM blobs WHERE blob_id = ?");
        st.bind(1, id);
        if (!st.step()) return std::nullopt;
        return BlobRow{st.blob(0), static_cast<uint64_t>(st.i64(1)), static_cast<uint64_t>(st.i64(2)),
                       static_cast<uint64_t>(st.i64(3))};
    }
    void blob_begin(ByteView id, ByteView owner, uint64_t total) {
        auto st = db_.prepare("INSERT INTO blobs (blob_id, owner, total, stored, created_at) VALUES (?, ?, ?, 0, ?)");
        st.bind(1, id).bind(2, owner).bind(3, static_cast<int64_t>(total)).bind(4, now_ms()).exec();
    }
    void blob_stored(ByteView id, uint64_t stored) {
        auto st = db_.prepare("UPDATE blobs SET stored = ? WHERE blob_id = ?");
        st.bind(1, static_cast<int64_t>(stored)).bind(2, id).exec();
    }
    void blob_forget(ByteView id) {
        auto st = db_.prepare("DELETE FROM blobs WHERE blob_id = ?");
        st.bind(1, id).exec();
    }
    std::vector<Bytes> banned_ids() {
        std::vector<Bytes> out;
        auto st = db_.prepare("SELECT user_id FROM users WHERE access_level = ? ORDER BY username");
        st.bind(1, static_cast<int64_t>(kBanned));
        while (st.step()) out.push_back(st.blob(0));
        return out;
    }
    uint32_t blob_count() {
        auto st = db_.prepare("SELECT COUNT(*) FROM blobs");
        return st.step() ? static_cast<uint32_t>(st.i64(0)) : 0;
    }
    uint32_t scheduled_total() {
        auto st = db_.prepare("SELECT COUNT(*) FROM scheduled_events");
        return st.step() ? static_cast<uint32_t>(st.i64(0)) : 0;
    }
    // Files older than so many days, with their sizes; 0 days means every file.
    std::vector<std::pair<Bytes, uint64_t>> blobs_older_than(uint32_t days) {
        std::vector<std::pair<Bytes, uint64_t>> out;
        auto st = db_.prepare("SELECT blob_id, total FROM blobs WHERE created_at <= ?");
        st.bind(1, now_ms() - uint64_t{days} * 24 * 3600 * 1000);
        while (st.step()) out.emplace_back(st.blob(0), st.u64(1));
        return out;
    }
    // Space promised to files, finished or not.
    uint64_t blob_bytes() {
        auto st = db_.prepare("SELECT COALESCE(SUM(total), 0) FROM blobs");
        st.step();
        return static_cast<uint64_t>(st.i64(0));
    }
    // Files to drop: uploads abandoned for a day, and, when messages are only
    // kept for a while, files older than that.
    std::vector<Bytes> blobs_to_drop(int retention_days) {
        std::vector<Bytes> out;
        auto st = db_.prepare("SELECT blob_id FROM blobs WHERE (stored < total AND created_at < ?) "
                              "OR (? > 0 AND created_at < ?)");
        st.bind(1, now_ms() - uint64_t{24} * 3600 * 1000)
            .bind(2, static_cast<int64_t>(retention_days))
            .bind(3, now_ms() - static_cast<uint64_t>(retention_days > 0 ? retention_days : 0) * 24 * 3600 * 1000);
        while (st.step()) out.push_back(st.blob(0));
        return out;
    }

    // Regular tidying: spent and expired invites, messages past the retention
    // window, then let SQLite compact its log.
    void housekeeping(int retention_days) {
        {
            auto st = db_.prepare("DELETE FROM invites WHERE (uses_left IS NOT NULL AND uses_left <= 0) "
                                  "OR (expires_at IS NOT NULL AND expires_at <= ?)");
            st.bind(1, now_ms()).exec();
        }
        if (retention_days > 0) {
            auto st = db_.prepare("DELETE FROM room_events WHERE server_ts < ?");
            st.bind(1, now_ms() - static_cast<uint64_t>(retention_days) * 24 * 3600 * 1000).exec();
        }
        sweep_expired();
        drop_unused_payloads();
        db_.exec("PRAGMA wal_checkpoint(TRUNCATE); PRAGMA optimize;");
        set_info("last_housekeeping", to_bytes(std::to_string(now_ms())));
    }
    uint32_t member_count() {
        auto st = db_.prepare("SELECT COUNT(*) FROM users WHERE access_level >= 0");
        st.step();
        return static_cast<uint32_t>(st.i64(0));
    }

    // Drops stored copies of disappearing messages whose time has come.
    // A shared part is kept for as long as some device still has its copy to collect.
    void drop_unused_payloads() {
        db_.exec("DELETE FROM event_payloads WHERE NOT EXISTS (SELECT 1 FROM room_events e "
                 "WHERE e.room_id = event_payloads.room_id AND e.seq = event_payloads.seq)");
    }

    int sweep_expired() {
        auto st = db_.prepare("DELETE FROM room_events WHERE expires_at IS NOT NULL AND expires_at <= ?");
        st.bind(1, now_ms()).exec();
        int gone = db_.changes();
        if (gone > 0) drop_unused_payloads();
        return gone;
    }

    std::vector<wire::RoomEventT> events_after(ByteView room_id, ByteView device_id, uint64_t seq) {
        std::vector<wire::RoomEventT> out;
        auto st = db_.prepare(
            "SELECT e.seq, e.event_id, e.sender_user, e.sender_device, e.server_ts, e.ciphertext, p.shared "
            "FROM room_events e LEFT JOIN event_payloads p ON p.room_id = e.room_id AND p.seq = e.seq "
            "WHERE e.room_id = ? AND e.recipient_device = ? AND e.seq > ? ORDER BY e.seq");
        st.bind(1, room_id).bind(2, device_id).bind(3, seq);
        while (st.step()) {
            wire::RoomEventT e;
            e.room_id = to_bytes(room_id);
            e.seq = st.u64(0);
            e.event_id = st.blob(1);
            e.sender_user = st.blob(2);
            e.sender_device = st.blob(3);
            e.server_ts = st.u64(4);
            e.ciphertext = st.blob(5);
            e.shared = st.blob(6);  // empty when the event was sent whole to each device
            out.push_back(std::move(e));
        }
        return out;
    }

private:
    db::Database db_;
};

}  // namespace corded::server

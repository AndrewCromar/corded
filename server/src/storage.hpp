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
)sql");
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
CREATE TABLE IF NOT EXISTS channel_overrides (
    room_id BLOB NOT NULL REFERENCES rooms(room_id), role_id INTEGER NOT NULL REFERENCES roles(role_id),
    allow INTEGER NOT NULL, deny INTEGER NOT NULL,
    PRIMARY KEY (room_id, role_id)
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
    std::optional<wire::BundleT> take_bundle(ByteView user_id) {
        auto dev = device_of_user(user_id);
        if (!dev) return std::nullopt;
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

    // Everyone who belongs to the community (not banned, not kicked).
    std::vector<Bytes> member_ids() {
        std::vector<Bytes> out;
        auto st = db_.prepare("SELECT user_id FROM users WHERE access_level >= 0 ORDER BY username");
        while (st.step()) out.push_back(st.blob(0));
        return out;
    }
    std::unique_ptr<wire::MemberT> member(ByteView user_id) {
        auto st = db_.prepare("SELECT username FROM users WHERE user_id = ?");
        st.bind(1, user_id);
        if (!st.step()) return nullptr;
        auto m = std::make_unique<wire::MemberT>();
        m->user_id = to_bytes(user_id);
        m->username = st.text(0);
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
    Bytes create_channel(const std::string& name) {
        Bytes room_id = random_bytes(16);
        auto st = db_.prepare("INSERT INTO rooms (room_id, created_at, is_direct, kind, name) "
                              "VALUES (?, ?, 0, 0, ?)");
        st.bind(1, room_id).bind(2, now_ms()).bind(3, name).exec();
        return room_id;
    }
    void rename_channel(ByteView room_id, const std::string& name) {
        auto st = db_.prepare("UPDATE rooms SET name = ? WHERE room_id = ? AND kind = 0");
        st.bind(1, name).bind(2, room_id).exec();
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
            auto st = db_.prepare("SELECT created_at, kind, name FROM rooms WHERE room_id = ?");
            st.bind(1, room_id);
            if (st.step()) {
                info.created_at = st.u64(0);
                info.kind = static_cast<uint8_t>(st.i64(1));
                info.is_direct = info.kind == kDirect;
                info.name = st.text(2);
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
                                   "event_id, sender_user, sender_device, server_ts, ciphertext) "
                                   "VALUES (?,?,?,?,?,?,?,?)");
            ins.bind(1, ev.room_id).bind(2, out.seq).bind(3, r->device_id).bind(4, ev.event_id)
                .bind(5, sender_user).bind(6, sender_device).bind(7, out.server_ts)
                .bind(8, r->ciphertext).exec();
        }
        tx.commit();
        return out;
    }

    std::vector<wire::RoomEventT> events_after(ByteView room_id, ByteView device_id, uint64_t seq) {
        std::vector<wire::RoomEventT> out;
        auto st = db_.prepare("SELECT seq, event_id, sender_user, sender_device, server_ts, "
                              "ciphertext FROM room_events WHERE room_id = ? AND "
                              "recipient_device = ? AND seq > ? ORDER BY seq");
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
            out.push_back(std::move(e));
        }
        return out;
    }

private:
    db::Database db_;
};

}  // namespace corded::server

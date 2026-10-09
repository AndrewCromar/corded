// cordedd storage: SQLite in WAL mode. The server only ever stores public
// keys, membership and ciphertext.
#pragma once

#include "corded/common/db.hpp"
#include "corded/common/frame.hpp"

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
inline constexpr int kBanned = -1, kUser = 0, kAdmin = 1;

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
        // Added after the first prototype: two-person chats are marked, so a
        // group that shrinks to two people is not mistaken for one.
        try {
            db_.exec("SELECT is_direct FROM rooms LIMIT 0");
        } catch (const db::Error&) {
            db_.exec("ALTER TABLE rooms ADD COLUMN is_direct INTEGER NOT NULL DEFAULT 0");
            db_.exec("UPDATE rooms SET is_direct = 1 WHERE "
                     "(SELECT COUNT(*) FROM memberships m WHERE m.room_id = rooms.room_id) = 2");
        }
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

    // Used at start-up to promote the administrators the operator named.
    void promote_by_username(std::string_view username) {
        auto st = db_.prepare("UPDATE users SET access_level = 1 WHERE username = ?");
        st.bind(1, username).exec();
    }

    bool user_exists(ByteView user_id) {
        auto st = db_.prepare("SELECT 1 FROM users WHERE user_id = ?");
        st.bind(1, user_id);
        return st.step();
    }

    bool is_member(ByteView room_id, ByteView user_id) {
        auto st = db_.prepare("SELECT 1 FROM memberships WHERE room_id = ? AND user_id = ?");
        st.bind(1, room_id).bind(2, user_id);
        return st.step();
    }

    wire::RoomInfoT room_info(ByteView room_id) {
        wire::RoomInfoT info;
        info.room_id = to_bytes(room_id);
        {
            auto st = db_.prepare("SELECT created_at, is_direct FROM rooms WHERE room_id = ?");
            st.bind(1, room_id);
            if (st.step()) {
                info.created_at = st.u64(0);
                info.is_direct = st.i64(1) != 0;
            }
        }
        auto st = db_.prepare("SELECT u.user_id, u.username, u.access_level FROM memberships m "
                              "JOIN users u ON u.user_id = m.user_id WHERE m.room_id = ? "
                              "ORDER BY u.username");
        st.bind(1, room_id);
        while (st.step()) {
            auto m = std::make_unique<wire::MemberT>();
            m->user_id = st.blob(0);
            m->username = st.text(1);
            m->is_admin = st.i64(2) == kAdmin;
            info.members.push_back(std::move(m));
        }
        return info;
    }

    std::vector<Bytes> rooms_of_user(ByteView user_id) {
        std::vector<Bytes> out;
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
        auto st = db_.prepare("INSERT INTO rooms (room_id, created_at, is_direct) VALUES (?, ?, ?)");
        st.bind(1, room_id).bind(2, now_ms()).bind(3, members.size() == 2 ? 1 : 0).exec();
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

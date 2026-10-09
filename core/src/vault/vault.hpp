// The local vault: an encrypted SQLite database holding this device's keys,
// sessions, rooms and decrypted events.
//
// Key hierarchy: passphrase -> Argon2id -> key-encryption key, which unwraps a
// random vault key, which encrypts the database.
#pragma once

#include "corded/common/db.hpp"
#include "crypto/crypto.hpp"

#include <optional>
#include <string>
#include <vector>

namespace corded {

class VaultError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class WrongPassphrase : public VaultError {
public:
    WrongPassphrase() : VaultError("wrong passphrase") {}
};

struct MemberRow {
    Bytes user_id;
    std::string username;
    bool is_admin = false;  // the owner, or holds the administrator permission
    bool is_owner = false;
    std::vector<uint32_t> roles;  // role ids, not counting @everyone
};

struct RoomRow {
    Bytes room_id;
    std::string name;  // empty until someone names the room
    int kind = 2;      // 0 channel of the server, 1 direct message, 2 group
    uint64_t ttl_s = 0;  // if set, new messages here disappear after this many seconds
    std::string channel_name;  // for channels, the name the server gives
    uint64_t acked_seq = 0;
    std::vector<MemberRow> members;
};

struct EventRow {
    Bytes room_id, event_id;
    std::optional<uint64_t> seq;  // unset while the event is only in the outbox
    std::string type;
    uint16_t type_version = 1;
    Bytes sender_user, sender_device;
    uint64_t origin_ts = 0, server_ts = 0;
    std::string state_key;
    std::string content;  // JSON
    std::string edited_content;  // JSON of the latest accepted edit, if any
    uint64_t expires_at = 0;  // ms since epoch; 0 = never. The event is erased then
    std::string fallback_text;
    std::string status;  // ok, pending, failed, undecryptable, redacted
    std::string rel_kind;
    Bytes rel_target;
    std::string rel_key;
};

struct OutboxRow {
    int64_t local_id = 0;
    Bytes room_id, event_id;
};

class Vault : public crypto::PrekeySource {
public:
    static bool exists(const std::string& dir);

    // fast_kdf lowers the Argon2id cost; it exists for tests only.
    void create(const std::string& dir, ByteView passphrase, const std::string& username,
                bool fast_kdf);
    void unlock(const std::string& dir, ByteView passphrase);
    void lock();
    bool unlocked() const { return db_.is_open(); }

    db::Database& db() { return db_; }
    const crypto::Identity& identity() const { return identity_; }
    const std::string& username() const { return username_; }

    std::optional<std::string> meta(const std::string& key);
    void set_meta(const std::string& key, const std::string& value);

    // Prekeys. kind 0 = signed, 1 = one-time.
    crypto::KeyPair add_prekey(int kind, uint32_t id);
    uint32_t next_prekey_id();
    std::optional<crypto::KeyPair> signed_prekey(uint32_t id) override;
    std::optional<crypto::KeyPair> take_one_time(uint32_t id) override;

    std::optional<crypto::PeerSessions> load_sessions(ByteView peer_user);
    void save_sessions(const crypto::PeerSessions& peer);

    // People whose safety number the user has checked.
    bool is_verified(ByteView user_id);
    void set_verified(ByteView user_id, bool verified);

    // kind < 0 leaves the stored kind and channel name alone.
    void upsert_room(ByteView room_id, const std::vector<MemberRow>& members, int kind = -1,
                     const std::string& channel_name = "");
    std::vector<RoomRow> rooms();
    std::optional<RoomRow> room(ByteView room_id);
    void advance_cursor(ByteView room_id, uint64_t seq);
    void set_room_name(ByteView room_id, const std::string& name);
    void delete_room(ByteView room_id);  // the room, its members and its history
    void set_room_ttl(ByteView room_id, uint64_t seconds);
    // Erases every event whose time has come and returns (room id, event id) of each.
    std::vector<std::pair<Bytes, Bytes>> expire_events(uint64_t now_ms);

    bool insert_event(const EventRow& e);  // false if the event id is already stored
    bool has_event(ByteView room_id, ByteView event_id);
    std::optional<EventRow> event(ByteView room_id, ByteView event_id);
    void confirm_event(ByteView room_id, ByteView event_id, uint64_t seq, uint64_t server_ts);
    void set_event_status(ByteView room_id, ByteView event_id, const std::string& status);
    std::vector<EventRow> timeline(ByteView room_id, uint32_t limit);
    // Events that point at `target` with the given relation kind, oldest first.
    std::vector<EventRow> related(ByteView room_id, ByteView target, const std::string& kind);
    uint32_t related_count(ByteView room_id, ByteView target, const std::string& kind);
    void set_edited_content(ByteView room_id, ByteView event_id, const std::string& content);
    void redact_event(ByteView room_id, ByteView event_id);  // erases the content for good

    void outbox_push(ByteView room_id, ByteView event_id);
    std::vector<OutboxRow> outbox();
    void outbox_remove(int64_t local_id);

private:
    void open_database(const std::string& dir, const Key32& vault_key);
    void migrate();
    void load_identity();

    db::Database db_;
    crypto::Identity identity_;
    std::string username_;
};

}  // namespace corded

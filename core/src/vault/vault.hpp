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
    std::string nickname;  // display name held by the server, if one was set there
    std::string profile_name;  // display name from their own profile, which no server sees
    bool bot = false;          // their profile says they are a program, not a person
    // What to show for this person: their own choice first.
    const std::string& display() const {
        return !profile_name.empty() ? profile_name : nickname.empty() ? username : nickname;
    }
};

struct RoomRow {
    Bytes room_id;
    std::string name;  // empty until someone names the room
    int kind = 2;      // 0 channel of the server, 1 direct message, 2 group
    uint64_t ttl_s = 0;  // if set, new messages here disappear after this many seconds
    int64_t server_id = 1;  // which of the vault's servers this room belongs to
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
    Bytes shared_by;  // set on history a member handed over, to that member's user id
    std::string fallback_text;
    std::string status;  // ok, pending, failed, undecryptable, redacted
    std::string rel_kind;
    Bytes rel_target;
    std::string rel_key;
};

struct OutboxRow {
    int64_t local_id = 0;
    Bytes room_id, event_id;
    Bytes only_user;  // if set, the event goes to this one member, not the room
};

class Vault : public crypto::PrekeySource {
public:
    static bool exists(const std::string& dir);

    // fast_kdf lowers the Argon2id cost; it exists for tests only.
    // With `seed`, the vault is for an existing person on a new device.
    void create(const std::string& dir, ByteView passphrase, const std::string& username,
                bool fast_kdf, const std::optional<Key32>& seed = std::nullopt);
    void unlock(const std::string& dir, ByteView passphrase);
    // Throws WrongPassphrase unless this passphrase opens the vault in `dir`.
    static void check_passphrase(const std::string& dir, ByteView passphrase);
    void lock();
    bool unlocked() const { return db_.is_open(); }

    db::Database& db() { return db_; }
    const crypto::Identity& identity() const { return identity_; }
    const std::string& username() const { return username_; }
    void set_username(const std::string& username);

    std::optional<std::string> meta(const std::string& key);
    void set_meta(const std::string& key, const std::string& value);

    // Prekeys. kind 0 = signed, 1 = one-time.
    crypto::KeyPair add_prekey(int kind, uint32_t id);
    uint32_t next_prekey_id();
    std::optional<crypto::KeyPair> signed_prekey(uint32_t id) override;
    std::optional<crypto::KeyPair> take_one_time(uint32_t id) override;

    // Encrypted sessions are kept per device, since one person may have several.
    std::optional<crypto::PeerSessions> load_sessions(ByteView peer_device);
    void save_sessions(const crypto::PeerSessions& peer);

    // People whose safety number the user has checked.
    bool is_verified(ByteView user_id);
    void set_verified(ByteView user_id, bool verified);

    // kind < 0 leaves the stored kind and channel name alone. server_id is
    // recorded when the room is first stored.
    void upsert_room(ByteView room_id, const std::vector<MemberRow>& members, int kind = -1,
                     const std::string& channel_name = "", int64_t server_id = 1);
    // server_id 0 means the rooms of every server.
    std::vector<RoomRow> rooms(int64_t server_id = 0);
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
    // The newest `limit` events, oldest first. With `before_seq`, the newest
    // `limit` among those the server ordered earlier than that.
    std::vector<EventRow> timeline(ByteView room_id, uint32_t limit, uint64_t before_seq = 0);
    // Text messages whose words contain `text` (case does not matter), newest
    // first; in one room, or everywhere when room_id is empty.
    std::vector<EventRow> search(const std::string& text, ByteView room_id, uint32_t limit);
    // Events that point at `target` with the given relation kind, oldest first.
    std::vector<EventRow> related(ByteView room_id, ByteView target, const std::string& kind);
    uint32_t related_count(ByteView room_id, ByteView target, const std::string& kind);
    void set_edited_content(ByteView room_id, ByteView event_id, const std::string& content);
    void redact_event(ByteView room_id, ByteView event_id);  // erases the content for good

    void outbox_push(ByteView room_id, ByteView event_id, ByteView only_user = {});

    // Read receipts: for each person in a room, the newest message they have
    // read. Returns true if this moved their marker forward.
    struct Receipt {
        Bytes user_id, event_id;
        uint64_t seq = 0;
    };
    bool set_receipt(ByteView room_id, ByteView user_id, ByteView event_id, uint64_t seq);
    std::vector<Receipt> receipts(ByteView room_id);

    // Profiles: what people have told this person about themselves. Kept only
    // here and in their own vaults; no server holds them. Returns true if this
    // one was newer than what was stored.
    bool set_profile(ByteView user_id, uint64_t version, const std::string& json_text);
    std::optional<std::string> profile(ByteView user_id);
    // Moves a person's marker back to just before the event at `before_seq`.
    void rewind_receipt(ByteView room_id, ByteView user_id, uint64_t before_seq);
    // The first message from someone else after this person's marker; empty if none.
    Bytes first_unread(ByteView room_id, ByteView user_id);
    // Messages from others that arrived after this person's read marker.
    uint32_t unread(ByteView room_id, ByteView user_id);
    std::vector<OutboxRow> outbox();
    void outbox_remove(int64_t local_id);

private:
    static Key32 unwrap_key(const std::string& dir, ByteView passphrase);
    void open_database(const std::string& dir, const Key32& vault_key);
    void migrate();
    void load_identity();

    db::Database db_;
    crypto::Identity identity_;
    std::string username_;
};

}  // namespace corded

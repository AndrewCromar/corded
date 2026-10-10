#include "corded/corded.h"
#include "engine/engine.hpp"

#include "corded/common/release.hpp"
#include "corded/common/sig.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace corded {

namespace {

using asio::ip::tcp;

constexpr uint16_t kDisconnected = 1;  // synthetic error code for dropped requests
constexpr int kOneTimePrekeyBatch = 50;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// 48-bit millisecond timestamp followed by 80 random bits: unique and sortable.
Bytes new_event_id() {
    Bytes id = random_bytes(16);
    uint64_t ts = now_ms();
    for (int i = 0; i < 6; ++i) id[i] = static_cast<uint8_t>(ts >> (8 * (5 - i)));
    return id;
}

Bytes need_b64(const nlohmann::json& j, const char* key, size_t size) {
    auto v = unb64(j.at(key).get<std::string>());
    if (!v || v->size() != size) throw std::invalid_argument(std::string("bad ") + key);
    return *v;
}

Bytes context_of(ByteView room_id, ByteView event_id) {
    Bytes c = to_bytes(room_id);
    append(c, event_id);
    return c;
}

// The note each device's ratchet carries when an event is sent in the shared
// form: a mark, the key and nonce the event was encrypted under, and a
// fingerprint of the encrypted event. The fingerprint is what stops anyone
// who holds the key (every recipient does) from swapping in different words
// under the sender's name.
constexpr char kSharedMark[4] = {'C', 'S', 'K', '1'};

Bytes shared_key_note(ByteView key, ByteView nonce, ByteView shared) {
    Bytes note(kSharedMark, kSharedMark + 4);
    append(note, key);
    append(note, nonce);
    Bytes print(crypto_generichash_BYTES);
    crypto_generichash(print.data(), print.size(), shared.data(), shared.size(), nullptr, 0);
    append(note, print);
    return note;
}

std::optional<Bytes> open_shared(ByteView note, ByteView shared, ByteView context) {
    if (note.size() != 4 + 32 + 24 + crypto_generichash_BYTES || !std::equal(kSharedMark, kSharedMark + 4, note.begin()))
        return std::nullopt;
    Bytes print(crypto_generichash_BYTES);
    crypto_generichash(print.data(), print.size(), shared.data(), shared.size(), nullptr, 0);
    if (sodium_memcmp(print.data(), note.data() + 60, print.size()) != 0) return std::nullopt;
    Key32 key{};
    std::copy(note.begin() + 4, note.begin() + 36, key.begin());
    auto plain = crypto::aead_decrypt(key, note.subspan(36, 24), shared, context);
    sodium_memzero(key.data(), key.size());
    return plain;
}

bool known_type(const std::string& type) {
    return type == "m.text" || type == "m.reaction" || type == "m.room.name" || type == "m.edit" ||
           type == "m.redaction" || type == "m.room.member" || type == "m.room.retention" ||
           type == "m.history.share" || type == "m.receipt" || type == "m.room.pin" || type == "m.profile" ||
           type == "m.poll" || type == "m.poll.vote" || type == "m.file" || type == "m.task" ||
           type == "m.task.done";
}

// The largest file this client will send or fetch. It is held in memory whole
// while it is encrypted, so this is a guard, not a promise; servers set their
// own, usually lower, limit.
constexpr uint64_t kMaxFileBytes = 200ull * 1024 * 1024;

// A name that is safe to give a file on this device, whatever the sender called it.
std::string safe_file_name(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (out.size() >= 80) break;
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c >= 0x80) out += static_cast<char>(c);
        else if (c == ' ') out += '_';
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    return out.empty() ? "file" : out;
}

std::string hex_of(ByteView v) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : v) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

}  // namespace

const char* conn_name(int c) {
    static const char* names[] = {"disconnected", "connecting", "authenticating", "syncing", "live"};
    return names[c];
}

// ---------------------------------------------------------------- engine

Engine::Engine(EngineConfig config)
    : config_(std::move(config)),
      work_(asio::make_work_guard(io_)),
      tls_ctx_(asio::ssl::context::tls_client),
      sweep_timer_(io_) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium failed to initialise");
    // Servers are authenticated by their pinned fingerprint, checked after the
    // handshake, so certificate-authority verification is switched off.
    tls::require_tls13(tls_ctx_);
    tls_ctx_.set_verify_mode(asio::ssl::verify_none);
    thread_ = std::thread([this] { io_.run(); });
    asio::post(io_, [this] { emit_vault_state(); });
}

Engine::~Engine() {
    asio::post(io_, [this] {
        close_sessions();
        sweep_timer_.cancel();
        vault_.lock();
        io_.stop();
    });
    work_.reset();
    if (thread_.joinable()) thread_.join();
}

bool Engine::vault_exists() const { return Vault::exists(config_.vault_dir); }

void Engine::close_sessions() {
    for (auto& [id, s] : sessions_) s->disconnect("shutting down");
    sessions_.clear();
}

void Engine::emit(json j) {
    j["seq"] = event_seq_++;
    std::string text = j.dump();
    {
        std::lock_guard lock(mu_);
        events_.push_back(std::move(text));
    }
    cv_.notify_one();
}

std::optional<std::string> Engine::next_event(int timeout_ms) {
    std::unique_lock lock(mu_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms),
                      [this] { return !events_.empty(); }))
        return std::nullopt;
    std::string out = std::move(events_.front());
    events_.pop_front();
    return out;
}

void Engine::ok(uint64_t req, json data) {
    emit({{"event", "command_result"}, {"request", req}, {"ok", true}, {"data", std::move(data)}});
}

void Engine::fail(uint64_t req, const std::string& code, const std::string& message) {
    emit({{"event", "command_result"},
          {"request", req},
          {"ok", false},
          {"error", {{"code", code}, {"message", message}}}});
}

void Engine::emit_vault_state() {
    json j = {{"event", "vault_state"}};
    if (vault_.unlocked()) {
        j["state"] = "unlocked";
        j["user_id"] = b64(vault_.identity().user.pk);
        j["username"] = vault_.username();
    } else {
        j["state"] = vault_exists() ? "locked" : "missing";
    }
    emit(std::move(j));
}

// The list of servers this vault belongs to: [{"id":1,"host":"..","port":".."}].
Engine::json Engine::load_servers() {
    if (auto stored = vault_.meta("servers")) {
        json list = json::parse(*stored, nullptr, false);
        if (list.is_array()) return list;
    }
    json list = json::array();
    // A vault from before several servers were supported has exactly one.
    auto host = vault_.meta("server_host");
    auto port = vault_.meta("server_port");
    if (host && port) {
        list.push_back({{"id", 1}, {"host", *host}, {"port", *port}});
        for (const char* key : {"registered", "prekeys_published", "server_fp", "invite"})
            if (auto v = vault_.meta(key)) vault_.set_meta(std::string("s1:") + key, *v);
        save_servers(list);
    }
    return list;
}

void Engine::save_servers(const json& list) { vault_.set_meta("servers", list.dump()); }

std::string Engine::effective_presence() {
    std::string choice = vault_.unlocked() ? vault_.meta("presence").value_or("auto") : "auto";
    if (choice == "dnd" || choice == "invisible") return choice;
    return active_ ? "online" : "away";
}

Session* Engine::session(int64_t id) {
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

Session* Engine::session_for_room(ByteView room_id) {
    auto room = vault_.room(room_id);
    return room ? session(room->server_id) : nullptr;
}

// Commands that name no server go to the first one, which keeps a client that
// only ever uses one server simple.
Session* Engine::default_session() { return sessions_.empty() ? nullptr : sessions_.begin()->second.get(); }

// ---------------------------------------------------------------- session

Session::Session(Engine& engine, int64_t id, std::string host, std::string port)
    : engine_(engine),
      id_(id),
      io_(engine.io_),
      vault_(engine.vault_),
      tls_ctx_(engine.tls_ctx_),
      next_request_(engine.next_request_),
      resolver_(engine.io_),
      reconnect_timer_(engine.io_),
      reshare_timer_(engine.io_),
      host_(std::move(host)),
      port_(std::move(port)) {}

Session::~Session() { disconnect(""); }

void Session::emit(json j) {
    j["server_id"] = id_;
    engine_.emit(std::move(j));
}
void Session::ok(uint64_t req, json data) { engine_.ok(req, std::move(data)); }
void Session::fail(uint64_t req, const std::string& code, const std::string& message) {
    engine_.fail(req, code, message);
}

std::optional<std::string> Session::meta(const std::string& key) {
    return vault_.meta("s" + std::to_string(id_) + ":" + key);
}
void Session::set_meta(const std::string& key, const std::string& value) {
    vault_.set_meta("s" + std::to_string(id_) + ":" + key, value);
}
std::vector<RoomRow> Session::rooms() { return vault_.rooms(id_); }

const char* Session::conn_name() const { return corded::conn_name(static_cast<int>(conn_)); }

void Session::emit_rooms() {
    for (const auto& room : rooms()) emit({{"event", "room_updated"}, {"room", room_json(room)}});
}

Session::json Session::rooms_json() {
    json out = json::array();
    for (const auto& room : rooms()) out.push_back(room_json(room));
    return out;
}

void Session::resume() {
    want_connection_ = true;
    start_connect();
}

void Session::disconnect(const std::string& reason) {
    want_connection_ = false;
    drop_connection(reason);
}

// Applies what a connect command or invite link says, then (re)connects.
void Session::connect(uint64_t req, const json& cmd) {
    // An explicit fingerprint (from an invite) must match. "reset_pin" accepts
    // whatever identity the server presents next; use it only on purpose.
    if (cmd.contains("invite")) set_meta("invite", cmd.at("invite").get<std::string>());
    if (cmd.contains("fingerprint")) set_meta("server_fp", cmd.at("fingerprint").get<std::string>());
    else if (cmd.value("reset_pin", false)) set_meta("server_fp", "");
    want_connection_ = true;
    backoff_s_ = 1;
    drop_connection("");
    start_connect();
    ok(req, {{"server_id", id_}});
}

void Session::send_presence() {
    if (conn_ != Conn::Live) return;
    wire::EphemeralT eph;
    eph.kind = "presence:" + engine_.effective_presence();
    send_frame(make_frame(0, std::move(eph)));
}

void Session::set_conn(Conn c, const std::string& detail) {
    conn_ = c;
    json j = {{"event", "connection_state"}, {"state", corded::conn_name(static_cast<int>(c))}};
    if (!detail.empty()) j["detail"] = detail;
    if (!host_.empty()) j["server"] = host_ + ":" + port_;
    if (!server_fingerprint_.empty() && c != Conn::Disconnected && c != Conn::Connecting)
        j["fingerprint"] = server_fingerprint_;
    emit(std::move(j));
}

Session::json Session::member_json(const MemberRow& m) {
    bool me = m.user_id.size() == 32 &&
              std::equal(m.user_id.begin(), m.user_id.end(), vault_.identity().user.pk.begin());
    json roles = json::array();
    for (uint32_t id : m.roles)
        for (const auto& r : roles_)
            if (r.id == id) roles.push_back(r.name);
    return {{"user_id", b64(m.user_id)},
            {"username", m.username},
            {"nickname", m.nickname},
            {"display_name", m.display()},
            {"me", me},
            {"bot", m.bot},
            {"status", presence_.count(m.user_id) ? presence_[m.user_id] : std::string("offline")},
            {"is_owner", m.is_owner},
            {"is_admin", m.is_admin},
            {"roles", std::move(roles)},
            {"verified", !me && vault_.is_verified(m.user_id)}};
}

Session::json Session::room_json(const RoomRow& room) {
    json members = json::array();
    std::string title;
    for (const auto& m : room.members) {
        json j = member_json(m);
        if (!j["me"].get<bool>()) title += (title.empty() ? "" : ", ") + m.display();
        members.push_back(std::move(j));
    }
    static const char* kinds[] = {"channel", "direct", "group"};
    if (room.kind == 0) title = "#" + room.channel_name;
    else if (!room.name.empty()) title = room.name;
    return {{"room_id", b64(room.room_id)},
            {"server_id", room.server_id},
            {"title", title.empty() ? "(empty room)" : title},
            {"name", room.kind == 0 ? room.channel_name : room.name},
            {"kind", kinds[room.kind >= 0 && room.kind <= 2 ? room.kind : 2]},
            {"is_group", room.kind == 2},
            {"disappear_after", room.ttl_s},
            {"pinned", pins(room.room_id)},
            {"nsfw", vault_.meta("nsfw:" + b64(room.room_id)).value_or("0") == "1"},
            // Pinned to the top of the list by whoever manages the channels.
            {"featured", vault_.meta("featured:" + b64(room.room_id)).value_or("0") == "1"},
            // Whether this person may write here, as the server last said.
            {"can_send", vault_.meta("can_send:" + b64(room.room_id)).value_or("1") != "0"},
            // Which of the server's sections the channel is listed under; 0 for none.
            {"section", std::strtoul(vault_.meta("section:" + b64(room.room_id)).value_or("0").c_str(), nullptr, 10)},
            {"archived", vault_.meta("archived:" + b64(room.room_id)).value_or("0") == "1"},
            // How a channel is laid out: "" for messages, "tasks" for a task list.
            {"channel_type", vault_.meta("channel_type:" + b64(room.room_id)).value_or("")},
            {"unread", vault_.unread(room.room_id, to_bytes(vault_.identity().user.pk))},
            {"first_unread", b64(vault_.first_unread(room.room_id, to_bytes(vault_.identity().user.pk)))},
            {"members", std::move(members)}};
}

// What this client knows about the community it is connected to.
Session::json Session::server_json() {
    json roles = json::array();
    for (const auto& r : roles_)
        roles.push_back({{"name", r.name},
                         {"position", r.position},
                         {"is_everyone", r.is_everyone},
                         {"permissions", perm::to_names(r.permissions)}});
    bool owner = server_owner_.size() == 32 &&
                 std::equal(server_owner_.begin(), server_owner_.end(), vault_.identity().user.pk.begin());
    // Named groups of channels, in the order they are listed.
    json sections = json::array();
    for (const auto& s : sections_)
        sections.push_back({{"section_id", s.id}, {"name", s.name}, {"position", s.position}});
    return {{"server_id", id_},
            {"address", host_ + ":" + port_},
            {"name", server_name_},
            {"description", server_description_},
            {"icon", server_icon_},
            {"is_owner", owner},
            {"history_sharing", history_sharing_},
            {"my_permissions", perm::to_names(my_permissions_)},
            {"sections", std::move(sections)},
            {"roles", std::move(roles)}};
}

Session::json Session::event_json(const EventRow& e) {
    json j = {{"room_id", b64(e.room_id)},
              {"event_id", b64(e.event_id)},
              {"type", e.type},
              {"type_version", e.type_version},
              {"sender_user", b64(e.sender_user)},
              {"origin_ts", e.origin_ts},
              {"server_ts", e.server_ts},
              {"status", e.status},
              {"known_type", known_type(e.type)}};
    j["seq"] = e.seq ? json(*e.seq) : json(nullptr);
    j["mine"] = e.sender_user.size() == 32 &&
                std::equal(e.sender_user.begin(), e.sender_user.end(),
                           vault_.identity().user.pk.begin());
    // An accepted edit replaces what is shown; the original stays in the vault.
    j["content"] = json::parse(e.edited_content.empty() ? e.content : e.edited_content, nullptr, false);
    if (j["content"].is_discarded()) j["content"] = json::object();
    j["edited"] = !e.edited_content.empty();
    j["expires_at"] = e.expires_at;
    // Old messages a member handed over: the client is taking that member's
    // word for who said what, so frontends should mark them.
    j["shared_history"] = !e.shared_by.empty();
    // How many messages hang off this one as a thread.
    j["thread_count"] = vault_.related_count(e.room_id, e.event_id, "thread");
    if (!e.fallback_text.empty()) j["fallback_text"] = e.fallback_text;
    if (e.status == "scheduled")
        j["scheduled_for"] = std::strtoull(vault_.meta("sched:" + b64(e.event_id)).value_or("0").c_str(), nullptr, 10);
    // A task is done or not according to the newest tick anyone gave it.
    if (e.type == "m.task") {
        json task = {{"done", false}};
        for (const auto& tick : vault_.related(e.room_id, e.event_id, "task")) {
            if (tick.type != "m.task.done" || tick.status == "redacted" || tick.status == "undecryptable") continue;
            json c = json::parse(tick.content, nullptr, false);
            if (!c.is_object()) continue;
            task = {{"done", c.value("done", false)}, {"by", b64(tick.sender_user)}, {"at", tick.origin_ts}};
            if (auto room = vault_.room(e.room_id))
                for (const auto& m : room->members)
                    if (m.user_id == tick.sender_user) task["by_name"] = m.display();
        }
        j["task"] = std::move(task);
    }
    if (!e.rel_kind.empty()) {
        j["relation"] = {{"kind", e.rel_kind}, {"target", b64(e.rel_target)}};
        if (!e.rel_key.empty()) j["relation"]["key"] = e.rel_key;
    }
    if (auto room = vault_.room(e.room_id))
        for (const auto& m : room->members)
            if (m.user_id == e.sender_user) {
                j["sender_name"] = m.display();
                j["sender_username"] = m.username;
            }
    return j;
}

// ---------------------------------------------------------------- commands in

uint64_t Engine::vault_create(Bytes passphrase, std::string username, std::string recovery_key) {
    uint64_t req = next_request_++;
    asio::post(io_, [this, req, pass = std::move(passphrase), name = std::move(username),
                     recovery = std::move(recovery_key)]() mutable {
        try {
            if (vault_.unlocked()) throw VaultError("the vault is already open");
            std::optional<Key32> seed;
            if (!recovery.empty()) {
                seed = crypto::decode_recovery_key(recovery);
                sodium_memzero(recovery.data(), recovery.size());
                if (!seed) throw VaultError("that recovery key is not valid; check it for typing mistakes");
            }
            vault_.create(config_.vault_dir, pass, name, config_.fast_kdf, seed);
            sodium_memzero(pass.data(), pass.size());
            after_unlock();
            ok(req);
        } catch (const std::exception& e) {
            sodium_memzero(pass.data(), pass.size());
            fail(req, "vault_error", e.what());
        }
    });
    return req;
}

uint64_t Engine::vault_unlock(Bytes passphrase) {
    uint64_t req = next_request_++;
    asio::post(io_, [this, req, pass = std::move(passphrase)]() mutable {
        try {
            if (vault_.unlocked()) {
                // Already open: this only checks the passphrase, so a frontend
                // can confirm it before, say, keeping it for fingerprint unlock.
                Vault::check_passphrase(config_.vault_dir, pass);
                sodium_memzero(pass.data(), pass.size());
                ok(req, {{"already_open", true}});
                return;
            }
            vault_.unlock(config_.vault_dir, pass);
            sodium_memzero(pass.data(), pass.size());
            after_unlock();
            ok(req);
        } catch (const WrongPassphrase&) {
            sodium_memzero(pass.data(), pass.size());
            fail(req, "wrong_passphrase", "wrong passphrase");
        } catch (const std::exception& e) {
            sodium_memzero(pass.data(), pass.size());
            fail(req, "vault_error", e.what());
        }
    });
    return req;
}

// Erases disappearing messages whose time has come and tells the frontend.
void Engine::sweep_expired() {
    if (!vault_.unlocked()) return;
    for (const auto& [room_id, event_id] : vault_.expire_events(now_ms()))
        emit({{"event", "event_expired"}, {"room_id", b64(room_id)}, {"event_id", b64(event_id)}});
}

void Engine::schedule_sweep() {
    sweep_timer_.expires_after(std::chrono::seconds(1));
    sweep_timer_.async_wait([this](asio::error_code ec) {
        if (ec || !vault_.unlocked()) return;
        try {
            sweep_expired();
        } catch (const std::exception& e) {
            emit({{"event", "warning"}, {"message", std::string("expiry sweep failed: ") + e.what()}});
        }
        schedule_sweep();
    });
}

// Readable copies of files exist only while someone is looking at them. The
// view folder is emptied whenever the vault is locked or unlocked, and so is
// anything readable left in the kept-files folder by versions that stored
// fetched files decrypted.
void Engine::wipe_views() {
    std::error_code ec;
    std::filesystem::remove_all(config_.vault_dir + "/view", ec);
    for (const auto& entry : std::filesystem::directory_iterator(config_.vault_dir + "/files", ec))
        if (entry.is_regular_file(ec) && entry.path().extension() != ".enc") std::filesystem::remove(entry.path(), ec);
}

void Engine::after_unlock() {
    wipe_views();
    emit_vault_state();
    sweep_expired();  // anything that came due while the vault was locked
    schedule_sweep();
    // Bring back every server this vault belongs to and reconnect to each.
    for (const auto& entry : load_servers()) {
        int64_t id = entry.value("id", int64_t{0});
        if (id <= 0 || sessions_.count(id)) continue;
        auto s = std::make_unique<Session>(*this, id, entry.value("host", ""), entry.value("port", ""));
        Session* raw = s.get();
        sessions_[id] = std::move(s);
        raw->emit_rooms();
        raw->resume();
    }
}

uint64_t Engine::command(std::string json_text) {
    uint64_t req = next_request_++;
    asio::post(io_, [this, req, text = std::move(json_text)] { run_command(req, text); });
    return req;
}

// Commands about the vault or about which servers exist are handled here; the
// rest go to the session of the server they concern.
void Engine::run_command(uint64_t req, const std::string& text) {
    try {
        json cmd = json::parse(text);
        std::string name = cmd.at("cmd").get<std::string>();

        if (name == "verify_release") {
            // An app checks a downloaded update before it installs it: the
            // file must carry the release key's signature. Needs no vault.
            auto read = [](const std::string& path) -> std::optional<Bytes> {
                std::ifstream in(path, std::ios::binary);
                if (!in) return std::nullopt;
                return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            };
            auto file = read(cmd.at("path").get<std::string>());
            auto signature = read(cmd.at("signature_path").get<std::string>());
            auto key = release::public_key_from_pem(CORDED_RELEASE_KEY);
            if (!file || !signature || !key) {
                fail(req, "invalid_argument", "the update or its signature could not be read");
                return;
            }
            ok(req, {{"valid", release::signed_by(*file, *signature, *key)}});
            return;
        }
        if (name == "status") {
            Session* first = default_session();
            json servers = json::array();
            for (auto& [id, s] : sessions_) {
                json entry = s->server_json();
                entry["connection"] = s->conn_name();
                servers.push_back(std::move(entry));
            }
            json data = {{"api", CORDED_ABI_VERSION_MINOR},
                         {"vault", vault_.unlocked() ? "unlocked" : vault_exists() ? "locked" : "missing"},
                         {"connection", first ? first->conn_name() : "disconnected"},
                         {"is_admin", first && first->is_admin()},
                         {"servers", std::move(servers)}};
            if (vault_.unlocked()) {
                data["user_id"] = b64(vault_.identity().user.pk);
                data["username"] = vault_.username();
            }
            ok(req, std::move(data));
            return;
        }
        if (!vault_.unlocked()) {
            fail(req, "vault_locked", "unlock the vault first");
            return;
        }
        if (name == "connect") {
            cmd_connect(req, cmd);
            return;
        }
        if (name == "wipe_views") {
            // A frontend closed what it was showing; nothing readable need stay.
            wipe_views();
            ok(req);
            return;
        }
        if (name == "lock") {
            wipe_views();
            close_sessions();
            sweep_timer_.cancel();
            vault_.lock();
            emit_vault_state();
            ok(req);
            return;
        }
        if (name == "set_username") {
            // The name is only a claim until a server accepts it, so it can be
            // changed freely before then, for example when the first choice
            // turned out to be taken.
            std::string wanted = cmd.at("username").get<std::string>();
            for (auto& [id, s] : sessions_)
                if (s->registered()) {
                    fail(req, "refused", "you already have an account on a server as " + vault_.username() +
                                             "; changing the name there is not supported yet");
                    return;
                }
            bool valid = !wanted.empty() && wanted.size() <= 32;
            for (char ch : wanted)
                valid = valid && ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-');
            if (!valid) {
                fail(req, "invalid_argument", "names use a-z, 0-9, _ and -, up to 32 characters");
                return;
            }
            vault_.set_username(wanted);
            emit_vault_state();
            for (auto& [id, s] : sessions_) s->resume();  // try again under the new name
            ok(req, {{"username", wanted}});
            return;
        }
        if (name == "list_servers") {
            json servers = json::array();
            for (auto& [id, s] : sessions_) {
                json entry = s->server_json();
                entry["connection"] = s->conn_name();
                servers.push_back(std::move(entry));
            }
            ok(req, {{"servers", std::move(servers)}});
            return;
        }
        if (name == "list_rooms" && !cmd.contains("server_id")) {
            // Every room on every server.
            json rooms = json::array();
            for (auto& [id, s] : sessions_)
                for (auto& room : s->rooms_json()) rooms.push_back(std::move(room));
            ok(req, {{"rooms", std::move(rooms)}});
            return;
        }
        if (name == "search") {
            // Looks through the messages on this device. Nothing leaves it:
            // no server can search what it cannot read.
            std::string text = cmd.at("text").get<std::string>();
            if (text.size() < 2 || text.size() > 200) {
                fail(req, "invalid_argument", "search for at least two characters");
                return;
            }
            Bytes room_id = cmd.contains("room_id") ? need_b64(cmd, "room_id", 16) : Bytes{};
            json results = json::array();
            for (const auto& e : vault_.search(text, room_id, cmd.value("limit", 50u))) {
                Session* owner = session_for_room(e.room_id);
                if (owner) results.push_back(owner->describe_event(e));
            }
            ok(req, {{"text", text}, {"results", std::move(results)}});
            return;
        }
        if (name == "set_profile") {
            // Changes what this person says about themselves; fields left out
            // stay as they were, and an empty string clears one.
            json profile = own_profile();
            static const std::map<std::string, size_t> limits = {
                {"display_name", 40}, {"full_name", 80}, {"birthday", 10}, {"bio", 500},
                {"email", 120},       {"phone", 40}};
            for (const auto& [field, limit] : limits) {
                if (!cmd.contains(field)) continue;
                std::string value = cmd.at(field).get<std::string>();
                if (value.size() > limit) {
                    fail(req, "invalid_argument", field + " is too long");
                    return;
                }
                if (value.empty()) profile.erase(field);
                else profile[field] = value;
            }
            // "I am a program, not a person." Self-declared, so that people
            // know what they are talking to.
            if (cmd.contains("bot")) {
                if (cmd.at("bot").get<bool>()) profile["bot"] = true;
                else profile.erase("bot");
            }
            if (cmd.contains("picture")) {
                // A small picture, already shrunk by the frontend, as base64.
                // It travels inside the profile, so no server stores it.
                std::string picture = cmd.at("picture").get<std::string>();
                if (picture.size() > 40 * 1024) {
                    fail(req, "invalid_argument", "the picture is too large; shrink it to about 128 pixels");
                    return;
                }
                if (picture.empty()) profile.erase("picture");
                else profile["picture"] = picture;
            }
            if (cmd.contains("links")) {
                json links = json::array();
                for (const auto& link : cmd.at("links")) {
                    std::string value = link.get<std::string>();
                    if (!value.empty() && value.size() <= 200 && links.size() < 5) links.push_back(value);
                }
                if (links.empty()) profile.erase("links");
                else profile["links"] = links;
            }
            profile["version"] = now_ms();
            vault_.set_meta("profile", profile.dump());
            vault_.set_profile(to_bytes(vault_.identity().user.pk), profile["version"].get<uint64_t>(), profile.dump());
            // Tell every chat now; anyone who joins later hears with the next message.
            for (auto& [id, s] : sessions_) s->share_profile_everywhere();
            for (auto& [id, s] : sessions_) s->emit_rooms();
            ok(req, {{"profile", profile}});
            return;
        }
        if (name == "get_profile") {
            // Someone's profile as this device knows it; without user_id, our own.
            if (!cmd.contains("user_id") && !cmd.contains("username")) {
                ok(req, {{"profile", own_profile()}, {"username", vault_.username()}});
                return;
            }
            Bytes user_id;
            if (cmd.contains("user_id")) {
                user_id = need_b64(cmd, "user_id", 32);
            } else {
                // By username: anyone this device shares a chat with.
                std::string wanted = cmd.at("username").get<std::string>();
                for (const auto& room : vault_.rooms())
                    for (const auto& m : room.members)
                        if (m.username == wanted) user_id = m.user_id;
                if (user_id.empty()) {
                    fail(req, "not_found", "you do not share a chat with anyone called " + wanted);
                    return;
                }
            }
            json profile = json::parse(vault_.profile(user_id).value_or("{}"), nullptr, false);
            json answer = {{"user_id", b64(user_id)}, {"profile", profile.is_object() ? profile : json::object()}};
            if (cmd.contains("username")) answer["username"] = cmd.at("username");
            ok(req, std::move(answer));
            return;
        }
        if (name == "set_presence") {
            // auto (online while in use, away otherwise), dnd or invisible.
            std::string status = cmd.at("status").get<std::string>();
            if (status != "auto" && status != "dnd" && status != "invisible") {
                fail(req, "invalid_argument", "status is auto, dnd or invisible");
                return;
            }
            vault_.set_meta("presence", status);
            for (auto& [id, s] : sessions_) s->send_presence();
            ok(req);
            return;
        }
        if (name == "set_active") {
            // The frontend says whether someone is using it right now.
            bool active = cmd.value("active", true);
            if (active != active_) {
                active_ = active;
                for (auto& [id, s] : sessions_) s->send_presence();
            }
            ok(req);
            return;
        }
        if (name == "forget_server") {
            // Leaves a server as far as this device is concerned: the
            // connection, the server's entry and its chats all go. The account
            // on the server is untouched; its owner can remove that.
            int64_t id = cmd.at("server_id").get<int64_t>();
            auto it = sessions_.find(id);
            if (it == sessions_.end()) {
                fail(req, "not_found", "unknown server");
                return;
            }
            it->second->disconnect("left this server");
            for (const auto& room : vault_.rooms(id)) {
                vault_.delete_room(room.room_id);
                emit({{"event", "room_removed"}, {"room_id", b64(room.room_id)}});
            }
            sessions_.erase(it);
            json kept = json::array();
            for (const auto& entry : load_servers())
                if (entry.value("id", int64_t{0}) != id) kept.push_back(entry);
            save_servers(kept);
            emit({{"event", "server_removed"}, {"server_id", id}});
            ok(req);
            return;
        }
        if (name == "disconnect" && !cmd.contains("server_id")) {
            for (auto& [id, s] : sessions_) s->disconnect("disconnected by user");
            ok(req);
            return;
        }

        // Which server is this about? A room says so by itself; otherwise the
        // command may name one, and if not, the first server is meant.
        Session* target = nullptr;
        if (cmd.contains("room_id")) {
            target = session_for_room(need_b64(cmd, "room_id", 16));
            if (!target) {
                fail(req, "not_found", "unknown room");
                return;
            }
        } else if (cmd.contains("server_id")) {
            target = session(cmd.at("server_id").get<int64_t>());
            if (!target) {
                fail(req, "not_found", "unknown server");
                return;
            }
        } else {
            target = default_session();
        }
        if (!target) {
            fail(req, "not_connected", "not connected to a server");
            return;
        }
        if (name == "disconnect") {
            target->disconnect("disconnected by user");
            ok(req);
            return;
        }
        target->run_command(req, name, cmd);
    } catch (const std::exception& e) {
        fail(req, "invalid_argument", e.what());
    }
}

void Session::run_command(uint64_t req, const std::string& name, const json& cmd) {
    try {
        if (name == "list_rooms") {
            ok(req, {{"rooms", rooms_json()}});
        } else if (name == "safety_numbers") {
            // One entry per other member of the room.
            Bytes room_id = need_b64(cmd, "room_id", 16);
            auto room = vault_.room(room_id);
            if (!room) {
                fail(req, "not_found", "unknown room");
                return;
            }
            json list = json::array();
            const Key32& me = vault_.identity().user.pk;
            for (const auto& m : room->members) {
                if (m.user_id.size() != 32 || std::equal(m.user_id.begin(), m.user_id.end(), me.begin()))
                    continue;
                list.push_back({{"user_id", b64(m.user_id)},
                                {"username", m.username},
                                {"safety_number", crypto::safety_number(me, to_key32(m.user_id))},
                                {"verified", vault_.is_verified(m.user_id)}});
            }
            ok(req, {{"room_id", b64(room_id)}, {"safety_numbers", std::move(list)}});
        } else if (name == "set_verified") {
            Bytes user_id = need_b64(cmd, "user_id", 32);
            vault_.set_verified(user_id, cmd.value("verified", true));
            for (const auto& room : rooms())
                for (const auto& m : room.members)
                    if (m.user_id == user_id) {
                        emit({{"event", "room_updated"}, {"room", room_json(room)}});
                        break;
                    }
            ok(req);
        } else if (name == "start_chat") {
            cmd_start_chat(req, cmd);
        } else if (name == "create_room") {
            cmd_create_room(req, cmd);
        } else if (name == "add_member") {
            // Look the person up, add them on the server, then tell the room.
            if (conn_ != Conn::Live) {
                fail(req, "not_connected", "not connected to a server");
                return;
            }
            Bytes room_id = need_b64(cmd, "room_id", 16);
            std::string username = cmd.at("username").get<std::string>();
            wire::LookupUserT q;
            q.username = username;
            request(std::move(q), [this, req, room_id, username](wire::FrameT& f) {
                if (f.body.type != wire::FrameBody_UserInfo) {
                    fail(req, "not_found", "no user called " + username);
                    return;
                }
                wire::AddMemberT add;
                add.room_id = room_id;
                add.user_id = f.body.AsUserInfo()->user_id;
                request(std::move(add), [this, req, room_id, username](wire::FrameT& r) {
                    if (r.body.type != wire::FrameBody_RoomInfo) {
                        auto* e = r.body.AsError();
                        fail(req, "server_error", e ? e->message : "could not add the member");
                        return;
                    }
                    store_room(*r.body.AsRoomInfo());
                    cmd_send_event(next_request_++, {{"room_id", b64(room_id)},
                                                     {"type", "m.room.member"},
                                                     {"content", {{"action", "added"}, {"username", username}}}});
                    ok(req);
                });
            });
        } else if (name == "server_info" || name == "member_list" || name == "create_role" ||
                   name == "edit_role" || name == "delete_role" || name == "grant_role" ||
                   name == "create_channel" || name == "rename_channel" || name == "delete_channel" ||
                   name == "set_channel_access" || name == "channel_access" || name == "set_channel_nsfw" ||
                   name == "purge_files" || name == "set_channel_featured" || name == "edit_section" || name == "set_channel_section" ||
                   name == "list_devices" || name == "remove_device" || name == "set_channel_archived" ||
                   name == "kick" ||
                   name == "ban_user" ||
                   name == "remove_account" || name == "set_nickname" ||
                   name == "create_invite" || name == "revoke_invite" || name == "get_settings" ||
                   name == "set_setting" || name == "restart_server" || name == "server_status" ||
                   name == "update_server") {
            community_command(req, name, cmd);
        } else if (name == "leave_room") {
            if (conn_ != Conn::Live) {
                fail(req, "not_connected", "not connected to a server");
                return;
            }
            Bytes room_id = need_b64(cmd, "room_id", 16);
            wire::LeaveRoomT leave;
            leave.room_id = room_id;
            request(std::move(leave), [this, req, room_id](wire::FrameT& r) {
                if (r.body.type != wire::FrameBody_Ok) {
                    auto* e = r.body.AsError();
                    fail(req, "server_error", e ? e->message : "could not leave the room");
                    return;
                }
                {
                    db::Transaction tx(vault_.db());
                    vault_.delete_room(room_id);
                    tx.commit();
                }
                emit({{"event", "room_removed"}, {"room_id", b64(room_id)}});
                ok(req);
            });
        } else if (name == "set_room_name") {
            cmd_send_event(req, {{"room_id", cmd.at("room_id")},
                                 {"type", "m.room.name"},
                                 {"state_key", ""},
                                 {"content", {{"name", cmd.at("name")}}}});
        } else if (name == "send_text") {
            json ev = {{"room_id", cmd.at("room_id")},
                       {"type", "m.text"},
                       {"content", {{"body", cmd.at("body")}}}};
            if (cmd.contains("reply_to"))
                ev["relation"] = {{"kind", "reply"}, {"target", cmd.at("reply_to")}};
            if (cmd.contains("expires_in")) ev["expires_in"] = cmd.at("expires_in");
            if (cmd.contains("send_at")) ev["send_at"] = cmd.at("send_at");
            share_profile(need_b64(cmd, "room_id", 16), false);
            // A thread message points at the message that started the thread.
            // A message has one relation, so a reply made inside a thread
            // belongs to the thread and names what it quotes in its content.
            if (cmd.contains("thread")) {
                ev["relation"] = {{"kind", "thread"}, {"target", cmd.at("thread")}};
                if (cmd.contains("reply_to")) ev["content"]["reply_to"] = cmd.at("reply_to");
            }
            cmd_send_event(req, ev);
        } else if (name == "typing") {
            // "I am typing in this room." Not stored anywhere; at most one every
            // three seconds per room actually goes out.
            Bytes room_id = need_b64(cmd, "room_id", 16);
            uint64_t now = now_ms();
            if (conn_ == Conn::Live && now - typing_sent_[room_id] >= 3000) {
                typing_sent_[room_id] = now;
                wire::EphemeralT eph;
                eph.room_id = room_id;
                eph.kind = "typing";
                send_frame(make_frame(0, std::move(eph)));
            }
            ok(req);
        } else if (name == "mark_read") {
            // "I have read up to this message." Sent to the room as an encrypted
            // receipt unless the user has turned receipts off.
            Bytes room_id = need_b64(cmd, "room_id", 16);
            Bytes event_id = need_b64(cmd, "event_id", 16);
            auto target = vault_.event(room_id, event_id);
            if (!target || !target->seq) {
                fail(req, "not_found", "unknown message");
                return;
            }
            const Bytes me = to_bytes(vault_.identity().user.pk);
            bool moved = vault_.set_receipt(room_id, me, event_id, *target->seq);
            if (moved && vault_.meta("send_receipts").value_or("1") != "0" &&
                vault_.meta("archived:" + b64(room_id)).value_or("0") != "1") {
                cmd_send_event(next_request_++, {{"room_id", b64(room_id)},
                                                 {"type", "m.receipt"},
                                                 {"expires_in", 7 * 24 * 3600},
                                                 {"relation", {{"kind", "reference"}, {"target", b64(event_id)}}}});
            }
            if (moved)
                if (auto room = vault_.room(room_id)) emit({{"event", "room_updated"}, {"room", room_json(*room)}});
            ok(req, {{"sent", moved}});
        } else if (name == "mark_unread") {
            // "Unread from this message on": this person's own place moves
            // back. Nobody else is told; what they saw as read stays read.
            // Without a message, the newest one from someone else: the chat
            // as a whole is marked, from its list.
            Bytes room_id = need_b64(cmd, "room_id", 16);
            Bytes me = to_bytes(vault_.identity().user.pk);
            std::optional<uint64_t> from;
            if (cmd.contains("event_id")) {
                auto target = vault_.event(room_id, need_b64(cmd, "event_id", 16));
                if (target) from = target->seq;
            } else {
                from = vault_.last_from_others(room_id, me);
            }
            if (!from) {
                fail(req, "not_found", cmd.contains("event_id") ? "unknown message" : "nobody else has written here yet");
                return;
            }
            vault_.rewind_receipt(room_id, me, *from);
            if (auto room = vault_.room(room_id)) emit({{"event", "room_updated"}, {"room", room_json(*room)}});
            ok(req);
        } else if (name == "fetch_receipts") {
            Bytes room_id = need_b64(cmd, "room_id", 16);
            ok(req, {{"room_id", b64(room_id)}, {"receipts", receipts_json(room_id)}});
        } else if (name == "set_read_receipts") {
            // Whether this client tells others what it has read.
            vault_.set_meta("send_receipts", cmd.value("enabled", true) ? "1" : "0");
            ok(req);
        } else if (name == "client_settings") {
            ok(req, {{"client_settings",
                      {{"username", vault_.username()},
                       {"user_id", b64(vault_.identity().user.pk)},
                       {"share_history", vault_.meta("share_history").value_or("1") != "0"},
                       {"send_read_receipts", vault_.meta("send_receipts").value_or("1") != "0"},
                       {"presence", vault_.meta("presence").value_or("auto")}}}});
        } else if (name == "get_recovery_key") {
            // The secret that lets this person set up another device. Whoever
            // has it can become them, so frontends should show it with care.
            ok(req, {{"recovery_key", crypto::encode_recovery_key(vault_.identity().seed())}});
        } else if (name == "request_history") {
            request_history(need_b64(cmd, "room_id", 16), req);
        } else if (name == "set_history_sharing") {
            // Whether this client answers when a newcomer asks for earlier messages.
            vault_.set_meta("share_history", cmd.value("enabled", true) ? "1" : "0");
            ok(req);
        } else if (name == "set_disappearing") {
            // Messages sent in this room from now on disappear after `seconds`
            // (0 turns it off). Travels as an encrypted state event.
            cmd_send_event(req, {{"room_id", cmd.at("room_id")},
                                 {"type", "m.room.retention"},
                                 {"state_key", ""},
                                 {"content", {{"ttl_seconds", cmd.at("seconds")}}}});
        } else if (name == "pin_event") {
            // Pins or unpins a message for everyone in the chat. Travels as
            // an encrypted event, so the server does not know what is pinned.
            Bytes room_id = need_b64(cmd, "room_id", 16);
            auto room = vault_.room(room_id);
            if (!room) {
                fail(req, "not_found", "unknown room");
                return;
            }
            if (room->kind == 0 && !can_moderate(*room, to_bytes(vault_.identity().user.pk))) {
                fail(req, "forbidden", "you do not have permission to pin messages in this channel");
                return;
            }
            cmd_send_event(req, {{"room_id", cmd.at("room_id")},
                                 {"type", "m.room.pin"},
                                 {"state_key", cmd.at("event_id")},
                                 {"content", {{"event_id", cmd.at("event_id")}, {"pinned", cmd.value("pinned", true)}}}});
        } else if (name == "edit_event") {
            cmd_send_event(req, {{"room_id", cmd.at("room_id")},
                                 {"type", "m.edit"},
                                 {"content", {{"body", cmd.at("body")}}},
                                 {"relation", {{"kind", "replace"}, {"target", cmd.at("event_id")}}}});
        } else if (name == "delete_event") {
            // Deleting a file you sent also takes its bytes off the server.
            if (auto target = vault_.event(need_b64(cmd, "room_id", 16), need_b64(cmd, "event_id", 16));
                target && target->type == "m.file" && conn_ == Conn::Live) {
                json content = json::parse(target->content, nullptr, false);
                auto blob_id = content.is_object() ? unb64(content.value("blob_id", "")) : std::nullopt;
                if (blob_id && blob_id->size() == 16) {
                    wire::DeleteBlobT q;
                    q.blob_id = *blob_id;
                    request(std::move(q), [](wire::FrameT&) {});  // refused if it is not ours to remove
                }
            }
            cmd_send_event(req, {{"room_id", cmd.at("room_id")},
                                 {"type", "m.redaction"},
                                 {"relation", {{"kind", "redact"}, {"target", cmd.at("event_id")}}}});
        } else if (name == "send_event") {
            cmd_send_event(req, cmd);
        } else if (name == "add_task") {
            // A row in a task list. Ticking it is a separate event, so anyone
            // who may write in the channel can tick a task someone else added.
            std::string body = cmd.at("body").get<std::string>();
            share_profile(need_b64(cmd, "room_id", 16), false);
            cmd_send_event(req, {{"room_id", cmd.at("room_id")},
                                 {"type", "m.task"},
                                 {"content", {{"body", body}}},
                                 {"fallback_text", "added a task: " + body}});
        } else if (name == "set_task_done") {
            auto task = vault_.event(need_b64(cmd, "room_id", 16), need_b64(cmd, "event_id", 16));
            if (!task || task->type != "m.task" || task->status == "redacted") {
                fail(req, "not_found", "there is no such task");
                return;
            }
            cmd_send_event(req, {{"room_id", cmd.at("room_id")},
                                 {"type", "m.task.done"},
                                 {"content", {{"done", cmd.value("done", true)}}},
                                 {"relation", {{"kind", "task"}, {"target", cmd.at("event_id")}}}});
        } else if (name == "cancel_scheduled") {
            // Takes back a message that is still waiting on the server.
            Bytes room_id = need_b64(cmd, "room_id", 16);
            Bytes event_id = need_b64(cmd, "event_id", 16);
            auto event = vault_.event(room_id, event_id);
            if (!event || event->status != "scheduled") {
                fail(req, "not_found", "that message is not waiting to be sent");
                return;
            }
            wire::CancelScheduledT q;
            q.room_id = room_id;
            q.event_id = event_id;
            request(std::move(q), [this, req, room_id, event_id](wire::FrameT& r) {
                auto* e = r.body.AsError();
                // Gone from the server already and never confirmed here: it
                // had not left this device yet, so it is simply dropped.
                if (e && e->code != err::NotFound) {
                    fail(req, e->code == kDisconnected ? "offline" : "refused", e->message);
                    return;
                }
                vault_.redact_event(room_id, event_id);
                vault_.set_meta("sched:" + b64(event_id), "");
                if (auto now = vault_.event(room_id, event_id))
                    emit({{"event", "event_updated"}, {"room_id", b64(room_id)}, {"data", event_json(*now)}});
                ok(req);
            });
        } else if (name == "send_file") {
            cmd_send_file(req, cmd);
        } else if (name == "download_file") {
            cmd_download_file(req, cmd);
        } else if (name == "fetch_thread") {
            Bytes room_id = need_b64(cmd, "room_id", 16);
            Bytes root_id = need_b64(cmd, "event_id", 16);
            auto root = vault_.event(room_id, root_id);
            if (!root) {
                fail(req, "not_found", "unknown message");
                return;
            }
            json replies = json::array();
            for (const auto& e : vault_.related(room_id, root_id, "thread")) replies.push_back(event_json(e));
            ok(req, {{"room_id", b64(room_id)}, {"root", event_json(*root)}, {"thread", std::move(replies)}});
        } else if (name == "fetch_timeline") {
            Bytes room_id = need_b64(cmd, "room_id", 16);
            json events = json::array();
            // "before" names a message; the page returned ends just ahead of it.
            uint64_t before_seq = 0;
            if (cmd.contains("before")) {
                auto anchor = vault_.event(room_id, need_b64(cmd, "before", 16));
                if (!anchor || !anchor->seq) {
                    fail(req, "not_found", "unknown message");
                    return;
                }
                before_seq = *anchor->seq;
            }
            uint32_t limit = cmd.value("limit", 200u);
            auto page = vault_.timeline(room_id, limit, before_seq);
            for (const auto& e : page)
                if (e.type != "m.history.share" && e.type != "m.receipt" && e.type != "m.profile")
                    events.push_back(event_json(e));  // envelopes and receipts are not messages
            // A full page means there may be older ones; ask again with "before"
            // set to "oldest".
            json data = {{"room_id", b64(room_id)}, {"events", std::move(events)}, {"more", page.size() == limit}};
            for (const auto& e : page)
                if (e.seq) {
                    data["oldest"] = b64(e.event_id);
                    break;
                }
            ok(req, std::move(data));
        } else {
            fail(req, "unknown_command", "unknown command: " + name);
        }
    } catch (const std::exception& e) {
        fail(req, "invalid_argument", e.what());
    }
}

// Parses corded://host[:port]/?fp=<fingerprint>&invite=<code> into the fields
// the connect command takes.
static nlohmann::json parse_invite_link(const std::string& link) {
    const std::string scheme = "corded://";
    if (link.rfind(scheme, 0) != 0) throw std::invalid_argument("not a corded:// link");
    std::string rest = link.substr(scheme.size());
    std::string authority = rest.substr(0, rest.find_first_of("/?"));
    std::string query = rest.find('?') == std::string::npos ? "" : rest.substr(rest.find('?') + 1);
    nlohmann::json out;
    auto colon = authority.rfind(':');
    out["host"] = colon == std::string::npos ? authority : authority.substr(0, colon);
    out["port"] = colon == std::string::npos ? std::string("7443") : authority.substr(colon + 1);
    if (out["host"].get<std::string>().empty()) throw std::invalid_argument("the link has no server address");
    for (size_t pos = 0; pos < query.size();) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        std::string pair = query.substr(pos, end - pos);
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string key = pair.substr(0, eq), value = pair.substr(eq + 1);
            if (key == "fp") out["fingerprint"] = value;
            if (key == "invite") out["invite"] = value;
        }
        pos = end + 1;
    }
    return out;
}

// Finds the server by address, or adds it to this vault, then connects.
void Engine::cmd_connect(uint64_t req, const json& given) {
    // An invite link carries everything: address, the server's key, the code.
    json cmd = given.contains("link") ? parse_invite_link(given.at("link").get<std::string>()) : given;
    std::string host = cmd.at("host").get<std::string>();
    std::string port = cmd.at("port").is_string() ? cmd.at("port").get<std::string>()
                                                  : std::to_string(cmd.at("port").get<int>());
    for (auto& [id, s] : sessions_)
        if (s->host() == host && s->port() == port) {
            s->connect(req, cmd);
            return;
        }
    json list = load_servers();
    int64_t id = 1;
    for (const auto& entry : list) id = std::max(id, entry.value("id", int64_t{0}) + 1);
    list.push_back({{"id", id}, {"host", host}, {"port", port}});
    save_servers(list);
    auto s = std::make_unique<Session>(*this, id, host, port);
    Session* raw = s.get();
    sessions_[id] = std::move(s);
    raw->connect(req, cmd);
}

void Session::cmd_start_chat(uint64_t req, const json& cmd) {
    if (conn_ != Conn::Live && conn_ != Conn::Syncing) {
        fail(req, "not_connected", "not connected to a server");
        return;
    }
    auto names = std::make_shared<std::vector<std::string>>();
    names->push_back(cmd.at("username").get<std::string>());
    lookup_next(req, names, std::make_shared<wire::CreateRoomT>(), "");
}

// Commands for running a community. The server decides whether we may; these
// only translate names into ids and report the answer.
void Session::community_command(uint64_t req, const std::string& name, const json& cmd) {
    if (name == "server_info") {
        ok(req, server_json());
        return;
    }
    if (conn_ != Conn::Live) {
        fail(req, "not_connected", "not connected to a server");
        return;
    }
    auto permission_bits = [](const json& list) {
        uint64_t bits = 0;
        for (const auto& n : list) {
            uint64_t bit = perm::from_name(n.get<std::string>());
            if (!bit) throw std::invalid_argument("unknown permission: " + n.get<std::string>());
            bits |= bit;
        }
        return bits;
    };
    auto role_id = [this](const std::string& role_name) -> uint32_t {
        for (const auto& r : roles_)
            if (r.name == role_name) return r.id;
        throw std::invalid_argument("no role called " + role_name);
    };

    if (name == "member_list") {
        request(wire::GetMembersT{}, [this, req](wire::FrameT& r) {
            auto* list = r.body.AsMemberList();
            if (!list) {
                fail(req, "refused", "could not fetch the member list");
                return;
            }
            json members = json::array();
            for (const auto& m : list->members) {
                if (!m) continue;
                MemberRow row{m->user_id, m->username, m->is_admin, m->is_owner, m->roles, m->nickname, {}};
                // Their own choice of name, if they have told us one.
                json profile = json::parse(vault_.profile(m->user_id).value_or("{}"), nullptr, false);
                if (profile.is_object() && profile.value("display_name", json()).is_string())
                    row.profile_name = profile["display_name"].get<std::string>();
                if (profile.is_object() && profile.value("bot", json()).is_boolean()) row.bot = profile["bot"].get<bool>();
                members.push_back(member_json(row));
            }
            // For those who may lift a ban: who is banned.
            json banned = json::array();
            for (const auto& m : list->banned)
                if (m) banned.push_back({{"user_id", b64(m->user_id)}, {"username", m->username}});
            ok(req, {{"members", std::move(members)}, {"banned", std::move(banned)}});
        });
    } else if (name == "create_role") {
        wire::NewRoleT q;
        q.name = cmd.at("name").get<std::string>();
        q.permissions = permission_bits(cmd.value("permissions", json::array()));
        simple_request(req, std::move(q));
    } else if (name == "edit_role") {
        wire::EditRoleT q;
        q.role_id = role_id(cmd.at("role").get<std::string>());
        q.name = cmd.value("name", std::string{});
        q.permissions = permission_bits(cmd.at("permissions"));
        simple_request(req, std::move(q));
    } else if (name == "delete_role") {
        wire::RemoveRoleT q;
        q.role_id = role_id(cmd.at("role").get<std::string>());
        simple_request(req, std::move(q));
    } else if (name == "grant_role") {
        uint32_t id = role_id(cmd.at("role").get<std::string>());
        bool grant = cmd.value("grant", true);
        admin_action(req, cmd.at("username").get<std::string>(), [this, req, id, grant](const Bytes& user_id) {
            wire::GrantRoleT q;
            q.user_id = user_id;
            q.role_id = id;
            q.assign = grant;
            simple_request(req, std::move(q));
        });
    } else if (name == "create_channel") {
        wire::CreateChannelT q;
        q.name = cmd.at("name").get<std::string>();
        q.channel_type = cmd.value("type", std::string{});
        simple_request(req, std::move(q));
    } else if (name == "rename_channel") {
        wire::UpdateChannelT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        q.name = cmd.at("name").get<std::string>();
        simple_request(req, std::move(q));
    } else if (name == "delete_channel") {
        wire::DeleteChannelT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        simple_request(req, std::move(q));
    } else if (name == "set_channel_access") {
        // An exception for one role in one channel, e.g. deny view_channel to
        // @everyone and allow it to "staff" to make a private channel.
        wire::SetOverrideT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        q.role_id = role_id(cmd.at("role").get<std::string>());
        q.allow = permission_bits(cmd.value("allow", json::array()));
        q.deny = permission_bits(cmd.value("deny", json::array()));
        simple_request(req, std::move(q));
    } else if (name == "list_devices") {
        // The devices signed in as this person on this server.
        wire::ListDevicesT q;
        q.user_id = to_bytes(vault_.identity().user.pk);
        request(std::move(q), [this, req](wire::FrameT& r) {
            auto* list = r.body.AsDeviceList();
            if (!list) {
                fail(req, "refused", "could not list your devices");
                return;
            }
            const auto& mine = vault_.identity().device.pk;
            json devices = json::array();
            for (const auto& d : list->devices) {
                if (!d || d->device_id.size() != 32) continue;
                devices.push_back({{"device_id", b64(d->device_id)},
                                   {"added_at", d->created_at},
                                   {"this_device", std::equal(mine.begin(), mine.end(), d->device_id.begin())}});
            }
            ok(req, {{"devices", std::move(devices)}});
        });
    } else if (name == "remove_device") {
        // Signs another of this person's devices out for good.
        wire::RemoveDeviceT q;
        q.device_id = need_b64(cmd, "device_id", 32);
        simple_request(req, std::move(q));
    } else if (name == "set_channel_archived") {
        // An archived channel stays readable; the server refuses anything new in it.
        wire::SetChannelArchivedT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        q.archived = cmd.value("archived", true);
        simple_request(req, std::move(q));
    } else if (name == "purge_files") {
        // Deletes the server's stored files older than `older_than_days` (0: all).
        wire::PurgeFilesT q;
        q.older_than_days = cmd.value("older_than_days", uint32_t{0});
        request(std::move(q), [this, req](wire::FrameT& r) {
            if (auto* done = r.body.AsPurged()) {
                ok(req, {{"files", done->files}, {"bytes", done->bytes}});
                return;
            }
            auto* e = r.body.AsError();
            fail(req, "refused", e ? e->message : "the server refused");
        });
    } else if (name == "edit_section") {
        // {name} makes one; {section_id, name, position?} renames or moves it;
        // {section_id, remove: true} takes it away.
        wire::EditSectionT q;
        q.section_id = cmd.value("section_id", uint32_t{0});
        q.name = cmd.value("name", std::string{});
        q.remove = cmd.value("remove", false);
        q.position = cmd.value("position", int32_t{0});
        if (q.section_id != 0 && !q.remove) {
            // Left out, a section keeps its name or its place.
            for (const auto& s : sections_)
                if (s.id == q.section_id) {
                    if (!cmd.contains("name")) q.name = s.name;
                    if (!cmd.contains("position")) q.position = s.position;
                }
        }
        simple_request(req, std::move(q));
    } else if (name == "set_channel_section") {
        wire::SetChannelSectionT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        q.section_id = cmd.value("section_id", uint32_t{0});
        simple_request(req, std::move(q));
    } else if (name == "set_channel_featured") {
        // Pins a channel to the top of everyone's list, or lets it go.
        wire::SetChannelFeaturedT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        q.featured = cmd.value("featured", true);
        simple_request(req, std::move(q));
    } else if (name == "set_channel_nsfw") {
        // Marks a channel so that clients warn before showing it.
        wire::SetChannelNsfwT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        q.nsfw = cmd.value("nsfw", true);
        simple_request(req, std::move(q));
    } else if (name == "channel_access") {
        // The exceptions a channel has, role by role.
        wire::GetOverridesT q;
        q.room_id = need_b64(cmd, "room_id", 16);
        Bytes room_id = q.room_id;
        request(std::move(q), [this, req, room_id](wire::FrameT& r) {
            auto* list = r.body.AsOverrides();
            if (!list) {
                auto* e = r.body.AsError();
                // A server from before this existed answers "unexpected frame".
                fail(req, "refused",
                     e && e->message != "unexpected frame" ? e->message
                                                           : "this server is too old to list channel access; update it");
                return;
            }
            json access = json::array();
            for (const auto& entry : list->entries) {
                if (!entry) continue;
                std::string role_name;
                for (const auto& role : roles_)
                    if (role.id == entry->role_id) role_name = role.name;
                if (role_name.empty()) continue;
                access.push_back({{"role", role_name},
                                  {"allow", perm::to_names(entry->allow)},
                                  {"deny", perm::to_names(entry->deny)}});
            }
            ok(req, {{"room_id", b64(room_id)}, {"access", std::move(access)}});
        });
    } else if (name == "create_invite") {
        wire::NewInviteT q;
        q.max_uses = cmd.value("max_uses", 0u);
        q.expires_in_s = static_cast<uint64_t>(cmd.value("expires_in_hours", 0.0) * 3600.0);
        request(std::move(q), [this, req](wire::FrameT& r) {
            auto* invite = r.body.AsInvite();
            if (!invite) {
                auto* e = r.body.AsError();
                fail(req, "refused", e ? e->message : "could not create an invite");
                return;
            }
            // The link pins the server's key, so whoever uses it cannot be
            // pointed at an impostor.
            ok(req, {{"code", invite->code},
                     {"max_uses", invite->max_uses},
                     {"expires_at", invite->expires_at},
                     {"link", "corded://" + host_ + ":" + port_ + "/?fp=" + server_fingerprint_ +
                                  "&invite=" + invite->code}});
        });
    } else if (name == "get_settings") {
        request(wire::GetSettingsT{}, [this, req](wire::FrameT& r) {
            auto* list = r.body.AsSettings();
            if (!list) {
                auto* e = r.body.AsError();
                fail(req, "refused", e ? e->message : "could not read the settings");
                return;
            }
            json settings = json::array();
            for (const auto& s : list->entries)
                if (s)
                    settings.push_back({{"key", s->key},
                                        {"value", s->value},
                                        {"owner_only", s->owner_only},
                                        {"needs_restart", s->needs_restart},
                                        {"description", s->description}});
            ok(req, {{"settings", std::move(settings)}});
        });
    } else if (name == "set_setting") {
        wire::SetSettingT q;
        q.key = cmd.at("key").get<std::string>();
        q.value = cmd.at("value").is_string() ? cmd.at("value").get<std::string>() : cmd.at("value").dump();
        simple_request(req, std::move(q));
    } else if (name == "update_server") {
        // The owner asks the server to install the newest signed release.
        // How it went arrives afterwards as a server notice.
        simple_request(req, wire::UpdateServerT{});
    } else if (name == "restart_server") {
        simple_request(req, wire::RestartT{});
    } else if (name == "server_status") {
        request(wire::GetStatusT{}, [this, req](wire::FrameT& r) {
            auto* st = r.body.AsStatus();
            if (!st) {
                auto* e = r.body.AsError();
                fail(req, "refused", e ? e->message : "could not read the status");
                return;
            }
            ok(req, {{"status",
                      {{"version", st->version},
                       {"started_at", st->started_at},
                       {"members", st->members},
                       {"online", st->online},
                       {"stored_bytes", st->stored_bytes},
                       {"file_bytes", st->file_bytes},
                       {"file_count", st->file_count},
                       {"scheduled", st->scheduled},
                       {"last_housekeeping", st->last_housekeeping},
                       {"scope", st->scope}}}});
        });
    } else if (name == "revoke_invite") {
        wire::RevokeInviteT q;
        q.code = cmd.at("code").get<std::string>();
        simple_request(req, std::move(q));
    } else if (name == "kick") {
        admin_action(req, cmd.at("username").get<std::string>(), [this, req](const Bytes& user_id) {
            wire::KickT q;
            q.user_id = user_id;
            simple_request(req, std::move(q));
        });
    } else if (name == "set_nickname") {
        // Your own display name, or with "username" someone else's.
        std::string nickname = cmd.value("nickname", std::string{});
        if (!cmd.contains("username")) {
            wire::SetNicknameT q;
            q.nickname = nickname;
            simple_request(req, std::move(q));
        } else {
            admin_action(req, cmd.at("username").get<std::string>(), [this, req, nickname](const Bytes& user_id) {
                wire::SetNicknameT q;
                q.user_id = user_id;
                q.nickname = nickname;
                simple_request(req, std::move(q));
            });
        }
    } else if (name == "remove_account") {
        admin_action(req, cmd.at("username").get<std::string>(), [this, req](const Bytes& user_id) {
            wire::RemoveAccountT q;
            q.user_id = user_id;
            simple_request(req, std::move(q));
        });
    } else if (name == "ban_user") {
        bool banned = cmd.value("banned", true);
        admin_action(req, cmd.at("username").get<std::string>(), [this, req, banned](const Bytes& user_id) {
            wire::BanUserT q;
            q.user_id = user_id;
            q.banned = banned;
            simple_request(req, std::move(q));
        });
    }
}

// Shared start of the commands that act on a person: find them by username.
// The server decides whether we are allowed; this only resolves the name.
void Session::admin_action(uint64_t req, const std::string& username,
                          std::function<void(const Bytes& user_id)> then) {
    if (conn_ != Conn::Live) {
        fail(req, "not_connected", "not connected to a server");
        return;
    }
    wire::LookupUserT q;
    q.username = username;
    request(std::move(q), [this, req, username, then = std::move(then)](wire::FrameT& f) {
        if (f.body.type != wire::FrameBody_UserInfo) {
            fail(req, "not_found", "no user called " + username);
            return;
        }
        then(f.body.AsUserInfo()->user_id);
    });
}

// A room with any number of people. Two-person rooms are unique per pair;
// larger ones are always new.
void Session::cmd_create_room(uint64_t req, const json& cmd) {
    if (conn_ != Conn::Live && conn_ != Conn::Syncing) {
        fail(req, "not_connected", "not connected to a server");
        return;
    }
    auto names = std::make_shared<std::vector<std::string>>();
    for (const auto& n : cmd.at("usernames")) names->push_back(n.get<std::string>());
    if (names->empty()) {
        fail(req, "invalid_argument", "name at least one other person");
        return;
    }
    lookup_next(req, names, std::make_shared<wire::CreateRoomT>(), cmd.value("name", std::string{}));
}

// Resolves usernames to user ids one at a time, then creates the room.
void Session::lookup_next(uint64_t req, std::shared_ptr<std::vector<std::string>> names,
                         std::shared_ptr<wire::CreateRoomT> create, std::string room_name) {
    if (create->members.size() == names->size()) {
        create_room(req, std::move(*create), std::move(room_name));
        return;
    }
    std::string username = (*names)[create->members.size()];
    wire::LookupUserT q;
    q.username = username;
    request(std::move(q), [this, req, names, create, room_name, username](wire::FrameT& f) {
        if (f.body.type != wire::FrameBody_UserInfo) {
            auto* e = f.body.AsError();
            fail(req, "not_found", e && e->code != kDisconnected ? "no user called " + username
                                                                 : "connection lost");
            return;
        }
        auto member = std::make_unique<wire::MemberT>();
        member->user_id = f.body.AsUserInfo()->user_id;
        create->members.push_back(std::move(member));
        lookup_next(req, names, create, room_name);
    });
}

void Session::create_room(uint64_t req, wire::CreateRoomT create, std::string room_name) {
    request(std::move(create), [this, req, room_name](wire::FrameT& r) {
        if (r.body.type != wire::FrameBody_RoomInfo) {
            auto* e = r.body.AsError();
            fail(req, "server_error", e ? e->message : "could not create the room");
            return;
        }
        store_room(*r.body.AsRoomInfo());
        auto room = vault_.room(r.body.AsRoomInfo()->room_id);
        if (!room) {
            fail(req, "internal", "room was not stored");
            return;
        }
        // The name travels as an encrypted state event, so the server never sees it.
        if (!room_name.empty())
            cmd_send_event(next_request_++, {{"room_id", b64(room->room_id)},
                                             {"type", "m.room.name"},
                                             {"state_key", ""},
                                             {"content", {{"name", room_name}}}});
        room = vault_.room(room->room_id);
        ok(req, {{"room", room_json(*room)}});
    });
}

// Whether a member's roles (as the server reported them) carry a permission.
bool Session::has_permission(const RoomRow& room, ByteView user_id, uint64_t permission) {
    for (const auto& m : room.members) {
        if (m.user_id.size() != user_id.size() || !std::equal(m.user_id.begin(), m.user_id.end(), user_id.begin()))
            continue;
        if (m.is_owner || m.is_admin) return true;
        for (const auto& r : roles_) {
            bool held = r.is_everyone || std::find(m.roles.begin(), m.roles.end(), r.id) != m.roles.end();
            if (held && (r.permissions & (permission | perm::Administrator))) return true;
        }
    }
    return false;
}

// Whether this person may delete other people's messages in a channel: the
// owner, an administrator, or anyone with a role that grants manage_messages.
// Judged from the roles the server reported; per-channel exceptions are not
// taken into account here.
bool Session::can_moderate(const RoomRow& room, ByteView user_id) {
    if (room.kind != 0) return false;
    for (const auto& m : room.members) {
        if (m.user_id.size() != user_id.size() || !std::equal(m.user_id.begin(), m.user_id.end(), user_id.begin()))
            continue;
        if (m.is_owner || m.is_admin) return true;
        for (uint32_t id : m.roles)
            for (const auto& r : roles_)
                if (r.id == id && (r.permissions & perm::ManageMessages)) return true;
    }
    return false;
}

// Edits and deletions change another event. An edit is honoured only from the
// person who sent the original. A deletion is honoured from them, or in a
// channel from a moderator. Deletion is a request: this client erases its
// copy, but cannot make anyone else forget.
void Session::apply_relation(const EventRow& e) {
    if (e.rel_target.empty()) return;
    if (e.rel_kind == "thread") {
        // The thread's first message gains a reply; tell frontends its count changed.
        if (auto root = vault_.event(e.room_id, e.rel_target))
            emit({{"event", "event_updated"}, {"room_id", b64(e.room_id)}, {"data", event_json(*root)}});
        return;
    }
    if (e.rel_kind == "task") {
        // Someone ticked a task, or took the tick back.
        if (auto task = vault_.event(e.room_id, e.rel_target))
            emit({{"event", "event_updated"}, {"room_id", b64(e.room_id)}, {"data", event_json(*task)}});
        return;
    }
    if (e.rel_kind != "replace" && e.rel_kind != "redact") return;
    auto target = vault_.event(e.room_id, e.rel_target);
    if (!target) return;
    if (target->sender_user != e.sender_user) {
        auto room = vault_.room(e.room_id);
        if (e.rel_kind != "redact" || !room || !can_moderate(*room, e.sender_user)) return;
    }
    if (target->status == "redacted" || target->status == "undecryptable") return;
    if (e.rel_kind == "replace") {
        if (e.type != "m.edit" || (target->type != "m.text" && target->type != "m.task")) return;
        vault_.set_edited_content(e.room_id, e.rel_target, e.content);
    } else {
        if (e.type != "m.redaction") return;
        vault_.redact_event(e.room_id, e.rel_target);
    }
    if (auto updated = vault_.event(e.room_id, e.rel_target))
        emit({{"event", "event_updated"}, {"room_id", b64(e.room_id)}, {"data", event_json(*updated)}});
}

// Room state carried by events: the room's name, and how long messages last.
// The messages pinned in a room, oldest pin first.
Session::json Session::pins(ByteView room_id) {
    json list = json::parse(vault_.meta("pins:" + b64(room_id)).value_or("[]"), nullptr, false);
    return list.is_array() ? list : json::array();
}

void Session::apply_state(const EventRow& e) {
    if (e.type == "m.room.pin") {
        auto room = vault_.room(e.room_id);
        if (!room) return;
        // In a channel, pinning is for those who may manage messages; in a
        // direct message or group, anyone in it may.
        if (room->kind == 0 && !can_moderate(*room, e.sender_user)) return;
        json content = json::parse(e.content, nullptr, false);
        if (!content.is_object() || !content.value("event_id", json()).is_string()) return;
        std::string target = content["event_id"].get<std::string>();
        bool pinned = content.value("pinned", true);
        json list = pins(e.room_id), kept = json::array();
        for (const auto& id : list)
            if (id != target) kept.push_back(id);
        if (pinned && kept.size() < 50) kept.push_back(target);
        vault_.set_meta("pins:" + b64(e.room_id), kept.dump());
        emit({{"event", "room_updated"}, {"room", room_json(*room)}});
        return;
    }
    if (e.type == "m.room.retention") {
        auto room = vault_.room(e.room_id);
        if (!room) return;
        // In a channel only people who manage channels decide this; in a
        // direct message or private group, anyone in it may.
        if (room->kind == 0 && !has_permission(*room, e.sender_user, perm::ManageChannels)) return;
        json content = json::parse(e.content, nullptr, false);
        if (!content.is_object() || !content.value("ttl_seconds", json()).is_number()) return;
        double ttl = content["ttl_seconds"].get<double>();
        if (ttl < 0 || ttl > 366.0 * 24 * 3600) return;
        vault_.set_room_ttl(e.room_id, static_cast<uint64_t>(ttl));
        if (auto updated = vault_.room(e.room_id))
            emit({{"event", "room_updated"}, {"room", room_json(*updated)}});
        return;
    }
    if (e.type != "m.room.name") return;
    json content = json::parse(e.content, nullptr, false);
    if (!content.is_object() || !content.value("name", json()).is_string()) return;
    std::string name = content["name"].get<std::string>();
    if (name.size() > 80) name.resize(80);
    vault_.set_room_name(e.room_id, name);
    if (auto room = vault_.room(e.room_id))
        emit({{"event", "room_updated"}, {"room", room_json(*room)}});
}

void Session::cmd_send_event(uint64_t req, const json& cmd) {
    EventRow e;
    e.room_id = need_b64(cmd, "room_id", 16);
    if (!vault_.room(e.room_id)) {
        fail(req, "not_found", "unknown room");
        return;
    }
    e.event_id = new_event_id();
    e.type = cmd.at("type").get<std::string>();
    e.type_version = cmd.value("type_version", uint16_t{1});
    e.sender_user = to_bytes(vault_.identity().user.pk);
    e.sender_device = to_bytes(vault_.identity().device.pk);
    e.origin_ts = now_ms();
    e.content = cmd.value("content", json::object()).dump();
    e.fallback_text = cmd.value("fallback_text", std::string{});
    e.state_key = cmd.value("state_key", std::string{});
    e.status = "pending";
    // For later: the server keeps the encrypted event aside until this time.
    uint64_t send_at = cmd.value("send_at", uint64_t{0});
    if (send_at > now_ms() + 2000) {
        e.status = "scheduled";
        e.origin_ts = send_at;  // the time people will see beside it
        vault_.set_meta("sched:" + b64(e.event_id), std::to_string(send_at));
    }
    if (cmd.contains("relation")) {
        const json& rel = cmd.at("relation");
        e.rel_kind = rel.at("kind").get<std::string>();
        e.rel_target = need_b64(rel, "target", 16);
        e.rel_key = rel.value("key", std::string{});
    }
    if (e.content.size() > 256 * 1024) {
        fail(req, "invalid_argument", "event content is too large");
        return;
    }
    // Disappearing: this message's own timer if given, else the room's. Room
    // settings and membership notices are not themselves made to disappear.
    {
        double own = cmd.value("expires_in", 0.0);
        uint64_t room_ttl = vault_.room(e.room_id)->ttl_s;
        bool housekeeping = e.type.rfind("m.room.", 0) == 0;
        if (own > 0) e.expires_at = e.origin_ts + static_cast<uint64_t>(own * 1000.0);
        else if (room_ttl > 0 && !housekeeping) e.expires_at = e.origin_ts + room_ttl * 1000;
    }
    {
        db::Transaction tx(vault_.db());
        vault_.insert_event(e);
        vault_.outbox_push(e.room_id, e.event_id);
        tx.commit();
    }
    if (e.type != "m.receipt" && e.type != "m.profile")
        emit({{"event", "event_received"}, {"room_id", b64(e.room_id)}, {"data", event_json(e)}});
    apply_state(e);
    apply_relation(e);
    ok(req, {{"event_id", b64(e.event_id)}});
    pump_outbox();
}

// ---------------------------------------------------------------- files
//
// A file is encrypted on the sender's device under a key made for it alone
// and announced by an m.file message that carries the key. It is encrypted in
// pieces, each sealed separately, so a file of any size passes through without
// ever being held whole in memory:
//
//   piece i = seal(key, nonce with i mixed in, bytes, blob id | i | "last?")
//
// The piece's number and whether it is the last are part of what is sealed, so
// pieces cannot be reordered, dropped from the middle or cut off at the end
// without the check failing. A sealed piece is exactly as large as one upload
// frame, so the server's pieces and the file's pieces line up.
//
// On the receiving device the file is kept only in this encrypted form. It is
// opened into a "view" folder when someone looks at it, and that folder is
// emptied when the vault is locked or unlocked.

namespace {

constexpr size_t kSealBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr size_t kFilePiece = kBlobChunkBytes - kSealBytes;  // plain bytes per piece

Bytes piece_nonce(ByteView base, uint64_t index) {
    Bytes n = to_bytes(base);
    for (int b = 0; b < 8; ++b) n[16 + static_cast<size_t>(b)] ^= static_cast<uint8_t>(index >> (8 * b));
    return n;
}

Bytes piece_label(ByteView blob_id, uint64_t index, bool last) {
    Bytes ad = to_bytes(blob_id);
    for (int b = 0; b < 8; ++b) ad.push_back(static_cast<uint8_t>(index >> (8 * b)));
    ad.push_back(last ? 1 : 0);
    return ad;
}

uint64_t pieces_in(uint64_t size) { return (size + kFilePiece - 1) / kFilePiece; }
uint64_t sealed_size(uint64_t size) { return size + pieces_in(size) * kSealBytes; }

Key32 as_key(const Bytes& bytes) {
    Key32 key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

}  // namespace

// send_file: {room_id, path, name?, mime?, caption?, thumbnail?, width?, height?,
// reply_to?, thread?}.
void Session::cmd_send_file(uint64_t req, const json& cmd) {
    Bytes room_id = need_b64(cmd, "room_id", 16);
    if (!vault_.room(room_id)) {
        fail(req, "not_found", "unknown room");
        return;
    }
    if (conn_ != Conn::Live) {
        fail(req, "offline", "a file can only be sent while connected");
        return;
    }
    std::string path = cmd.at("path").get<std::string>();
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(path, ec);
    auto up = std::make_shared<Upload>();
    up->in.open(path, std::ios::binary);
    if (ec || size == 0 || !up->in) {
        fail(req, "invalid_argument", "that file could not be read, or is empty");
        return;
    }
    up->size = size;
    up->blob_id = random_bytes(16);
    up->key = random_bytes(32);
    up->nonce = random_bytes(24);

    std::string name = cmd.value("name", std::filesystem::path(path).filename().string());
    json content = {{"body", cmd.value("caption", std::string{})},
                    {"name", name},
                    {"mime", cmd.value("mime", std::string{"application/octet-stream"})},
                    {"size", size},
                    {"blob_id", b64(up->blob_id)},
                    {"key", b64(up->key)},
                    {"nonce", b64(up->nonce)},
                    // Sealed in pieces of this many bytes; a client that only
                    // knows the older whole-file form will say it cannot open it.
                    {"piece", kFilePiece}};
    for (const char* extra : {"thumbnail", "width", "height"})
        if (cmd.contains(extra)) content[extra] = cmd.at(extra);
    up->event = {{"room_id", cmd.at("room_id")},
                 {"type", "m.file"},
                 {"content", std::move(content)},
                 {"fallback_text", "sent a file: " + name}};
    if (cmd.contains("reply_to")) up->event["relation"] = {{"kind", "reply"}, {"target", cmd.at("reply_to")}};
    if (cmd.contains("thread")) {
        up->event["relation"] = {{"kind", "thread"}, {"target", cmd.at("thread")}};
        if (cmd.contains("reply_to")) up->event["content"]["reply_to"] = cmd.at("reply_to");
    }
    upload_next(req, std::move(up));
}

void Session::upload_next(uint64_t req, std::shared_ptr<Upload> up) {
    uint64_t done = up->index * kFilePiece;
    if (done >= up->size) {
        sodium_memzero(up->key.data(), up->key.size());
        share_profile(need_b64(up->event, "room_id", 16), false);
        cmd_send_event(req, up->event);
        return;
    }
    Bytes plain(static_cast<size_t>(std::min<uint64_t>(kFilePiece, up->size - done)));
    up->in.read(reinterpret_cast<char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
    if (static_cast<size_t>(up->in.gcount()) != plain.size()) {
        fail(req, "invalid_argument", "that file changed or could not be read while it was being sent");
        return;
    }
    bool last = done + plain.size() >= up->size;
    wire::PutBlobT q;
    q.blob_id = up->blob_id;
    q.offset = up->index * kBlobChunkBytes;
    q.total = sealed_size(up->size);
    q.data = crypto::aead_encrypt(as_key(up->key), piece_nonce(up->nonce, up->index), plain,
                                  piece_label(up->blob_id, up->index, last));
    sodium_memzero(plain.data(), plain.size());
    uint64_t after = done + plain.size();
    request(std::move(q), [this, req, up, after](wire::FrameT& r) {
        if (r.body.type != wire::FrameBody_Ok) {
            auto* e = r.body.AsError();
            fail(req, "refused", e ? e->message : "the server refused the file");
            return;
        }
        ++up->index;
        emit({{"event", "file_progress"}, {"request_id", req}, {"done", after}, {"total", up->size}});
        upload_next(req, up);
    });
}

// download_file: {room_id, event_id, dir?}. Makes sure the file's encrypted
// form is on this device (fetching it if it is not), then opens it into `dir`
// (by default the vault's "view" folder) and answers with that path. A file
// someone asks to keep goes wherever `dir` says and stays there; what is in
// the view folder is gone at the next lock or unlock.
void Session::cmd_download_file(uint64_t req, const json& cmd) {
    Bytes room_id = need_b64(cmd, "room_id", 16);
    Bytes event_id = need_b64(cmd, "event_id", 16);
    auto e = vault_.event(room_id, event_id);
    json content = e && e->type == "m.file" && e->status != "redacted" ? json::parse(e->content, nullptr, false)
                                                                     : json();
    auto blob_id = content.is_object() ? unb64(content.value("blob_id", "")) : std::nullopt;
    auto key = content.is_object() ? unb64(content.value("key", "")) : std::nullopt;
    auto nonce = content.is_object() ? unb64(content.value("nonce", "")) : std::nullopt;
    if (!blob_id || blob_id->size() != 16 || !key || key->size() != 32 || !nonce || nonce->size() != 24) {
        fail(req, "not_found", "that message has no file");
        return;
    }
    auto down = std::make_shared<Download>();
    down->blob_id = *blob_id;
    down->key = *key;
    down->nonce = *nonce;
    down->size = content.value("size", uint64_t{0});
    down->piece = content.value("piece", uint64_t{0});
    down->name = content.value("name", std::string{"file"});
    down->mime = content.value("mime", std::string{});
    if (down->piece != 0 && down->piece != kFilePiece) {
        fail(req, "bad_file", "this file was sent in a form this version cannot open; update Corded");
        return;
    }
    // The older form is one sealed piece and has to fit in memory to be checked.
    if (down->piece == 0 && down->size > kMaxFileBytes) {
        fail(req, "invalid_argument", "that file is too large to fetch");
        return;
    }
    down->sealed = down->piece == 0 ? down->size + kSealBytes : sealed_size(down->size);
    std::string stem = hex_of(event_id).substr(12);
    std::string kept_dir = engine_.config_.vault_dir + "/files";
    down->kept = kept_dir + "/" + stem + ".enc";
    std::string dir = cmd.value("dir", engine_.config_.vault_dir + "/view");
    down->path = dir + "/" + stem + "-" + safe_file_name(down->name);
    std::error_code ec;
    std::filesystem::create_directories(kept_dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (std::filesystem::file_size(down->kept, ec) == down->sealed && !ec) {
        open_file(req, down, true);
        return;
    }
    if (conn_ != Conn::Live) {
        fail(req, "offline", "a file can only be fetched while connected");
        return;
    }
    down->out.open(down->kept + ".part", std::ios::binary | std::ios::trunc);
    if (!down->out) {
        fail(req, "io_error", "the file could not be saved on this device");
        return;
    }
    download_next(req, std::move(down));
}

void Session::download_next(uint64_t req, std::shared_ptr<Download> down) {
    wire::GetBlobT q;
    q.blob_id = down->blob_id;
    q.offset = down->received;
    request(std::move(q), [this, req, down](wire::FrameT& r) {
        auto* piece = r.body.AsBlob();
        // What the server says about the size is checked against what the
        // sender said inside the encrypted message.
        if (!piece || piece->data.empty() || piece->offset != down->received || piece->total != down->sealed ||
            down->received + piece->data.size() > down->sealed) {
            auto* e = r.body.AsError();
            down->out.close();
            std::error_code ec;
            std::filesystem::remove(down->kept + ".part", ec);
            fail(req, e && e->code == kDisconnected ? "offline" : e ? "not_found" : "bad_file",
                 e ? e->message : "the server sent something that is not this file");
            return;
        }
        down->out.write(reinterpret_cast<const char*>(piece->data.data()),
                        static_cast<std::streamsize>(piece->data.size()));
        down->received += piece->data.size();
        emit({{"event", "file_progress"}, {"request_id", req}, {"done", down->received}, {"total", down->sealed}});
        if (down->received < down->sealed) {
            download_next(req, down);
            return;
        }
        down->out.close();
        std::error_code ec;
        std::filesystem::rename(down->kept + ".part", down->kept, ec);
        if (!down->out || ec) {
            fail(req, "io_error", "the file could not be saved on this device");
            return;
        }
        open_file(req, down, false);
    });
}

// Checks the kept, encrypted file piece by piece and writes it out readable.
// Anything that does not pass is thrown away, kept copy included, so the next
// try fetches it afresh.
void Session::open_file(uint64_t req, std::shared_ptr<Download> down, bool cached) {
    auto spoiled = [&](const char* why) {
        std::error_code ec;
        std::filesystem::remove(down->kept, ec);
        std::filesystem::remove(down->path + ".part", ec);
        fail(req, "bad_file", why);
    };
    std::ifstream in(down->kept, std::ios::binary);
    std::ofstream out(down->path + ".part", std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        fail(req, "io_error", "the file could not be opened on this device");
        return;
    }
    Key32 key = as_key(down->key);
    uint64_t written = 0;
    if (down->piece == 0) {
        // The older form: the whole file sealed as one.
        Bytes sealed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto plain = crypto::aead_decrypt(key, down->nonce, sealed, down->blob_id);
        if (!plain || plain->size() != down->size) {
            sodium_memzero(key.data(), key.size());
            spoiled("the file did not pass its check; it was damaged or swapped");
            return;
        }
        out.write(reinterpret_cast<const char*>(plain->data()), static_cast<std::streamsize>(plain->size()));
        written = plain->size();
        sodium_memzero(plain->data(), plain->size());
    } else {
        Bytes sealed(kBlobChunkBytes);
        for (uint64_t index = 0; written < down->size; ++index) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(kFilePiece, down->size - written)) + kSealBytes;
            in.read(reinterpret_cast<char*>(sealed.data()), static_cast<std::streamsize>(want));
            bool last = written + (want - kSealBytes) >= down->size;
            auto plain = static_cast<size_t>(in.gcount()) == want
                             ? crypto::aead_decrypt(key, piece_nonce(down->nonce, index), ByteView(sealed.data(), want),
                                                    piece_label(down->blob_id, index, last))
                             : std::nullopt;
            if (!plain) {
                sodium_memzero(key.data(), key.size());
                out.close();
                spoiled("the file did not pass its check; it was damaged, cut short or swapped");
                return;
            }
            out.write(reinterpret_cast<const char*>(plain->data()), static_cast<std::streamsize>(plain->size()));
            written += plain->size();
            sodium_memzero(plain->data(), plain->size());
        }
    }
    sodium_memzero(key.data(), key.size());
    out.close();
    std::error_code ec;
    std::filesystem::rename(down->path + ".part", down->path, ec);
    if (!out || ec) {
        fail(req, "io_error", "the file could not be opened on this device");
        return;
    }
    ok(req, {{"path", down->path}, {"name", down->name}, {"mime", down->mime}, {"cached", cached}});
}

// ---------------------------------------------------------------- network

void Session::start_connect() {
    if (!want_connection_ || conn_ != Conn::Disconnected) return;
    uint64_t gen = ++conn_gen_;
    set_conn(Conn::Connecting);
    auto s = std::make_shared<tls::Stream>(io_, tls_ctx_);
    stream_ = s;
    resolver_.async_resolve(host_, port_, [this, gen, s](asio::error_code ec, tcp::resolver::results_type r) {
        if (gen != conn_gen_) return;
        if (ec) {
            drop_connection("cannot resolve server: " + ec.message());
            return;
        }
        asio::async_connect(s->lowest_layer(), r, [this, gen, s](asio::error_code ec2, const tcp::endpoint&) {
            if (gen != conn_gen_) return;
            if (ec2) {
                drop_connection("cannot connect: " + ec2.message());
                return;
            }
            asio::error_code ignored;
            s->lowest_layer().set_option(tcp::no_delay(true), ignored);
            s->async_handshake(asio::ssl::stream_base::client, [this, gen, s](asio::error_code ec3) {
                if (gen != conn_gen_) return;
                if (ec3) {
                    drop_connection("secure connection failed: " + ec3.message());
                    return;
                }
                if (!check_server_identity()) return;
                set_conn(Conn::Authenticating);
                read_header(gen);
            });
        });
    });
}

// Trust on first use: the first fingerprint seen for a server is remembered,
// and a different one later is refused outright.
bool Session::check_server_identity() {
    std::string seen;
    try {
        seen = b64(tls::peer_fingerprint(*stream_));
        tls_exporter_ = tls::exporter(*stream_);
    } catch (const std::exception& e) {
        drop_connection(std::string("secure connection failed: ") + e.what());
        return false;
    }
    std::string pinned = meta("server_fp").value_or("");
    if (pinned.empty()) {
        set_meta("server_fp", seen);
        emit({{"event", "server_pinned"}, {"server", host_ + ":" + port_}, {"fingerprint", seen}});
    } else if (pinned != seen) {
        want_connection_ = false;
        drop_connection("the server's identity does not match the one saved for it (expected " +
                        pinned + ", got " + seen + "); refusing to connect");
        return false;
    }
    server_fingerprint_ = seen;
    return true;
}

void Session::drop_connection(const std::string& reason) {
    ++conn_gen_;  // orphans every callback of the old connection
    Conn previous = conn_;
    conn_ = Conn::Disconnected;  // before the handlers below run, so they cannot send
    asio::error_code ec;
    resolver_.cancel();
    if (stream_) stream_->lowest_layer().close(ec);
    stream_.reset();  // callbacks still running hold their own reference
    reconnect_timer_.cancel();
    out_.clear();
    writing_ = false;
    sending_ = false;
    bundle_requested_.clear();
    devices_.clear();
    devices_requested_.clear();
    unreachable_.clear();
    // Tell everyone waiting on an answer that it is not coming.
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& [id, handler] : pending) {
        wire::FrameT f = error_frame(id, kDisconnected, "connection lost");
        handler(f);
    }
    if (previous != Conn::Disconnected) set_conn(Conn::Disconnected, reason);
    if (want_connection_) schedule_reconnect();
}

void Session::schedule_reconnect() {
    int delay = backoff_s_;
    backoff_s_ = std::min(backoff_s_ * 2, 30);
    reconnect_timer_.expires_after(std::chrono::seconds(delay));
    reconnect_timer_.async_wait([this](asio::error_code ec) {
        if (!ec) start_connect();
    });
}

void Session::read_header(uint64_t gen) {
    auto s = stream_;
    asio::async_read(*s, asio::buffer(hdr_), [this, gen, s](asio::error_code ec, size_t) {
        if (gen != conn_gen_) return;
        if (ec) {
            drop_connection("connection closed");
            return;
        }
        uint32_t n = decode_length(hdr_.data());
        if (n == 0 || n > kMaxFrameBytes) {
            drop_connection("server sent a bad frame");
            return;
        }
        read_body(gen, n);
    });
}

void Session::read_body(uint64_t gen, uint32_t n) {
    body_.resize(n);
    auto s = stream_;
    asio::async_read(*s, asio::buffer(body_), [this, gen, s](asio::error_code ec, size_t) {
        if (gen != conn_gen_) return;
        if (ec) {
            drop_connection("connection closed");
            return;
        }
        auto frame = decode_frame(body_);
        if (!frame) {
            drop_connection("server sent a malformed frame");
            return;
        }
        try {
            on_frame(*frame);
        } catch (const std::exception& e) {
            emit({{"event", "warning"}, {"message", std::string("error handling a frame: ") + e.what()}});
        }
        if (gen == conn_gen_) read_header(gen);
    });
}

void Session::send_frame(const wire::FrameT& f) {
    if (conn_ == Conn::Disconnected || conn_ == Conn::Connecting) return;
    out_.push_back(encode_frame(f));
    if (!writing_) write_next(conn_gen_);
}

void Session::write_next(uint64_t gen) {
    if (out_.empty()) {
        writing_ = false;
        return;
    }
    writing_ = true;
    auto buf = out_.front();
    auto s = stream_;
    asio::async_write(*s, asio::buffer(*buf), [this, gen, buf, s](asio::error_code ec, size_t) {
        if (gen != conn_gen_) return;
        if (ec) {
            drop_connection("connection closed");
            return;
        }
        out_.pop_front();
        write_next(gen);
    });
}

// ---------------------------------------------------------------- protocol

void Session::on_frame(wire::FrameT& f) {
    if (f.request_id != 0) {
        auto it = pending_.find(f.request_id);
        if (it != pending_.end()) {
            Handler handler = std::move(it->second);
            pending_.erase(it);
            handler(f);
            return;
        }
    }
    switch (f.body.type) {
        case wire::FrameBody_Hello: on_hello(*f.body.AsHello()); break;
        case wire::FrameBody_AuthOk: on_auth_ok(*f.body.AsAuthOk()); break;
        case wire::FrameBody_RoomInfo: store_room(*f.body.AsRoomInfo()); break;
        case wire::FrameBody_ServerInfo: on_server_info(*f.body.AsServerInfo()); break;
        case wire::FrameBody_HistoryWanted: on_history_wanted(*f.body.AsHistoryWanted()); break;
        case wire::FrameBody_DevicesChanged:
            // Someone added a device; ask again who their devices are before the next send.
            devices_.erase(f.body.AsDevicesChanged()->user_id);
            // A new device has nobody's profile, not even its own person's.
            reshare_profile_soon(f.body.AsDevicesChanged()->user_id);
            break;
        case wire::FrameBody_Ephemeral: {
            const auto* eph = f.body.AsEphemeral();
            if (eph->kind.rfind("presence:", 0) == 0) {
                std::string status = eph->kind.substr(9);
                if (status == "offline") presence_.erase(eph->sender_user);
                else presence_[eph->sender_user] = status;
                emit({{"event", "presence"}, {"user_id", b64(eph->sender_user)}, {"status", status}});
                break;
            }
            if (eph->kind != "typing") break;
            auto room = vault_.room(eph->room_id);
            if (!room || room->server_id != id_) break;
            for (const auto& m : room->members)
                if (m.user_id == eph->sender_user)
                    emit({{"event", "typing"},
                          {"room_id", b64(eph->room_id)},
                          {"user_id", b64(m.user_id)},
                          {"username", m.username},
                          {"display_name", m.display()}});
            break;
        }
        case wire::FrameBody_SendOk: {
            // Not an answer to anything just asked: a message this device
            // scheduled has now been delivered, and this is where it landed.
            const auto* sent = f.body.AsSendOk();
            auto event = vault_.event(sent->room_id, sent->event_id);
            if (!event || event->status != "scheduled") break;
            {
                db::Transaction tx(vault_.db());
                vault_.confirm_event(sent->room_id, sent->event_id, sent->seq, sent->server_ts);
                vault_.advance_cursor(sent->room_id, sent->seq);
                vault_.set_meta("sched:" + b64(sent->event_id), "");
                tx.commit();
            }
            emit({{"event", "event_send_status"},
                  {"room_id", b64(sent->room_id)},
                  {"event_id", b64(sent->event_id)},
                  {"status", "sent"},
                  {"event_seq", sent->seq},
                  {"server_ts", sent->server_ts}});
            if (auto now = vault_.event(sent->room_id, sent->event_id))
                emit({{"event", "event_updated"}, {"room_id", b64(sent->room_id)}, {"data", event_json(*now)}});
            break;
        }
        case wire::FrameBody_Notice:
            emit({{"event", "server_notice"}, {"message", f.body.AsNotice()->message}});
            break;
        case wire::FrameBody_RoomList: reconcile_rooms(*f.body.AsRoomList()); break;
        case wire::FrameBody_RoomEvent: on_room_event(*f.body.AsRoomEvent()); break;
        case wire::FrameBody_Error:
            emit({{"event", "warning"}, {"message", "server: " + f.body.AsError()->message}});
            break;
        default: break;
    }
}

void Session::on_hello(const wire::HelloT& hello) {
    if (hello.protocol_version != kProtocolVersion) {
        want_connection_ = false;
        drop_connection("the server speaks a different protocol version");
        return;
    }
    challenge_auth_msg_ = signed_message(kCtxAuth, {hello.challenge, to_bytes(hello.server_name), tls_exporter_});
    if (meta("registered") != "1") {
        send_register();
        return;
    }
    wire::AuthenticateT auth;
    auth.device_id = to_bytes(vault_.identity().device.pk);
    auth.signature = sign(vault_.identity().device.sk, challenge_auth_msg_);
    auth.caps = kCapSharedPayload;
    request(std::move(auth), [this](wire::FrameT& f) {
        auto* e = f.body.AsError();
        if (!e || e->code == kDisconnected) return;
        if (e->code == err::UnknownDevice) {
            // The server does not know us (for example its data was reset).
            set_meta("prekeys_published", "0");
            send_register();
            return;
        }
        if (e->code == err::Kicked) {
            // Removed from the community. Do not walk straight back in: the
            // person has to choose to rejoin, which registers again.
            set_meta("registered", "0");
            want_connection_ = false;
            drop_connection("you were removed from this server; connect again to rejoin");
            return;
        }
        want_connection_ = false;
        drop_connection("sign-in failed: " + e->message);
    });
}

void Session::send_register() {
    const auto& id = vault_.identity();
    wire::RegisterT reg;
    reg.username = vault_.username();
    reg.user_id = to_bytes(id.user.pk);
    reg.device_id = to_bytes(id.device.pk);
    reg.dh_key = to_bytes(id.dh.pk);
    reg.cert = id.cert;
    reg.signature = sign(id.device.sk, challenge_auth_msg_);
    reg.invite = meta("invite").value_or("");
    reg.caps = kCapSharedPayload;
    request(std::move(reg), [this](wire::FrameT& f) {
        auto* e = f.body.AsError();
        if (!e || e->code == kDisconnected) return;
        want_connection_ = false;
        drop_connection("registration failed: " + e->message +
                        (e->code == err::NameTaken ? "; choose another name (set_username) and it will try again" : ""));
    });
}

void Session::on_auth_ok(const wire::AuthOkT& auth) {
    is_admin_ = auth.is_admin;
    emit({{"event", "account"}, {"username", vault_.username()}, {"is_admin", is_admin_}});
    set_meta("registered", "1");
    backoff_s_ = 1;
    set_conn(Conn::Syncing);
    publish_prekeys();
    request(wire::ListRoomsT{}, [this](wire::FrameT& f) {
        if (f.body.type == wire::FrameBody_Error) return;
        if (auto* list = f.body.AsRoomList()) reconcile_rooms(*list);
        wire::SyncT sync;
        for (const auto& room : rooms()) {
            auto cur = std::make_unique<wire::CursorT>();
            cur->room_id = room.room_id;
            cur->seq = room.acked_seq;
            sync.cursors.push_back(std::move(cur));
        }
        request(std::move(sync), [this](wire::FrameT& done) {
            if (done.body.type != wire::FrameBody_SyncComplete) return;
            set_conn(Conn::Live);
            // Whoever was online before is unknown now; the server says again.
            presence_.clear();
            emit({{"event", "presence_reset"}});
            send_presence();
            pump_outbox();
        });
    });
}

void Session::on_server_info(const wire::ServerInfoT& info) {
    server_name_ = info.name;
    server_description_ = info.description;
    server_icon_ = info.icon;
    server_owner_ = info.owner;
    my_permissions_ = info.my_permissions;
    history_sharing_ = info.history_sharing;
    roles_.clear();
    for (const auto& r : info.roles)
        if (r) roles_.push_back({r->role_id, r->name, r->position, r->permissions, r->is_everyone});
    sections_.clear();
    for (const auto& s : info.sections)
        if (s) sections_.push_back({s->section_id, s->name, s->position});
    json j = server_json();
    j["event"] = "server_info";
    emit(std::move(j));
}

// The server's list is the truth about which rooms we are in: store what it
// lists and drop what it does not (a channel we can no longer see, a group we
// were removed from).
void Session::reconcile_rooms(const wire::RoomListT& list) {
    std::set<Bytes> current;
    for (const auto& room : list.rooms) {
        if (!room) continue;
        store_room(*room);
        current.insert(room->room_id);
    }
    for (const auto& known : rooms()) {
        if (current.count(known.room_id)) continue;
        {
            db::Transaction tx(vault_.db());
            vault_.delete_room(known.room_id);
            tx.commit();
        }
        emit({{"event", "room_removed"}, {"room_id", b64(known.room_id)}});
    }
}

// ---------------------------------------------------------------- receipts
// A read receipt says "I have read up to this message". It travels as an
// encrypted event like any other, so the server cannot tell who read what.

Session::json Session::receipts_json(ByteView room_id) {
    json out = json::array();
    auto room = vault_.room(room_id);
    for (const auto& r : vault_.receipts(room_id)) {
        json j = {{"user_id", b64(r.user_id)}, {"event_id", b64(r.event_id)}, {"event_seq", r.seq}};
        if (room)
            for (const auto& m : room->members)
                if (m.user_id == r.user_id) {
                    j["username"] = m.username;
                    j["display_name"] = m.display();
                }
        out.push_back(std::move(j));
    }
    return out;
}

// ---------------------------------------------------------------- profiles
// A profile is what a person says about themselves: a display name and a few
// optional details. It travels as an encrypted event to the chats they are
// in, so the people they talk with have it and no server does.

Engine::json Engine::own_profile() {
    json p = json::parse(vault_.meta("profile").value_or("{}"), nullptr, false);
    return p.is_object() ? p : json::object();
}

void Session::accept_profile(const EventRow& e) {
    // Only a person's own word about themselves counts.
    json content = json::parse(e.content, nullptr, false);
    if (!content.is_object() || !content.value("version", json()).is_number_unsigned()) return;
    if (content.dump().size() > 64 * 1024) return;  // room for a small picture
    if (!vault_.set_profile(e.sender_user, content["version"].get<uint64_t>(), content.dump())) return;
    // From another device of this same person: it is this device's profile too,
    // if it is newer than what this device holds.
    const Key32& me = vault_.identity().user.pk;
    if (e.sender_user.size() == 32 && std::equal(me.begin(), me.end(), e.sender_user.begin())) {
        json mine = engine_.own_profile();
        if (mine.value("version", uint64_t{0}) < content["version"].get<uint64_t>())
            vault_.set_meta("profile", content.dump());
    }
    emit({{"event", "profile_updated"}, {"user_id", b64(e.sender_user)}, {"profile", content}});
    // Their name may have changed wherever they appear.
    for (const auto& room : rooms())
        for (const auto& m : room.members)
            if (m.user_id == e.sender_user) {
                emit({{"event", "room_updated"}, {"room", room_json(room)}});
                break;
            }
}

void Session::share_profile(ByteView room_id, bool force) {
    json profile = engine_.own_profile();
    if (!profile.contains("version")) return;  // nothing set yet
    if (vault_.meta("archived:" + b64(room_id)).value_or("0") == "1") return;  // closed to writing
    auto room = vault_.room(room_id);
    if (!room) return;
    // Sent once per version and per set of people, so someone who joins
    // later gets it with the next thing said here.
    std::string who;
    for (const auto& m : room->members) who += b64(m.user_id);
    std::string mark = std::to_string(profile["version"].get<uint64_t>()) + ":" +
                       std::to_string(std::hash<std::string>{}(who));
    std::string key = "profile_sent:" + b64(room_id);
    if (!force && vault_.meta(key).value_or("") == mark) return;
    vault_.set_meta(key, mark);
    cmd_send_event(engine_.next_request_++, {{"room_id", b64(room_id)}, {"type", "m.profile"}, {"content", profile}});
}

// A device that was just added has seen no profiles: they were sent before it
// existed. A few seconds after it appears (so its keys are published), this
// person's profile is sent once more, in the smallest chat shared with its
// owner. The owner's other devices do the same, which is how a new device
// learns its own person's profile.
void Session::reshare_profile_soon(const Bytes& user_id) {
    if (!engine_.own_profile().contains("version")) return;
    reshare_for_.insert(user_id);
    reshare_timer_.expires_after(std::chrono::seconds(5));
    reshare_timer_.async_wait([this](asio::error_code ec) {
        if (ec || conn_ != Conn::Live) return;
        auto people = std::move(reshare_for_);
        reshare_for_.clear();
        std::set<Bytes> done;  // one message per chat, however many people it covers
        for (const auto& user : people) {
            const RoomRow* best = nullptr;
            auto all = rooms();
            for (const auto& room : all) {
                bool shared = false;
                for (const auto& m : room.members)
                    if (m.user_id == user) shared = true;
                if (shared && (!best || room.members.size() < best->members.size())) best = &room;
            }
            if (best && done.insert(best->room_id).second) share_profile(best->room_id, true);
        }
    });
}

void Session::share_profile_everywhere() {
    for (const auto& room : rooms()) share_profile(room.room_id, true);
}

void Session::accept_receipt(const EventRow& receipt) {
    if (receipt.rel_kind != "reference" || receipt.rel_target.size() != 16) return;
    auto target = vault_.event(receipt.room_id, receipt.rel_target);
    if (!target || !target->seq) return;  // a receipt for something we never had
    if (!vault_.set_receipt(receipt.room_id, receipt.sender_user, receipt.rel_target, *target->seq)) return;
    const auto& me = vault_.identity().user.pk;
    // Read on another of this person's devices: the unread count drops here too.
    if (receipt.sender_user.size() == 32 && std::equal(me.begin(), me.end(), receipt.sender_user.begin()))
        if (auto room = vault_.room(receipt.room_id)) emit({{"event", "room_updated"}, {"room", room_json(*room)}});
    json j = {{"event", "receipt"},
              {"room_id", b64(receipt.room_id)},
              {"user_id", b64(receipt.sender_user)},
              {"event_id", b64(receipt.rel_target)},
              {"event_seq", *target->seq}};
    if (auto room = vault_.room(receipt.room_id))
        for (const auto& m : room->members)
            if (m.user_id == receipt.sender_user) {
                j["username"] = m.username;
                j["display_name"] = m.display();
            }
    emit(std::move(j));
}

// ---------------------------------------------------------------- history
// Messages from before someone joined were never encrypted to them. A newcomer
// asks; members' clients that are willing hand the messages over, encrypted to
// the newcomer alone. See master_plan/04-community-model.md, D-28.

void Session::request_history(const Bytes& room_id, uint64_t req) {
    if (conn_ != Conn::Live && conn_ != Conn::Syncing) {
        if (req) fail(req, "not_connected", "not connected to a server");
        return;
    }
    if (!history_sharing_) {
        if (req) fail(req, "refused", "this server does not allow sharing earlier messages");
        return;
    }
    history_asked_[room_id] = now_ms();
    wire::HistoryRequestT q;
    q.room_id = room_id;
    q.limit = 200;
    request(std::move(q), [this, req](wire::FrameT& r) {
        if (!req) return;
        if (r.body.type == wire::FrameBody_Ok) {
            ok(req);
            return;
        }
        auto* e = r.body.AsError();
        fail(req, "refused", e ? e->message : "the server refused");
    });
}

// Someone new wants earlier messages from a room we are in.
void Session::on_history_wanted(const wire::HistoryWantedT& wanted) {
    auto room = vault_.room(wanted.room_id);
    if (!room || room->server_id != id_ || wanted.requester.size() != 32) return;
    const Key32& me = vault_.identity().user.pk;
    // Another device of the same person always gets its own history; anyone
    // else only if this member has not opted out.
    bool own_device = std::equal(wanted.requester.begin(), wanted.requester.end(), me.begin());
    if (!own_device && (room->kind == 1 || vault_.meta("share_history").value_or("1") == "0")) return;
    bool is_member = false;
    for (const auto& m : room->members)
        if (m.user_id == wanted.requester) is_member = true;
    if (!is_member) return;  // only people the server has told us are in the room

    json batch = json::array();
    auto flush = [&] {
        if (batch.empty()) return;
        EventRow e;
        e.room_id = room->room_id;
        e.event_id = new_event_id();
        e.type = "m.history.share";
        e.sender_user = to_bytes(me);
        e.sender_device = to_bytes(vault_.identity().device.pk);
        e.origin_ts = now_ms();
        e.content = json{{"events", std::move(batch)}}.dump();
        e.status = "pending";
        e.expires_at = e.origin_ts + 3600 * 1000;  // the envelope itself need not be kept
        db::Transaction tx(vault_.db());
        vault_.insert_event(e);
        vault_.outbox_push(e.room_id, e.event_id, wanted.requester);
        tx.commit();
        batch = json::array();
    };
    // A device of this same person also gets the profiles held here for the
    // people in the room (its own person's among them), a few at a time so a
    // handful of pictures never makes one message too large.
    if (own_device) {
        json profiles = json::array();
        auto send_profiles = [&] {
            if (profiles.empty()) return;
            EventRow e;
            e.room_id = room->room_id;
            e.event_id = new_event_id();
            e.type = "m.history.share";
            e.sender_user = to_bytes(me);
            e.sender_device = to_bytes(vault_.identity().device.pk);
            e.origin_ts = now_ms();
            e.content = json{{"profiles", std::move(profiles)}}.dump();
            e.status = "pending";
            e.expires_at = e.origin_ts + 3600 * 1000;
            db::Transaction tx(vault_.db());
            vault_.insert_event(e);
            vault_.outbox_push(e.room_id, e.event_id, wanted.requester);
            tx.commit();
            profiles = json::array();
        };
        for (const auto& m : room->members) {
            auto held = vault_.profile(m.user_id);
            json profile = held ? json::parse(*held, nullptr, false) : json();
            if (m.user_id.size() == 32 && std::equal(me.begin(), me.end(), m.user_id.begin())) {
                json mine = engine_.own_profile();
                if (mine.contains("version")) profile = mine;
            }
            if (!profile.is_object() || !profile.contains("version")) continue;
            profiles.push_back({{"user_id", b64(m.user_id)}, {"profile", std::move(profile)}});
            if (profiles.size() >= 4) send_profiles();
        }
        send_profiles();
    }
    uint32_t limit = std::min<uint32_t>(wanted.limit ? wanted.limit : 200, 200);
    for (const auto& e : vault_.timeline(room->room_id, limit)) {
        // Never disappearing messages, deleted ones, unsent ones, or history
        // envelopes themselves.
        if (e.status != "ok" || e.expires_at != 0 || !e.seq || e.type == "m.history.share" ||
            e.type == "m.receipt" || e.type == "m.profile")
            continue;
        json item = {{"event_id", b64(e.event_id)},
                     {"type", e.type},
                     {"type_version", e.type_version},
                     {"sender_user", b64(e.sender_user)},
                     {"sender_device", b64(e.sender_device)},
                     {"origin_ts", e.origin_ts},
                     {"server_ts", e.server_ts},
                     {"seq", *e.seq},
                     {"state_key", e.state_key},
                     {"content", e.content},
                     {"edited_content", e.edited_content},
                     {"fallback_text", e.fallback_text}};
        if (!e.rel_kind.empty())
            item["relation"] = {{"kind", e.rel_kind}, {"target", b64(e.rel_target)}, {"key", e.rel_key}};
        batch.push_back(std::move(item));
        if (batch.size() >= 40) flush();
    }
    flush();
    pump_outbox();
}

// A member answered our request. Only accepted if we asked, recently, and the
// sender is in the room.
void Session::accept_history(const EventRow& share) {
    auto asked = history_asked_.find(share.room_id);
    if (asked == history_asked_.end() || now_ms() - asked->second > 10 * 60 * 1000) return;
    auto room = vault_.room(share.room_id);
    if (!room) return;
    bool from_member = false;
    for (const auto& m : room->members)
        if (m.user_id == share.sender_user) from_member = true;
    if (!from_member) return;
    json content = json::parse(share.content, nullptr, false);
    if (!content.is_object()) return;
    // From another device of this same person: the profiles it holds, which
    // were sent before this device existed. Nobody else's word is taken for
    // what a third person says about themselves.
    const Key32& self = vault_.identity().user.pk;
    if (content.value("profiles", json()).is_array() && share.sender_user.size() == 32 &&
        std::equal(self.begin(), self.end(), share.sender_user.begin())) {
        for (const auto& item : content["profiles"]) {
            auto who = item.is_object() ? unb64(item.value("user_id", "")) : std::nullopt;
            json profile = item.is_object() ? item.value("profile", json()) : json();
            if (!who || who->size() != 32 || !profile.is_object() ||
                !profile.value("version", json()).is_number_unsigned() || profile.dump().size() > 64 * 1024)
                continue;
            uint64_t version = profile["version"].get<uint64_t>();
            if (!vault_.set_profile(*who, version, profile.dump())) continue;
            if (std::equal(self.begin(), self.end(), who->begin()) &&
                engine_.own_profile().value("version", uint64_t{0}) < version)
                vault_.set_meta("profile", profile.dump());
            emit({{"event", "profile_updated"}, {"user_id", b64(*who)}, {"profile", profile}});
        }
        emit({{"event", "room_updated"}, {"room", room_json(*room)}});
    }
    if (!content.value("events", json()).is_array()) return;

    std::vector<EventRow> added;
    {
        db::Transaction tx(vault_.db());
        for (const auto& item : content["events"]) {
            try {
                EventRow e;
                e.room_id = share.room_id;
                e.event_id = need_b64(item, "event_id", 16);
                if (vault_.has_event(e.room_id, e.event_id)) continue;
                e.type = item.at("type").get<std::string>();
                if (e.type.empty() || e.type == "m.history.share") continue;
                e.type_version = item.value("type_version", uint16_t{1});
                e.sender_user = need_b64(item, "sender_user", 32);
                e.sender_device = need_b64(item, "sender_device", 32);
                e.origin_ts = item.value("origin_ts", uint64_t{0});
                e.server_ts = item.value("server_ts", uint64_t{0});
                e.seq = item.at("seq").get<uint64_t>();
                e.state_key = item.value("state_key", std::string{});
                e.content = item.value("content", std::string{"{}"});
                e.edited_content = item.value("edited_content", std::string{});
                e.fallback_text = item.value("fallback_text", std::string{});
                e.status = "ok";
                e.shared_by = share.sender_user;
                if (item.contains("relation")) {
                    e.rel_kind = item["relation"].value("kind", std::string{});
                    e.rel_target = need_b64(item["relation"], "target", 16);
                    e.rel_key = item["relation"].value("key", std::string{});
                }
                if (e.content.size() > 256 * 1024) continue;
                if (vault_.insert_event(e)) added.push_back(std::move(e));
            } catch (const std::exception&) {
                continue;  // one malformed entry does not spoil the rest
            }
        }
        tx.commit();
    }
    for (const auto& e : added)
        emit({{"event", "event_received"}, {"room_id", b64(e.room_id)}, {"data", event_json(e)}});
    if (!added.empty())
        emit({{"event", "history_received"}, {"room_id", b64(share.room_id)}, {"count", added.size()}});
}

void Session::publish_prekeys() {
    if (meta("prekeys_published") == "1") return;
    const auto& id = vault_.identity();
    wire::PublishPrekeysT pub;
    {
        db::Transaction tx(vault_.db());
        uint32_t next = vault_.next_prekey_id();
        crypto::KeyPair spk = vault_.add_prekey(0, next);
        pub.spk_id = next;
        pub.spk = to_bytes(spk.pk);
        pub.spk_sig = sign(id.device.sk, signed_message(kCtxSignedPrekey, {spk.pk}));
        for (int i = 1; i <= kOneTimePrekeyBatch; ++i) {
            crypto::KeyPair otk = vault_.add_prekey(1, next + static_cast<uint32_t>(i));
            auto entry = std::make_unique<wire::KeyEntryT>();
            entry->id = next + static_cast<uint32_t>(i);
            entry->key = to_bytes(otk.pk);
            pub.otks.push_back(std::move(entry));
        }
        tx.commit();
    }
    request(std::move(pub), [this](wire::FrameT& f) {
        if (f.body.type == wire::FrameBody_Ok) set_meta("prekeys_published", "1");
    });
}

void Session::store_room(const wire::RoomInfoT& info) {
    if (info.room_id.size() != 16) return;
    std::vector<MemberRow> members;
    for (const auto& m : info.members)
        if (m && m->user_id.size() == 32)
            members.push_back({m->user_id, m->username, m->is_admin, m->is_owner, m->roles, m->nickname});
    // A member list without us means we were removed from the room.
    const Key32& me = vault_.identity().user.pk;
    bool still_in = false;
    for (const auto& m : members)
        if (std::equal(m.user_id.begin(), m.user_id.end(), me.begin())) still_in = true;
    if (!still_in) {
        if (!vault_.room(info.room_id)) return;
        db::Transaction tx(vault_.db());
        vault_.delete_room(info.room_id);
        tx.commit();
        emit({{"event", "room_removed"}, {"room_id", b64(info.room_id)}, {"reason", "removed"}});
        return;
    }
    bool first_sight = !vault_.room(info.room_id).has_value();
    {
        db::Transaction tx(vault_.db());
        vault_.upsert_room(info.room_id, members, info.kind, info.name, id_);
        vault_.set_meta("nsfw:" + b64(info.room_id), info.nsfw ? "1" : "0");
        vault_.set_meta("featured:" + b64(info.room_id), info.featured ? "1" : "0");
        vault_.set_meta("section:" + b64(info.room_id), std::to_string(info.section));
        vault_.set_meta("can_send:" + b64(info.room_id), info.can_send ? "1" : "0");
        vault_.set_meta("archived:" + b64(info.room_id), info.archived ? "1" : "0");
        vault_.set_meta("channel_type:" + b64(info.room_id), info.channel_type);
        tx.commit();
    }
    // New to this room: ask whether anyone will share what was said before.
    if (first_sight && members.size() > 1) request_history(info.room_id, 0);
    if (auto room = vault_.room(info.room_id))
        emit({{"event", "room_updated"}, {"room", room_json(*room)}});
}

void Session::on_room_event(const wire::RoomEventT& ev) {
    if (ev.room_id.size() != 16 || ev.event_id.size() != 16 || ev.sender_user.size() != 32 ||
        ev.sender_device.size() != 32)
        return;
    if (vault_.has_event(ev.room_id, ev.event_id)) {
        vault_.advance_cursor(ev.room_id, ev.seq);
        return;
    }

    EventRow row;
    row.room_id = ev.room_id;
    row.event_id = ev.event_id;
    row.seq = ev.seq;
    row.sender_user = ev.sender_user;
    row.sender_device = ev.sender_device;
    row.server_ts = ev.server_ts;
    row.origin_ts = ev.server_ts;
    bool decrypted = false;
    {
        // The ratchet step, the consumed prekey, the stored event and the sync
        // cursor commit together or not at all.
        db::Transaction tx(vault_.db());
        vault_.upsert_room(ev.room_id, {}, -1, "", id_);
        // Sessions are per device. A device must keep belonging to the same person.
        auto peer = vault_.load_sessions(ev.sender_device).value_or(crypto::PeerSessions{});
        bool same_device = peer.empty() || to_bytes(peer.user_id) == ev.sender_user;
        peer.user_id = to_key32(ev.sender_user);
        peer.device_id = to_key32(ev.sender_device);
        std::optional<Bytes> plain;
        if (same_device)
            plain = crypto::decrypt(vault_.identity(), vault_, peer,
                                    context_of(ev.room_id, ev.event_id), ev.ciphertext);
        // Sent in the shared form: what the ratchet carried is the key to the
        // part everyone got, and a fingerprint that part must match.
        if (plain && !ev.shared.empty())
            plain = open_shared(*plain, ev.shared, context_of(ev.room_id, ev.event_id));
        if (plain) {
            flatbuffers::Verifier verifier(plain->data(), plain->size());
            const wire::Event* inner =
                verifier.VerifyBuffer<wire::Event>() ? flatbuffers::GetRoot<wire::Event>(plain->data())
                                                     : nullptr;
            wire::EventT e;
            if (inner) inner->UnPackTo(&e);
            // The claims inside the event must match who actually encrypted it.
            if (inner && e.event_id == ev.event_id && e.sender_user == ev.sender_user &&
                e.sender_device == ev.sender_device && !e.type.empty()) {
                row.type = e.type;
                row.type_version = e.type_version;
                row.origin_ts = e.origin_ts;
                row.state_key = e.state_key;
                row.content = to_string(e.content);
                row.fallback_text = e.fallback_text;
                row.expires_at = e.expires_at;
                row.status = "ok";
                if (e.relation && e.relation->target.size() == 16) {
                    row.rel_kind = e.relation->kind;
                    row.rel_target = e.relation->target;
                    row.rel_key = e.relation->key;
                }
                vault_.insert_event(row);
                vault_.save_sessions(peer);
                vault_.advance_cursor(ev.room_id, ev.seq);
                tx.commit();
                decrypted = true;
            }
        }
    }
    if (!decrypted) {
        row.type = "m.undecryptable";
        row.content = "{}";
        row.status = "undecryptable";
        db::Transaction tx(vault_.db());
        vault_.upsert_room(ev.room_id, {}, -1, "", id_);
        vault_.insert_event(row);
        vault_.advance_cursor(ev.room_id, ev.seq);
        tx.commit();
    }
    if (decrypted && row.type == "m.history.share") {
        accept_history(row);
        return;  // the envelope is not a message; frontends never see it
    }
    if (decrypted && row.type == "m.receipt") {
        accept_receipt(row);
        return;  // shown on the message it refers to, not as a message
    }
    if (decrypted && row.type == "m.profile") {
        accept_profile(row);
        return;  // about a person, not part of the conversation
    }
    // Announce the event first, then whatever it changes.
    emit({{"event", "event_received"},
          {"room_id", b64(row.room_id)},
          {"unread", vault_.unread(row.room_id, to_bytes(vault_.identity().user.pk))},
          {"data", event_json(row)}});
    if (decrypted) {
        apply_state(row);
        apply_relation(row);
    }
}

void Session::fail_outbox(const OutboxRow& row, const std::string& message) {
    {
        db::Transaction tx(vault_.db());
        vault_.set_event_status(row.room_id, row.event_id, "failed");
        vault_.outbox_remove(row.local_id);
        tx.commit();
    }
    emit({{"event", "event_send_status"},
          {"room_id", b64(row.room_id)},
          {"event_id", b64(row.event_id)},
          {"status", "failed"},
          {"message", message}});
}

// Sends the oldest queued event. One send is in flight at a time, which keeps
// events in order and makes retries simple.
void Session::pump_outbox() {
    if (conn_ != Conn::Live || sending_ || !vault_.unlocked()) return;
    // The oldest queued event that belongs to this server.
    std::optional<OutboxRow> next;
    for (const auto& queued : vault_.outbox()) {
        auto its_room = vault_.room(queued.room_id);
        if (!its_room || !vault_.event(queued.room_id, queued.event_id)) {
            vault_.outbox_remove(queued.local_id);  // its room or event is gone
            continue;
        }
        if (its_room->server_id == id_) {
            next = queued;
            break;
        }
    }
    if (!next) return;
    OutboxRow row = *next;
    auto room = vault_.room(row.room_id);
    auto event = vault_.event(row.room_id, row.event_id);
    // Taken back before it left (a scheduled message cancelled while offline, say).
    if (event->status == "redacted" && event->type != "m.redaction") {
        vault_.outbox_remove(row.local_id);
        pump_outbox();
        return;
    }

    // Who gets a copy: every other member, or the one member an envelope is
    // for. Each person may have several devices, and this person's own other
    // devices get a copy too, so everything shows up everywhere.
    const Key32& me = vault_.identity().user.pk;
    const Bytes my_user = to_bytes(me);
    const Bytes my_device = to_bytes(vault_.identity().device.pk);
    std::vector<Bytes> others;
    for (const auto& m : room->members)
        if (m.user_id.size() == 32 && m.user_id != my_user) others.push_back(m.user_id);
    std::vector<Bytes> users = others;
    users.push_back(my_user);
    if (!row.only_user.empty()) {
        bool present = row.only_user == my_user ||
                       std::find(others.begin(), others.end(), row.only_user) != others.end();
        if (!present) {
            vault_.outbox_remove(row.local_id);
            pump_outbox();
            return;
        }
        users = {row.only_user};
    }

    // Learn each person's devices, then make sure there is a session with each.
    bool waiting = false;
    std::vector<Bytes> targets;  // device ids
    for (const auto& user : users) {
        auto known = devices_.find(user);
        if (known == devices_.end()) {
            waiting = true;
            if (!devices_requested_.insert(user).second) continue;
            wire::ListDevicesT q;
            q.user_id = user;
            request(std::move(q), [this, user](wire::FrameT& f) {
                devices_requested_.erase(user);
                auto* list = f.body.AsDeviceList();
                if (!list) return;  // retried on the next attempt to send
                std::vector<Bytes> ids;
                for (const auto& d : list->devices)
                    if (d && d->device_id.size() == 32) {
                        ids.push_back(d->device_id);
                        device_caps_[d->device_id] = d->caps;
                    }
                devices_[user] = std::move(ids);
                pump_outbox();
            });
            continue;
        }
        for (const auto& device : known->second) {
            if (device == my_device || unreachable_.count(device)) continue;
            auto sessions = vault_.load_sessions(device);
            if (sessions && !sessions->empty()) {
                targets.push_back(device);
                continue;
            }
            waiting = true;
            if (!bundle_requested_.insert(device).second) continue;
            wire::FetchBundleT q;
            q.user_id = user;
            q.device_id = device;
            request(std::move(q), [this, user, device](wire::FrameT& f) {
                bundle_requested_.erase(device);
                auto* b = f.body.AsBundle();
                if (!b) {
                    auto* e = f.body.AsError();
                    if (e && e->code == kDisconnected) return;  // retried after reconnect
                    // A device that has published no keys cannot be reached yet;
                    // carry on without it rather than hold everything up.
                    unreachable_.insert(device);
                    pump_outbox();
                    return;
                }
                try {
                    if (b->user_id != user || b->device_id != device || b->dh_key.size() != 32 ||
                        b->spk.size() != 32 || (b->has_otk && b->otk.size() != 32))
                        throw crypto::CryptoError("malformed key bundle");
                    crypto::PeerBundle bundle;
                    bundle.user_id = to_key32(b->user_id);
                    bundle.device_id = to_key32(b->device_id);
                    bundle.dh_key = to_key32(b->dh_key);
                    bundle.cert = b->cert;
                    bundle.spk_id = b->spk_id;
                    bundle.spk = to_key32(b->spk);
                    bundle.spk_sig = b->spk_sig;
                    bundle.has_otk = b->has_otk;
                    bundle.otk_id = b->otk_id;
                    if (b->has_otk) bundle.otk = to_key32(b->otk);
                    // start_session checks that the person's identity key signed this device.
                    crypto::PeerSessions peer;
                    crypto::start_session(vault_.identity(), bundle, peer);
                    vault_.save_sessions(peer);
                } catch (const std::exception& e) {
                    unreachable_.insert(device);
                    emit({{"event", "warning"},
                          {"message", std::string("could not start a secure session with a device: ") + e.what()}});
                }
                pump_outbox();
            });
        }
    }
    if (waiting) return;
    // A channel may have nobody else in it yet; the message still gets its place.
    if (others.empty() && room->kind != 0) {
        fail_outbox(row, "nobody else is in this room");
        pump_outbox();
        return;
    }

    // Build the plaintext event.
    wire::EventT inner;
    inner.event_id = event->event_id;
    inner.type = event->type;
    inner.type_version = event->type_version;
    inner.sender_user = event->sender_user;
    inner.sender_device = event->sender_device;
    inner.origin_ts = event->origin_ts;
    inner.state_key = event->state_key;
    inner.content_encoding = 0;
    inner.content = to_bytes(event->content);
    inner.fallback_text = event->fallback_text;
    inner.expires_at = event->expires_at;
    if (!event->rel_kind.empty()) {
        inner.relation = std::make_unique<wire::RelationT>();
        inner.relation->kind = event->rel_kind;
        inner.relation->target = event->rel_target;
        inner.relation->key = event->rel_key;
    }
    flatbuffers::FlatBufferBuilder fbb(512);
    fbb.Finish(wire::Event::Pack(fbb, &inner));
    ByteView plaintext(fbb.GetBufferPointer(), fbb.GetSize());

    wire::SendRoomEventT send;
    send.room_id = row.room_id;
    send.event_id = row.event_id;
    send.expires_at = event->expires_at;  // lets the server drop its copy on time
    if (event->status == "scheduled")
        send.send_at = std::strtoull(vault_.meta("sched:" + b64(row.event_id)).value_or("0").c_str(), nullptr, 10);
    try {
        // Persist each advanced ratchet before its ciphertext leaves the
        // process, so a crash can never reuse a message key.
        db::Transaction tx(vault_.db());
        Bytes context = context_of(row.room_id, row.event_id);
        // To several devices that all understand it, the event is encrypted
        // once under a key of its own, and each device's ratchet carries only
        // that key and a fingerprint of the result. Every device still gets
        // its own ratchet step, so nothing about who can read what changes;
        // what changes is that a message with a picture in it is uploaded and
        // stored once instead of once per device.
        bool share = targets.size() >= 2 &&
                     std::all_of(targets.begin(), targets.end(), [&](const Bytes& d) {
                         auto caps = device_caps_.find(d);
                         return caps != device_caps_.end() && (caps->second & kCapSharedPayload);
                     });
        Bytes key_note;
        if (share) {
            Bytes key_bytes = random_bytes(32), nonce = random_bytes(24);
            Key32 key{};
            std::copy(key_bytes.begin(), key_bytes.end(), key.begin());
            send.shared = crypto::aead_encrypt(key, nonce, plaintext, context);
            key_note = shared_key_note(key_bytes, nonce, send.shared);
            sodium_memzero(key.data(), key.size());
            sodium_memzero(key_bytes.data(), key_bytes.size());
        }
        for (const auto& device : targets) {
            auto peer = vault_.load_sessions(device);
            auto r = std::make_unique<wire::RecipientT>();
            r->device_id = to_bytes(peer->device_id);
            r->ciphertext = crypto::encrypt(vault_.identity(), *peer, context, share ? ByteView(key_note) : plaintext);
            vault_.save_sessions(*peer);
            send.recipients.push_back(std::move(r));
        }
        tx.commit();
    } catch (const std::exception& e) {
        fail_outbox(row, std::string("encryption failed: ") + e.what());
        pump_outbox();
        return;
    }

    sending_ = true;
    request(std::move(send), [this, row](wire::FrameT& f) {
        sending_ = false;
        if (auto* okf = f.body.AsSendOk()) {
            {
                db::Transaction tx(vault_.db());
                vault_.confirm_event(row.room_id, row.event_id, okf->seq, okf->server_ts);
                vault_.advance_cursor(row.room_id, okf->seq);
                vault_.outbox_remove(row.local_id);
                tx.commit();
            }
            emit({{"event", "event_send_status"},
                  {"room_id", b64(row.room_id)},
                  {"event_id", b64(row.event_id)},
                  {"status", "sent"},
                  {"event_seq", okf->seq},
                  {"server_ts", okf->server_ts}});
            pump_outbox();
            return;
        }
        if (auto* waiting = f.body.AsScheduled()) {
            // The server holds it; it tells this device when it has gone out.
            vault_.outbox_remove(row.local_id);
            emit({{"event", "event_send_status"},
                  {"room_id", b64(row.room_id)},
                  {"event_id", b64(row.event_id)},
                  {"status", "scheduled"},
                  {"scheduled_for", waiting->send_at}});
            pump_outbox();
            return;
        }
        auto* e = f.body.AsError();
        if (e && e->code == kDisconnected) return;  // stays queued, resent after reconnect
        fail_outbox(row, e ? e->message : "the server rejected the event");
        pump_outbox();
    });
}

}  // namespace corded

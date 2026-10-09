#include "engine/engine.hpp"

#include "corded/common/sig.hpp"

#include <algorithm>
#include <chrono>

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

bool known_type(const std::string& type) {
    return type == "m.text" || type == "m.reaction" || type == "m.room.name";
}

const char* conn_name(int c) {
    static const char* names[] = {"disconnected", "connecting", "authenticating", "syncing", "live"};
    return names[c];
}

}  // namespace

Engine::Engine(EngineConfig config)
    : config_(std::move(config)),
      work_(asio::make_work_guard(io_)),
      resolver_(io_),
      tls_ctx_(asio::ssl::context::tls_client),
      reconnect_timer_(io_) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium failed to initialise");
    // The server is authenticated by its pinned fingerprint, checked after the
    // handshake, so certificate-authority verification is switched off.
    tls::require_tls13(tls_ctx_);
    tls_ctx_.set_verify_mode(asio::ssl::verify_none);
    thread_ = std::thread([this] { io_.run(); });
    asio::post(io_, [this] { emit_vault_state(); });
}

Engine::~Engine() {
    asio::post(io_, [this] {
        want_connection_ = false;
        drop_connection("shutting down");
        vault_.lock();
        io_.stop();
    });
    work_.reset();
    if (thread_.joinable()) thread_.join();
}

bool Engine::vault_exists() const { return Vault::exists(config_.vault_dir); }

// ---------------------------------------------------------------- events out

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

void Engine::set_conn(Conn c, const std::string& detail) {
    conn_ = c;
    json j = {{"event", "connection_state"}, {"state", conn_name(static_cast<int>(c))}};
    if (!detail.empty()) j["detail"] = detail;
    if (!host_.empty()) j["server"] = host_ + ":" + port_;
    if (!server_fingerprint_.empty() && c != Conn::Disconnected && c != Conn::Connecting)
        j["fingerprint"] = server_fingerprint_;
    emit(std::move(j));
}

Engine::json Engine::room_json(const RoomRow& room) {
    json members = json::array();
    std::string title;
    for (const auto& m : room.members) {
        bool me = ByteView(m.user_id).size() == 32 &&
                  std::equal(m.user_id.begin(), m.user_id.end(), vault_.identity().user.pk.begin());
        members.push_back({{"user_id", b64(m.user_id)},
                           {"username", m.username},
                           {"me", me},
                           {"verified", !me && vault_.is_verified(m.user_id)}});
        if (!me) title += (title.empty() ? "" : ", ") + m.username;
    }
    if (!room.name.empty()) title = room.name;
    return {{"room_id", b64(room.room_id)},
            {"title", title.empty() ? "(empty room)" : title},
            {"name", room.name},
            {"is_group", room.members.size() > 2},
            {"members", std::move(members)}};
}

Engine::json Engine::event_json(const EventRow& e) {
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
    j["content"] = json::parse(e.content, nullptr, false);
    if (j["content"].is_discarded()) j["content"] = json::object();
    if (!e.fallback_text.empty()) j["fallback_text"] = e.fallback_text;
    if (!e.rel_kind.empty()) {
        j["relation"] = {{"kind", e.rel_kind}, {"target", b64(e.rel_target)}};
        if (!e.rel_key.empty()) j["relation"]["key"] = e.rel_key;
    }
    if (auto room = vault_.room(e.room_id))
        for (const auto& m : room->members)
            if (m.user_id == e.sender_user) j["sender_name"] = m.username;
    return j;
}

// ---------------------------------------------------------------- commands in

uint64_t Engine::vault_create(Bytes passphrase, std::string username) {
    uint64_t req = next_request_++;
    asio::post(io_, [this, req, pass = std::move(passphrase), name = std::move(username)]() mutable {
        try {
            if (vault_.unlocked()) throw VaultError("the vault is already open");
            vault_.create(config_.vault_dir, pass, name, config_.fast_kdf);
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
            if (vault_.unlocked()) throw VaultError("the vault is already open");
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

void Engine::after_unlock() {
    emit_vault_state();
    for (const auto& room : vault_.rooms()) emit({{"event", "room_updated"}, {"room", room_json(room)}});
    // Reconnect to the server used last time, if there was one.
    auto host = vault_.meta("server_host");
    auto port = vault_.meta("server_port");
    if (host && port) {
        host_ = *host;
        port_ = *port;
        want_connection_ = true;
        start_connect();
    }
}

uint64_t Engine::command(std::string json_text) {
    uint64_t req = next_request_++;
    asio::post(io_, [this, req, text = std::move(json_text)] { run_command(req, text); });
    return req;
}

void Engine::run_command(uint64_t req, const std::string& text) {
    try {
        json cmd = json::parse(text);
        std::string name = cmd.at("cmd").get<std::string>();

        if (name == "status") {
            json data = {{"vault", vault_.unlocked() ? "unlocked" : vault_exists() ? "locked" : "missing"},
                         {"connection", conn_name(static_cast<int>(conn_))}};
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
        } else if (name == "disconnect") {
            want_connection_ = false;
            drop_connection("disconnected by user");
            ok(req);
        } else if (name == "lock") {
            want_connection_ = false;
            drop_connection("vault locked");
            vault_.lock();
            emit_vault_state();
            ok(req);
        } else if (name == "list_rooms") {
            json rooms = json::array();
            for (const auto& room : vault_.rooms()) rooms.push_back(room_json(room));
            ok(req, {{"rooms", std::move(rooms)}});
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
            for (const auto& room : vault_.rooms())
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
            cmd_send_event(req, ev);
        } else if (name == "send_event") {
            cmd_send_event(req, cmd);
        } else if (name == "fetch_timeline") {
            Bytes room_id = need_b64(cmd, "room_id", 16);
            json events = json::array();
            for (const auto& e : vault_.timeline(room_id, cmd.value("limit", 200u)))
                events.push_back(event_json(e));
            ok(req, {{"room_id", b64(room_id)}, {"events", std::move(events)}});
        } else {
            fail(req, "unknown_command", "unknown command: " + name);
        }
    } catch (const std::exception& e) {
        fail(req, "invalid_argument", e.what());
    }
}

void Engine::cmd_connect(uint64_t req, const json& cmd) {
    std::string host = cmd.at("host").get<std::string>();
    std::string port = cmd.at("port").is_string() ? cmd.at("port").get<std::string>()
                                                  : std::to_string(cmd.at("port").get<int>());
    // The prototype knows one server per vault. Pointing at a different one
    // means registering and publishing keys again.
    if (vault_.meta("server_host") != host || vault_.meta("server_port") != port) {
        vault_.set_meta("server_host", host);
        vault_.set_meta("server_port", port);
        vault_.set_meta("registered", "0");
        vault_.set_meta("prekeys_published", "0");
        vault_.set_meta("server_fp", "");
    }
    // An explicit fingerprint (from an invite) must match. "reset_pin" accepts
    // whatever identity the server presents next; use it only on purpose.
    if (cmd.contains("fingerprint")) vault_.set_meta("server_fp", cmd.at("fingerprint").get<std::string>());
    else if (cmd.value("reset_pin", false)) vault_.set_meta("server_fp", "");
    host_ = host;
    port_ = port;
    want_connection_ = true;
    backoff_s_ = 1;
    drop_connection("");
    start_connect();
    ok(req);
}

void Engine::cmd_start_chat(uint64_t req, const json& cmd) {
    if (conn_ != Conn::Live && conn_ != Conn::Syncing) {
        fail(req, "not_connected", "not connected to a server");
        return;
    }
    auto names = std::make_shared<std::vector<std::string>>();
    names->push_back(cmd.at("username").get<std::string>());
    lookup_next(req, names, std::make_shared<wire::CreateRoomT>(), "");
}

// A room with any number of people. Two-person rooms are unique per pair;
// larger ones are always new.
void Engine::cmd_create_room(uint64_t req, const json& cmd) {
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
void Engine::lookup_next(uint64_t req, std::shared_ptr<std::vector<std::string>> names,
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

void Engine::create_room(uint64_t req, wire::CreateRoomT create, std::string room_name) {
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

// Room state carried by events. Only the room name so far.
void Engine::apply_state(const EventRow& e) {
    if (e.type != "m.room.name") return;
    json content = json::parse(e.content, nullptr, false);
    if (!content.is_object() || !content.value("name", json()).is_string()) return;
    std::string name = content["name"].get<std::string>();
    if (name.size() > 80) name.resize(80);
    vault_.set_room_name(e.room_id, name);
    if (auto room = vault_.room(e.room_id))
        emit({{"event", "room_updated"}, {"room", room_json(*room)}});
}

void Engine::cmd_send_event(uint64_t req, const json& cmd) {
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
    {
        db::Transaction tx(vault_.db());
        vault_.insert_event(e);
        vault_.outbox_push(e.room_id, e.event_id);
        tx.commit();
    }
    apply_state(e);
    emit({{"event", "event_received"}, {"room_id", b64(e.room_id)}, {"data", event_json(e)}});
    ok(req, {{"event_id", b64(e.event_id)}});
    pump_outbox();
}

// ---------------------------------------------------------------- network

void Engine::start_connect() {
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
bool Engine::check_server_identity() {
    std::string seen;
    try {
        seen = b64(tls::peer_fingerprint(*stream_));
        tls_exporter_ = tls::exporter(*stream_);
    } catch (const std::exception& e) {
        drop_connection(std::string("secure connection failed: ") + e.what());
        return false;
    }
    std::string pinned = vault_.meta("server_fp").value_or("");
    if (pinned.empty()) {
        vault_.set_meta("server_fp", seen);
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

void Engine::drop_connection(const std::string& reason) {
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

void Engine::schedule_reconnect() {
    int delay = backoff_s_;
    backoff_s_ = std::min(backoff_s_ * 2, 30);
    reconnect_timer_.expires_after(std::chrono::seconds(delay));
    reconnect_timer_.async_wait([this](asio::error_code ec) {
        if (!ec) start_connect();
    });
}

void Engine::read_header(uint64_t gen) {
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

void Engine::read_body(uint64_t gen, uint32_t n) {
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

void Engine::send_frame(const wire::FrameT& f) {
    if (conn_ == Conn::Disconnected || conn_ == Conn::Connecting) return;
    out_.push_back(encode_frame(f));
    if (!writing_) write_next(conn_gen_);
}

void Engine::write_next(uint64_t gen) {
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

void Engine::on_frame(wire::FrameT& f) {
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
        case wire::FrameBody_AuthOk: on_auth_ok(); break;
        case wire::FrameBody_RoomInfo: store_room(*f.body.AsRoomInfo()); break;
        case wire::FrameBody_RoomEvent: on_room_event(*f.body.AsRoomEvent()); break;
        case wire::FrameBody_Error:
            emit({{"event", "warning"}, {"message", "server: " + f.body.AsError()->message}});
            break;
        default: break;
    }
}

void Engine::on_hello(const wire::HelloT& hello) {
    if (hello.protocol_version != kProtocolVersion) {
        want_connection_ = false;
        drop_connection("the server speaks a different protocol version");
        return;
    }
    challenge_auth_msg_ = signed_message(kCtxAuth, {hello.challenge, to_bytes(hello.server_name), tls_exporter_});
    if (vault_.meta("registered") != "1") {
        send_register();
        return;
    }
    wire::AuthenticateT auth;
    auth.device_id = to_bytes(vault_.identity().device.pk);
    auth.signature = sign(vault_.identity().device.sk, challenge_auth_msg_);
    request(std::move(auth), [this](wire::FrameT& f) {
        auto* e = f.body.AsError();
        if (!e || e->code == kDisconnected) return;
        if (e->code == err::UnknownDevice) {
            // The server does not know us (for example its data was reset).
            vault_.set_meta("prekeys_published", "0");
            send_register();
            return;
        }
        want_connection_ = false;
        drop_connection("sign-in failed: " + e->message);
    });
}

void Engine::send_register() {
    const auto& id = vault_.identity();
    wire::RegisterT reg;
    reg.username = vault_.username();
    reg.user_id = to_bytes(id.user.pk);
    reg.device_id = to_bytes(id.device.pk);
    reg.dh_key = to_bytes(id.dh.pk);
    reg.cert = id.cert;
    reg.signature = sign(id.device.sk, challenge_auth_msg_);
    request(std::move(reg), [this](wire::FrameT& f) {
        auto* e = f.body.AsError();
        if (!e || e->code == kDisconnected) return;
        want_connection_ = false;
        drop_connection("registration failed: " + e->message);
    });
}

void Engine::on_auth_ok() {
    vault_.set_meta("registered", "1");
    backoff_s_ = 1;
    set_conn(Conn::Syncing);
    publish_prekeys();
    request(wire::ListRoomsT{}, [this](wire::FrameT& f) {
        if (auto* list = f.body.AsRoomList())
            for (const auto& room : list->rooms)
                if (room) store_room(*room);
        if (f.body.type == wire::FrameBody_Error) return;
        wire::SyncT sync;
        for (const auto& room : vault_.rooms()) {
            auto cur = std::make_unique<wire::CursorT>();
            cur->room_id = room.room_id;
            cur->seq = room.acked_seq;
            sync.cursors.push_back(std::move(cur));
        }
        request(std::move(sync), [this](wire::FrameT& done) {
            if (done.body.type != wire::FrameBody_SyncComplete) return;
            set_conn(Conn::Live);
            pump_outbox();
        });
    });
}

void Engine::publish_prekeys() {
    if (vault_.meta("prekeys_published") == "1") return;
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
        if (f.body.type == wire::FrameBody_Ok) vault_.set_meta("prekeys_published", "1");
    });
}

void Engine::store_room(const wire::RoomInfoT& info) {
    if (info.room_id.size() != 16) return;
    std::vector<MemberRow> members;
    for (const auto& m : info.members)
        if (m && m->user_id.size() == 32) members.push_back({m->user_id, m->username});
    {
        db::Transaction tx(vault_.db());
        vault_.upsert_room(info.room_id, members);
        tx.commit();
    }
    if (auto room = vault_.room(info.room_id))
        emit({{"event", "room_updated"}, {"room", room_json(*room)}});
}

void Engine::on_room_event(const wire::RoomEventT& ev) {
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
        vault_.upsert_room(ev.room_id, {});
        auto peer = vault_.load_sessions(ev.sender_user).value_or(crypto::PeerSessions{});
        bool same_device = peer.empty() || to_bytes(peer.device_id) == ev.sender_device;
        peer.user_id = to_key32(ev.sender_user);
        peer.device_id = to_key32(ev.sender_device);
        std::optional<Bytes> plain;
        if (same_device)
            plain = crypto::decrypt(vault_.identity(), vault_, peer,
                                    context_of(ev.room_id, ev.event_id), ev.ciphertext);
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
        vault_.upsert_room(ev.room_id, {});
        vault_.insert_event(row);
        vault_.advance_cursor(ev.room_id, ev.seq);
        tx.commit();
    }
    if (decrypted) apply_state(row);
    emit({{"event", "event_received"}, {"room_id", b64(row.room_id)}, {"data", event_json(row)}});
}

void Engine::fail_outbox(const OutboxRow& row, const std::string& message) {
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
void Engine::pump_outbox() {
    if (conn_ != Conn::Live || sending_ || !vault_.unlocked()) return;
    auto queue = vault_.outbox();
    if (queue.empty()) return;
    OutboxRow row = queue.front();
    auto room = vault_.room(row.room_id);
    auto event = vault_.event(row.room_id, row.event_id);
    if (!room || !event) {
        vault_.outbox_remove(row.local_id);
        pump_outbox();
        return;
    }

    // Every other member needs a session before anything can be encrypted.
    const Key32& me = vault_.identity().user.pk;
    bool waiting = false;
    std::vector<Bytes> others;
    for (const auto& m : room->members) {
        if (m.user_id.size() != 32 || std::equal(m.user_id.begin(), m.user_id.end(), me.begin()))
            continue;
        others.push_back(m.user_id);
        auto sessions = vault_.load_sessions(m.user_id);
        if (sessions && !sessions->empty()) continue;
        waiting = true;
        if (!bundle_requested_.insert(m.user_id).second) continue;
        wire::FetchBundleT q;
        q.user_id = m.user_id;
        request(std::move(q), [this, user = m.user_id, row](wire::FrameT& f) {
            bundle_requested_.erase(user);
            auto* b = f.body.AsBundle();
            if (!b) {
                auto* e = f.body.AsError();
                if (e && e->code == kDisconnected) return;  // retried after reconnect
                fail_outbox(row, e ? e->message : "could not fetch keys");
                pump_outbox();
                return;
            }
            try {
                if (b->user_id != user || b->device_id.size() != 32 || b->dh_key.size() != 32 ||
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
                crypto::PeerSessions peer;
                crypto::start_session(vault_.identity(), bundle, peer);
                vault_.save_sessions(peer);
            } catch (const std::exception& e) {
                fail_outbox(row, std::string("could not start a secure session: ") + e.what());
            }
            pump_outbox();
        });
    }
    if (waiting) return;
    if (others.empty()) {
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
    try {
        // Persist each advanced ratchet before its ciphertext leaves the
        // process, so a crash can never reuse a message key.
        db::Transaction tx(vault_.db());
        Bytes context = context_of(row.room_id, row.event_id);
        for (const auto& user : others) {
            auto peer = vault_.load_sessions(user);
            auto r = std::make_unique<wire::RecipientT>();
            r->device_id = to_bytes(peer->device_id);
            r->ciphertext = crypto::encrypt(vault_.identity(), *peer, context, plaintext);
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
        auto* e = f.body.AsError();
        if (e && e->code == kDisconnected) return;  // stays queued, resent after reconnect
        fail_outbox(row, e ? e->message : "the server rejected the event");
        pump_outbox();
    });
}

}  // namespace corded

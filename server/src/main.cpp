// cordedd: the Corded relay server. It authenticates devices, hands out prekey
// bundles, tracks room membership, and stores and forwards ciphertext.
//
// Prototype limits: one device per user, a single I/O thread.
#include "storage.hpp"

#include "corded/common/sig.hpp"
#include "corded/common/tls.hpp"

#include <asio.hpp>
#include <spdlog/spdlog.h>

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace corded::server {

using asio::ip::tcp;

class Server;

struct Options {
    std::string host = "0.0.0.0", data_dir = "./cordedd-data", name = "corded";
    uint16_t port = 7443;
    // Who may create an account: anyone, only people with the invite code, or nobody.
    enum class Registration { Open, Invite, Closed } registration = Registration::Open;
    std::string invite_code;
    // Accounts with every permission, named by whoever runs the server.
    std::set<std::string> admins;
    // Each connection may send this many frames per second, with short bursts
    // above it. Faster senders are slowed down, not disconnected.
    double frames_per_second = 100, frame_burst = 200;
    int max_connections_per_address = 20;
};

class Conn : public std::enable_shared_from_this<Conn> {
public:
    Conn(tcp::socket sock, asio::ssl::context& tls, Server& server)
        : stream_(std::move(sock), tls), server_(server) {}

    asio::awaitable<void> run();

    void send(const wire::FrameT& frame) {
        if (closed_) return;
        out_.push_back(encode_frame(frame));
        if (!writing_) write_next();
    }
    template <typename T>
    void reply(uint32_t request_id, T&& body) {
        send(make_frame(request_id, std::forward<T>(body)));
    }
    void fail(uint32_t request_id, uint16_t code, std::string message) {
        send(error_frame(request_id, code, std::move(message)));
    }
    void close() {
        if (closed_) return;
        closed_ = true;
        asio::error_code ec;
        stream_.lowest_layer().close(ec);
    }

    // Sends what is queued, then closes.
    void close_when_flushed() {
        close_after_flush_ = true;
        if (!writing_) close();
    }

    bool authed() const { return !device_id.empty(); }

    std::string address;
    double tokens = 0;
    std::chrono::steady_clock::time_point refilled = std::chrono::steady_clock::now();
    Bytes challenge;
    Bytes tls_exporter;  // binds the sign-in signature to this TLS session
    Bytes device_id, user_id;
    std::string username;

private:
    void write_next() {
        if (out_.empty()) {
            writing_ = false;
            if (close_after_flush_) close();
            return;
        }
        writing_ = true;
        auto self = shared_from_this();
        auto buf = out_.front();
        asio::async_write(stream_, asio::buffer(*buf), [self, buf](asio::error_code ec, size_t) {
            if (ec) {
                self->close();
                return;
            }
            self->out_.pop_front();
            self->write_next();
        });
    }

    tls::Stream stream_;
    Server& server_;
    std::deque<std::shared_ptr<Bytes>> out_;
    bool writing_ = false;
    bool closed_ = false;
    bool close_after_flush_ = false;
};

class Server {
public:
    Server(asio::io_context& io, Options options)
        : io_(io),
          acceptor_(io),
          tls_(asio::ssl::context::tls_server),
          storage_(options.data_dir + "/cordedd.db"),
          name_(options.name),
          options_(std::move(options)) {
        const std::string& host = options_.host;
        const std::string& data_dir = options_.data_dir;
        uint16_t port = options_.port;
        // The server's identity is a self-signed certificate made on first
        // start. Clients pin its fingerprint.
        std::string cert = data_dir + "/tls-cert.pem", key = data_dir + "/tls-key.pem";
        if (!std::filesystem::exists(cert) || !std::filesystem::exists(key)) {
            tls::generate_self_signed(cert, key, name_);
            spdlog::info("generated a new TLS identity in {}", data_dir);
        }
        tls::require_tls13(tls_);
        tls_.use_certificate_chain_file(cert);
        tls_.use_private_key_file(key, asio::ssl::context::pem);
        fingerprint_ = b64(tls::file_fingerprint(cert));
        tcp::endpoint ep(asio::ip::make_address(host), port);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();
        spdlog::info("cordedd listening on {}:{}", host, acceptor_.local_endpoint().port());
        spdlog::info("server fingerprint: {}", fingerprint_);
        for (const auto& admin : options_.admins) {
            storage_.promote_by_username(admin);
            spdlog::info("administrator: {}", admin);
        }
        spdlog::info("registration: {}", options_.registration == Options::Registration::Open     ? "open to anyone"
                                         : options_.registration == Options::Registration::Invite ? "invite code required"
                                                                                                   : "closed");
    }

    asio::awaitable<void> accept_loop() {
        for (;;) {
            asio::error_code ec;
            tcp::socket sock =
                co_await acceptor_.async_accept(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                if (ec == asio::error::operation_aborted) co_return;
                spdlog::warn("accept failed: {}", ec.message());
                continue;
            }
            sock.set_option(tcp::no_delay(true), ec);
            std::string address = sock.remote_endpoint(ec).address().to_string();
            if (per_address_[address] >= options_.max_connections_per_address) {
                spdlog::warn("too many connections from {}", address);
                continue;  // the socket closes as it goes out of scope
            }
            ++per_address_[address];
            auto conn = std::make_shared<Conn>(std::move(sock), tls_, *this);
            conn->address = address;
            conn->tokens = options_.frame_burst;
            asio::co_spawn(io_, [conn] { return conn->run(); }, asio::detached);
        }
    }

    void stop() {
        asio::error_code ec;
        acceptor_.close(ec);
        for (auto& [id, weak] : online_)
            if (auto c = weak.lock()) c->close();
    }

    const std::string& name() const { return name_; }
    const Options& options() const { return options_; }

    void closed(Conn& c) {
        auto it = per_address_.find(c.address);
        if (it != per_address_.end() && --it->second <= 0) per_address_.erase(it);
    }

    void disconnected(Conn& c) {
        if (!c.authed()) return;
        auto it = online_.find(c.device_id);
        if (it != online_.end() && it->second.lock().get() == &c) online_.erase(it);
        spdlog::info("{} disconnected", c.username);
    }

    void handle(const std::shared_ptr<Conn>& c, wire::FrameT& f) {
        uint32_t rid = f.request_id;
        try {
            switch (f.body.type) {
                case wire::FrameBody_Register: on_register(c, rid, *f.body.AsRegister()); return;
                case wire::FrameBody_Authenticate:
                    on_authenticate(c, rid, *f.body.AsAuthenticate());
                    return;
                default: break;
            }
            if (!c->authed()) {
                c->fail(rid, err::NotAuthenticated, "authenticate first");
                return;
            }
            switch (f.body.type) {
                case wire::FrameBody_PublishPrekeys:
                    on_publish_prekeys(*c, rid, *f.body.AsPublishPrekeys());
                    break;
                case wire::FrameBody_FetchBundle: on_fetch_bundle(*c, rid, *f.body.AsFetchBundle()); break;
                case wire::FrameBody_LookupUser: on_lookup_user(*c, rid, *f.body.AsLookupUser()); break;
                case wire::FrameBody_CreateRoom: on_create_room(*c, rid, *f.body.AsCreateRoom()); break;
                case wire::FrameBody_ListRooms: on_list_rooms(*c, rid); break;
                case wire::FrameBody_AddMember: on_add_member(*c, rid, *f.body.AsAddMember()); break;
                case wire::FrameBody_LeaveRoom: on_leave_room(*c, rid, *f.body.AsLeaveRoom()); break;
                case wire::FrameBody_KickMember: on_kick_member(*c, rid, *f.body.AsKickMember()); break;
                case wire::FrameBody_BanUser: on_ban_user(*c, rid, *f.body.AsBanUser()); break;
                case wire::FrameBody_SetAdmin: on_set_admin(*c, rid, *f.body.AsSetAdmin()); break;
                case wire::FrameBody_SendRoomEvent:
                    on_send_room_event(*c, rid, *f.body.AsSendRoomEvent());
                    break;
                case wire::FrameBody_Sync: on_sync(*c, rid, *f.body.AsSync()); break;
                default: c->fail(rid, err::Malformed, "unexpected frame"); break;
            }
        } catch (const std::exception& e) {
            spdlog::error("handler failed: {}", e.what());
            c->fail(rid, err::Internal, "internal error");
        }
    }

private:
    static bool valid_username(const std::string& s) {
        if (s.empty() || s.size() > 32) return false;
        for (char ch : s)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
                return false;
        return true;
    }

    void mark_online(const std::shared_ptr<Conn>& c, const DeviceRow& dev) {
        // A new connection for a device replaces the old one.
        auto it = online_.find(dev.device_id);
        if (it != online_.end())
            if (auto old = it->second.lock(); old && old != c) old->close();
        c->device_id = dev.device_id;
        c->user_id = dev.user_id;
        c->username = dev.username;
        online_[dev.device_id] = c;
        spdlog::info("{} connected{}", dev.username, dev.access_level == kAdmin ? " (administrator)" : "");
        wire::AuthOkT ok;
        ok.user_id = dev.user_id;
        ok.username = dev.username;
        ok.is_admin = dev.access_level == kAdmin;
        ok.server_ts = now_ms();
        c->reply(0, std::move(ok));
    }

    void on_register(const std::shared_ptr<Conn>& c, uint32_t rid, const wire::RegisterT& r) {
        if (r.user_id.size() != 32 || r.device_id.size() != 32 || r.dh_key.size() != 32 ||
            !valid_username(r.username)) {
            c->fail(rid, err::Malformed, "bad registration (names are a-z, 0-9, _ and -)");
            return;
        }
        // People who already have an account may always come back; only new
        // accounts are subject to the registration policy.
        if (!storage_.find_device(r.device_id)) {
            using Reg = Options::Registration;
            bool allowed = options_.registration == Reg::Open;
            if (options_.registration == Reg::Invite)
                allowed = r.invite.size() == options_.invite_code.size() &&
                          sodium_memcmp(r.invite.data(), options_.invite_code.data(), r.invite.size()) == 0;
            if (!allowed) {
                c->fail(rid, err::RegistrationClosed,
                        options_.registration == Reg::Invite
                            ? "this server needs a valid invite code to register"
                            : "this server is not accepting new accounts");
                return;
            }
        }
        auto cert_msg = signed_message(kCtxDeviceCert, {r.device_id, r.dh_key});
        auto auth_msg = signed_message(kCtxAuth, {c->challenge, to_bytes(name_), c->tls_exporter});
        if (!verify_sig(r.user_id, cert_msg, r.cert) ||
            !verify_sig(r.device_id, auth_msg, r.signature)) {
            c->fail(rid, err::BadSignature, "signature check failed");
            return;
        }
        if (uint16_t code = storage_.register_user(r); code != 0) {
            c->fail(rid, code, "username is taken");
            return;
        }
        auto dev = storage_.find_device(r.device_id);
        if (!dev || dev->user_id != r.user_id) {
            c->fail(rid, err::Forbidden, "device belongs to another user");
            return;
        }
        if (dev->access_level == kBanned) {
            c->fail(rid, err::Forbidden, "this account is banned from the server");
            return;
        }
        // The operator named this username as an administrator.
        if (options_.admins.count(dev->username) && dev->access_level != kAdmin) {
            storage_.set_access_level(dev->user_id, kAdmin);
            dev->access_level = kAdmin;
        }
        mark_online(c, *dev);
    }

    void on_authenticate(const std::shared_ptr<Conn>& c, uint32_t rid, const wire::AuthenticateT& a) {
        auto dev = storage_.find_device(a.device_id);
        auto msg = signed_message(kCtxAuth, {c->challenge, to_bytes(name_), c->tls_exporter});
        // Same answer for an unknown device and a bad signature.
        if (!dev || !verify_sig(a.device_id, msg, a.signature)) {
            c->fail(rid, err::UnknownDevice, "authentication failed");
            return;
        }
        if (dev->access_level == kBanned) {
            c->fail(rid, err::Forbidden, "this account is banned from the server");
            return;
        }
        mark_online(c, *dev);
    }

    void on_publish_prekeys(Conn& c, uint32_t rid, const wire::PublishPrekeysT& p) {
        auto msg = signed_message(kCtxSignedPrekey, {p.spk});
        if (p.spk.size() != 32 || !verify_sig(c.device_id, msg, p.spk_sig)) {
            c.fail(rid, err::BadSignature, "bad signed prekey");
            return;
        }
        if (p.otks.size() > 200) {
            c.fail(rid, err::Malformed, "too many prekeys");
            return;
        }
        storage_.put_prekeys(c.device_id, p);
        c.reply(rid, wire::OkT{});
    }

    void on_fetch_bundle(Conn& c, uint32_t rid, const wire::FetchBundleT& q) {
        auto bundle = storage_.take_bundle(q.user_id);
        if (!bundle) {
            c.fail(rid, err::NotFound, "no keys published for that user");
            return;
        }
        c.reply(rid, std::move(*bundle));
    }

    void on_lookup_user(Conn& c, uint32_t rid, const wire::LookupUserT& q) {
        auto user = storage_.lookup_user(q.username);
        if (!user) {
            c.fail(rid, err::NotFound, "no such user");
            return;
        }
        c.reply(rid, std::move(*user));
    }

    void on_create_room(Conn& c, uint32_t rid, const wire::CreateRoomT& q) {
        std::set<Bytes> members{c.user_id};
        for (const auto& m : q.members) {
            if (!m || !storage_.user_exists(m->user_id)) {
                c.fail(rid, err::NotFound, "unknown member");
                return;
            }
            members.insert(m->user_id);
        }
        if (members.size() < 2 || members.size() > 64) {
            c.fail(rid, err::Malformed, "a room needs between 2 and 64 members");
            return;
        }
        std::vector<Bytes> list(members.begin(), members.end());
        Bytes room_id;
        bool created = true;
        if (list.size() == 2) {
            if (auto existing = storage_.find_direct_room(list[0], list[1])) {
                room_id = *existing;
                created = false;
            }
        }
        if (created) room_id = storage_.create_room(list);
        wire::RoomInfoT info = storage_.room_info(room_id);
        if (created) {
            for (const auto& m : info.members) {
                if (m->user_id == c.user_id) continue;
                if (auto dev = storage_.device_of_user(m->user_id)) push(dev->device_id, info);
            }
        }
        c.reply(rid, std::move(info));
    }

    // Sends the room's current membership to every member who is online.
    void announce_room(const wire::RoomInfoT& info, const Bytes& except_user) {
        for (const auto& m : info.members) {
            if (m->user_id == except_user) continue;
            if (auto dev = storage_.device_of_user(m->user_id)) push(dev->device_id, info);
        }
    }

    void on_add_member(Conn& c, uint32_t rid, const wire::AddMemberT& q) {
        if (!storage_.is_member(q.room_id, c.user_id)) {
            c.fail(rid, err::Forbidden, "not a member of that room");
            return;
        }
        if (storage_.is_direct(q.room_id)) {
            c.fail(rid, err::Forbidden, "a two-person chat cannot gain members; start a group instead");
            return;
        }
        if (!storage_.user_exists(q.user_id)) {
            c.fail(rid, err::NotFound, "unknown user");
            return;
        }
        storage_.add_member(q.room_id, q.user_id);
        wire::RoomInfoT info = storage_.room_info(q.room_id);
        if (info.members.size() > 64) {
            storage_.remove_member(q.room_id, q.user_id);
            c.fail(rid, err::Malformed, "a room holds at most 64 members");
            return;
        }
        announce_room(info, c.user_id);
        c.reply(rid, std::move(info));
    }

    void on_leave_room(Conn& c, uint32_t rid, const wire::LeaveRoomT& q) {
        if (!storage_.is_member(q.room_id, c.user_id)) {
            c.fail(rid, err::Forbidden, "not a member of that room");
            return;
        }
        if (storage_.is_direct(q.room_id)) {
            c.fail(rid, err::Forbidden, "a two-person chat cannot be left");
            return;
        }
        storage_.remove_member(q.room_id, c.user_id);
        announce_room(storage_.room_info(q.room_id), c.user_id);
        c.reply(rid, wire::OkT{});
    }

    // Administrator status is read from storage on every use, so removing it
    // takes effect at once.
    bool require_admin(Conn& c, uint32_t rid) {
        if (storage_.access_level(c.user_id) == kAdmin) return true;
        c.fail(rid, err::Forbidden, "only a server administrator can do that");
        return false;
    }

    void on_kick_member(Conn& c, uint32_t rid, const wire::KickMemberT& q) {
        if (!require_admin(c, rid)) return;
        if (!storage_.is_member(q.room_id, c.user_id)) {
            c.fail(rid, err::Forbidden, "you are not in that room");
            return;
        }
        if (storage_.is_direct(q.room_id) || !storage_.is_member(q.room_id, q.user_id) ||
            q.user_id == c.user_id) {
            c.fail(rid, err::Forbidden, "that person cannot be removed from this room");
            return;
        }
        storage_.remove_member(q.room_id, q.user_id);
        wire::RoomInfoT info = storage_.room_info(q.room_id);
        announce_room(info, c.user_id);
        // The removed person gets the new member list too, which no longer has them.
        if (auto dev = storage_.device_of_user(q.user_id)) push(dev->device_id, info);
        c.reply(rid, std::move(info));
    }

    void on_ban_user(Conn& c, uint32_t rid, const wire::BanUserT& q) {
        if (!require_admin(c, rid)) return;
        if (!storage_.user_exists(q.user_id)) {
            c.fail(rid, err::NotFound, "unknown user");
            return;
        }
        int level = storage_.access_level(q.user_id);
        if (q.user_id == c.user_id || level == kAdmin) {
            c.fail(rid, err::Forbidden, "an administrator cannot be banned; remove their admin status first");
            return;
        }
        storage_.set_access_level(q.user_id, q.banned ? kBanned : kUser);
        if (q.banned) {
            if (auto dev = storage_.device_of_user(q.user_id)) {
                auto it = online_.find(dev->device_id);
                if (it != online_.end())
                    if (auto conn = it->second.lock()) {
                        conn->fail(0, err::Forbidden, "this account is banned from the server");
                        conn->close_when_flushed();
                    }
            }
        }
        spdlog::info("{} {} a user", c.username, q.banned ? "banned" : "unbanned");
        c.reply(rid, wire::OkT{});
    }

    void on_set_admin(Conn& c, uint32_t rid, const wire::SetAdminT& q) {
        if (!require_admin(c, rid)) return;
        if (!storage_.user_exists(q.user_id)) {
            c.fail(rid, err::NotFound, "unknown user");
            return;
        }
        if (q.user_id == c.user_id) {
            c.fail(rid, err::Forbidden, "you cannot change your own administrator status");
            return;
        }
        if (storage_.access_level(q.user_id) == kBanned) {
            c.fail(rid, err::Forbidden, "that account is banned");
            return;
        }
        storage_.set_access_level(q.user_id, q.admin ? kAdmin : kUser);
        c.reply(rid, wire::OkT{});
    }

    void on_list_rooms(Conn& c, uint32_t rid) {
        wire::RoomListT list;
        for (const auto& room_id : storage_.rooms_of_user(c.user_id))
            list.rooms.push_back(std::make_unique<wire::RoomInfoT>(storage_.room_info(room_id)));
        c.reply(rid, std::move(list));
    }

    void on_send_room_event(Conn& c, uint32_t rid, const wire::SendRoomEventT& ev) {
        if (ev.room_id.size() != 16 || ev.event_id.size() != 16 || ev.recipients.empty()) {
            c.fail(rid, err::Malformed, "bad event");
            return;
        }
        if (!storage_.is_member(ev.room_id, c.user_id)) {
            c.fail(rid, err::Forbidden, "not a member of that room");
            return;
        }
        // Copies addressed to someone who is not (or no longer) in the room are
        // dropped: a sender may not have heard yet that a member left.
        wire::SendRoomEventT accepted;
        accepted.room_id = ev.room_id;
        accepted.event_id = ev.event_id;
        for (const auto& r : ev.recipients) {
            auto dev = r ? storage_.find_device(r->device_id) : std::nullopt;
            if (!dev || !storage_.is_member(ev.room_id, dev->user_id) || r->ciphertext.empty()) continue;
            accepted.recipients.push_back(std::make_unique<wire::RecipientT>(*r));
        }
        StoredEvent stored = storage_.store_event(accepted, c.user_id, c.device_id);
        if (!stored.existed) {
            for (const auto& r : accepted.recipients) {
                wire::RoomEventT out;
                out.room_id = ev.room_id;
                out.seq = stored.seq;
                out.event_id = ev.event_id;
                out.sender_user = c.user_id;
                out.sender_device = c.device_id;
                out.server_ts = stored.server_ts;
                out.ciphertext = r->ciphertext;
                push(r->device_id, out);
            }
        }
        wire::SendOkT ok;
        ok.room_id = ev.room_id;
        ok.event_id = ev.event_id;
        ok.seq = stored.seq;
        ok.server_ts = stored.server_ts;
        c.reply(rid, std::move(ok));
    }

    void on_sync(Conn& c, uint32_t rid, const wire::SyncT& q) {
        std::map<Bytes, uint64_t> cursors;
        for (const auto& cur : q.cursors)
            if (cur) cursors[cur->room_id] = cur->seq;
        size_t count = 0;
        for (const auto& room_id : storage_.rooms_of_user(c.user_id)) {
            uint64_t after = cursors.count(room_id) ? cursors[room_id] : 0;
            for (auto& ev : storage_.events_after(room_id, c.device_id, after)) {
                c.reply(0, std::move(ev));
                ++count;
            }
        }
        spdlog::debug("{} synced {} events", c.username, count);
        c.reply(rid, wire::SyncCompleteT{});
    }

    template <typename T>
    void push(const Bytes& device_id, const T& body) {
        auto it = online_.find(device_id);
        if (it == online_.end()) return;
        if (auto conn = it->second.lock()) conn->reply(0, T(body));
    }

    asio::io_context& io_;
    tcp::acceptor acceptor_;
    asio::ssl::context tls_;
    Storage storage_;
    std::string name_;
    Options options_;
    std::string fingerprint_;
    std::map<std::string, int> per_address_;
    std::map<Bytes, std::weak_ptr<Conn>> online_;
};

asio::awaitable<void> Conn::run() {
    auto self = shared_from_this();
    try {
        co_await stream_.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        tls_exporter = tls::exporter(stream_);
        challenge = random_bytes(32);
        wire::HelloT hello;
        hello.protocol_version = kProtocolVersion;
        hello.server_name = server_.name();
        hello.challenge = challenge;
        reply(0, std::move(hello));
        for (;;) {
            std::array<uint8_t, 4> hdr;
            co_await asio::async_read(stream_, asio::buffer(hdr), asio::use_awaitable);
            uint32_t n = decode_length(hdr.data());
            if (n == 0 || n > kMaxFrameBytes) {
                fail(0, err::Malformed, "bad frame length");
                close_after_flush_ = true;
                break;
            }
            Bytes body(n);
            co_await asio::async_read(stream_, asio::buffer(body), asio::use_awaitable);
            auto frame = decode_frame(body);
            if (!frame) {
                fail(0, err::Malformed, "malformed frame");
                close_after_flush_ = true;
                break;
            }
            // Rate limit: refill the bucket, and if it is empty wait until it is not.
            const Options& opt = server_.options();
            auto now = std::chrono::steady_clock::now();
            tokens = std::min(opt.frame_burst,
                              tokens + std::chrono::duration<double>(now - refilled).count() * opt.frames_per_second);
            refilled = now;
            if (tokens < 1.0) {
                asio::steady_timer pause(co_await asio::this_coro::executor);
                pause.expires_after(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>((1.0 - tokens) / opt.frames_per_second)));
                co_await pause.async_wait(asio::use_awaitable);
                tokens = 1.0;
                refilled = std::chrono::steady_clock::now();
            }
            tokens -= 1.0;
            server_.handle(self, *frame);
        }
    } catch (const std::exception&) {
        // Connection dropped; nothing to report.
    }
    server_.disconnected(*this);
    server_.closed(*this);
    if (!close_after_flush_ || !writing_) close();
}

}  // namespace corded::server

int main(int argc, char** argv) {
    corded::server::Options opt;
    using Reg = corded::server::Options::Registration;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") opt.port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--host") opt.host = next();
        else if (a == "--data") opt.data_dir = next();
        else if (a == "--name") opt.name = next();
        else if (a == "--invite-code") {
            opt.invite_code = next();
            opt.registration = Reg::Invite;
        } else if (a == "--closed") opt.registration = Reg::Closed;
        else if (a == "--admin") opt.admins.insert(next());
        else if (a == "--verbose") spdlog::set_level(spdlog::level::debug);
        else {
            std::fprintf(stderr,
                         "usage: cordedd [--host ADDR] [--port N] [--data DIR] [--name NAME]\n"
                         "               [--invite-code CODE | --closed] [--admin USERNAME]... [--verbose]\n\n"
                         "  --admin USERNAME    make this account a server administrator with every\n"
                         "                      permission; may be given more than once\n"
                         "  --invite-code CODE  only people who give this code can create an account\n"
                         "  --closed            nobody can create an account; existing users still sign in\n");
            return a == "--help" ? 0 : 2;
        }
    }
    if (opt.registration == Reg::Invite && opt.invite_code.size() < 8) {
        std::fprintf(stderr, "the invite code must be at least 8 characters\n");
        return 2;
    }
    try {
        if (sodium_init() < 0) throw std::runtime_error("libsodium failed to initialise");
        std::filesystem::create_directories(opt.data_dir);
        asio::io_context io(1);
        corded::server::Server server(io, opt);
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](asio::error_code, int) {
            spdlog::info("shutting down");
            server.stop();
            io.stop();
        });
        asio::co_spawn(io, server.accept_loop(), asio::detached);
        io.run();
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
    return 0;
}

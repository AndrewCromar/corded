// cordedd: the Corded relay server. It authenticates devices, hands out prekey
// bundles, tracks room membership, and stores and forwards ciphertext.
//
// Prototype limits: open registration, one device per user, a single I/O
// thread, no rate limiting.
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

    bool authed() const { return !device_id.empty(); }

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
    Server(asio::io_context& io, const std::string& host, uint16_t port, const std::string& data_dir,
           std::string name)
        : io_(io),
          acceptor_(io),
          tls_(asio::ssl::context::tls_server),
          storage_(data_dir + "/cordedd.db"),
          name_(std::move(name)) {
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
            auto conn = std::make_shared<Conn>(std::move(sock), tls_, *this);
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
        spdlog::info("{} connected", dev.username);
        wire::AuthOkT ok;
        ok.user_id = dev.user_id;
        ok.username = dev.username;
        ok.server_ts = now_ms();
        c->reply(0, std::move(ok));
    }

    void on_register(const std::shared_ptr<Conn>& c, uint32_t rid, const wire::RegisterT& r) {
        if (r.user_id.size() != 32 || r.device_id.size() != 32 || r.dh_key.size() != 32 ||
            !valid_username(r.username)) {
            c->fail(rid, err::Malformed, "bad registration (names are a-z, 0-9, _ and -)");
            return;
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
        for (const auto& r : ev.recipients) {
            auto dev = r ? storage_.find_device(r->device_id) : std::nullopt;
            if (!dev || !storage_.is_member(ev.room_id, dev->user_id) || r->ciphertext.empty()) {
                c.fail(rid, err::Forbidden, "recipient is not in the room");
                return;
            }
        }
        StoredEvent stored = storage_.store_event(ev, c.user_id, c.device_id);
        if (!stored.existed) {
            for (const auto& r : ev.recipients) {
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
    std::string fingerprint_;
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
            server_.handle(self, *frame);
        }
    } catch (const std::exception&) {
        // Connection dropped; nothing to report.
    }
    server_.disconnected(*this);
    if (!close_after_flush_ || !writing_) close();
}

}  // namespace corded::server

int main(int argc, char** argv) {
    std::string host = "0.0.0.0", data_dir = "./cordedd-data", name = "corded";
    uint16_t port = 7443;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--host") host = next();
        else if (a == "--data") data_dir = next();
        else if (a == "--name") name = next();
        else if (a == "--verbose") spdlog::set_level(spdlog::level::debug);
        else {
            std::fprintf(stderr,
                         "usage: cordedd [--host ADDR] [--port N] [--data DIR] [--name NAME] "
                         "[--verbose]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    try {
        if (sodium_init() < 0) throw std::runtime_error("libsodium failed to initialise");
        std::filesystem::create_directories(data_dir);
        asio::io_context io(1);
        corded::server::Server server(io, host, port, data_dir, name);
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

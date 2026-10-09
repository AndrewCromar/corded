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
    // The account that owns this community. If empty, the first person to
    // register becomes the owner.
    std::string owner_name;
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
        if (!options_.owner_name.empty()) {
            if (auto user = storage_.lookup_user(options_.owner_name)) storage_.set_info("owner", user->user_id);
            spdlog::info("owner: {}", options_.owner_name);
        } else if (storage_.owner().empty()) {
            spdlog::info("no owner yet: the first person to register will own this server");
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
                case wire::FrameBody_BanUser: on_ban_user(*c, rid, *f.body.AsBanUser()); break;
                case wire::FrameBody_Kick: on_kick(*c, rid, *f.body.AsKick()); break;
                case wire::FrameBody_GetMembers: on_get_members(*c, rid); break;
                case wire::FrameBody_NewRole: on_create_role(*c, rid, *f.body.AsNewRole()); break;
                case wire::FrameBody_EditRole: on_update_role(*c, rid, *f.body.AsEditRole()); break;
                case wire::FrameBody_RemoveRole: on_delete_role(*c, rid, *f.body.AsRemoveRole()); break;
                case wire::FrameBody_GrantRole: on_assign_role(*c, rid, *f.body.AsGrantRole()); break;
                case wire::FrameBody_CreateChannel: on_create_channel(*c, rid, *f.body.AsCreateChannel()); break;
                case wire::FrameBody_UpdateChannel: on_update_channel(*c, rid, *f.body.AsUpdateChannel()); break;
                case wire::FrameBody_DeleteChannel: on_delete_channel(*c, rid, *f.body.AsDeleteChannel()); break;
                case wire::FrameBody_SetOverride: on_set_override(*c, rid, *f.body.AsSetOverride()); break;
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
        // Ownership: the operator's choice wins; otherwise the first to arrive.
        bool named = !options_.owner_name.empty() && dev.username == options_.owner_name;
        if (named ? !storage_.is_owner(dev.user_id) : (options_.owner_name.empty() && storage_.owner().empty())) {
            storage_.set_info("owner", dev.user_id);
            spdlog::info("{} now owns this server", dev.username);
        }
        spdlog::info("{} connected{}", dev.username, storage_.is_owner(dev.user_id) ? " (owner)" : "");
        wire::AuthOkT ok;
        ok.user_id = dev.user_id;
        ok.username = dev.username;
        ok.is_admin = (storage_.permissions(dev.user_id) & perm::Administrator) != 0;
        ok.server_ts = now_ms();
        c->reply(0, std::move(ok));
        c->reply(0, server_info(dev.user_id));
    }

    wire::ServerInfoT server_info(const Bytes& for_user) {
        wire::ServerInfoT info;
        info.name = name_;
        info.owner = storage_.owner();
        info.my_permissions = storage_.permissions(for_user);
        for (auto& r : storage_.roles()) info.roles.push_back(std::make_unique<wire::RoleT>(std::move(r)));
        return info;
    }

    wire::RoomListT room_list(const Bytes& for_user) {
        wire::RoomListT list;
        for (const auto& room_id : storage_.rooms_of_user(for_user))
            list.rooms.push_back(std::make_unique<wire::RoomInfoT>(storage_.room_info(room_id)));
        return list;
    }

    // Roles, channels or membership changed: give everyone online a fresh
    // picture of the community and of what they can see. Simple rather than
    // clever; fine for communities of modest size.
    void broadcast_state() {
        for (auto& [device, weak] : online_) {
            auto conn = weak.lock();
            if (!conn) continue;
            conn->reply(0, server_info(conn->user_id));
            conn->reply(0, room_list(conn->user_id));
        }
    }

    void on_register(const std::shared_ptr<Conn>& c, uint32_t rid, const wire::RegisterT& r) {
        if (r.user_id.size() != 32 || r.device_id.size() != 32 || r.dh_key.size() != 32 ||
            !valid_username(r.username)) {
            c->fail(rid, err::Malformed, "bad registration (names are a-z, 0-9, _ and -)");
            return;
        }
        // People who already have an account may always come back; only new
        // accounts are subject to the registration policy.
        auto before = storage_.find_device(r.device_id);
        if (before && before->access_level == kBanned) {
            c->fail(rid, err::Forbidden, "this account is banned from the server");
            return;
        }
        // Someone who was kicked may come back, on the same terms as a newcomer.
        bool rejoining = before && before->access_level == kKicked;
        if (!before || rejoining) {
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
        if (rejoining) {
            storage_.set_access_level(dev->user_id, kUser);
            dev->access_level = kUser;
        }
        mark_online(c, *dev);
        // A new member changes who is in every channel they can see.
        if (!before || rejoining) broadcast_state();
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
        if (dev->access_level == kKicked) {
            // Their client registers again, which is how a kicked member rejoins.
            c->fail(rid, err::Kicked, "you were removed from this server");
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
            if (!m || !storage_.user_exists(m->user_id) || storage_.access_level(m->user_id) < 0) {
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
        if (storage_.kind(q.room_id) != kGroup) {
            c.fail(rid, err::Forbidden, "only a group can gain members this way");
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
        if (storage_.kind(q.room_id) != kGroup) {
            c.fail(rid, err::Forbidden, "only a group can be left this way");
            return;
        }
        storage_.remove_member(q.room_id, c.user_id);
        announce_room(storage_.room_info(q.room_id), c.user_id);
        c.reply(rid, wire::OkT{});
    }

    // ---------------------------------------------------------- community
    // Permissions are read from storage on every request, so a change takes
    // effect at once.

    bool require(Conn& c, uint32_t rid, uint64_t permission) {
        if (storage_.permissions(c.user_id) & permission) return true;
        c.fail(rid, err::Forbidden, "you do not have permission to do that");
        return false;
    }
    // Nobody acts on the owner, on themselves, or on someone of equal or higher rank.
    bool outranks(Conn& c, uint32_t rid, const Bytes& target) {
        if (target != c.user_id && !storage_.is_owner(target) &&
            storage_.rank(c.user_id) > storage_.rank(target))
            return true;
        c.fail(rid, err::Forbidden, "you cannot do that to someone of equal or higher rank");
        return false;
    }
    static bool valid_name(const std::string& s, size_t max) {
        if (s.empty() || s.size() > max) return false;
        for (unsigned char ch : s)
            if (ch < 0x20 || ch == 0x7F) return false;
        return true;
    }
    void drop_connection_of(const Bytes& user_id, const std::string& why) {
        auto dev = storage_.device_of_user(user_id);
        if (!dev) return;
        auto it = online_.find(dev->device_id);
        if (it == online_.end()) return;
        if (auto conn = it->second.lock()) {
            conn->fail(0, err::Forbidden, why);
            conn->close_when_flushed();
        }
        online_.erase(it);
    }

    void on_get_members(Conn& c, uint32_t rid) {
        wire::MemberListT list;
        for (const auto& id : storage_.member_ids())
            if (auto m = storage_.member(id)) list.members.push_back(std::move(m));
        c.reply(rid, std::move(list));
    }

    void on_create_role(Conn& c, uint32_t rid, const wire::NewRoleT& q) {
        if (!require(c, rid, perm::ManageRoles)) return;
        // Nobody can hand out a permission they do not hold themselves.
        if (!valid_name(q.name, 32) || (q.permissions & ~storage_.permissions(c.user_id)) ||
            (q.permissions & ~perm::All)) {
            c.fail(rid, err::Forbidden, "bad role name, or permissions you do not hold yourself");
            return;
        }
        for (const auto& r : storage_.roles())
            if (r.name == q.name) {
                c.fail(rid, err::NameTaken, "a role with that name already exists");
                return;
            }
        storage_.create_role(q.name, q.permissions);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    // The role must exist and rank below the person changing it.
    std::optional<wire::RoleT> manageable_role(Conn& c, uint32_t rid, uint32_t role_id, bool allow_everyone) {
        auto role = storage_.role(role_id);
        if (!role || (role->is_everyone && !allow_everyone)) {
            c.fail(rid, err::NotFound, "no such role");
            return std::nullopt;
        }
        if (!storage_.is_owner(c.user_id) && role->position >= storage_.rank(c.user_id)) {
            c.fail(rid, err::Forbidden, "that role is not below your own");
            return std::nullopt;
        }
        return role;
    }

    void on_update_role(Conn& c, uint32_t rid, const wire::EditRoleT& q) {
        if (!require(c, rid, perm::ManageRoles)) return;
        auto role = manageable_role(c, rid, q.role_id, true);
        if (!role) return;
        std::string name = role->is_everyone || q.name.empty() ? role->name : q.name;
        uint64_t mine = storage_.permissions(c.user_id);
        // Bits the editor does not hold may stay as they are but cannot be added.
        if (!valid_name(name, 32) || (q.permissions & ~perm::All) ||
            ((q.permissions & ~role->permissions) & ~mine)) {
            c.fail(rid, err::Forbidden, "bad role name, or permissions you do not hold yourself");
            return;
        }
        storage_.update_role(q.role_id, name, q.permissions);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_delete_role(Conn& c, uint32_t rid, const wire::RemoveRoleT& q) {
        if (!require(c, rid, perm::ManageRoles)) return;
        if (!manageable_role(c, rid, q.role_id, false)) return;
        storage_.delete_role(q.role_id);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_assign_role(Conn& c, uint32_t rid, const wire::GrantRoleT& q) {
        if (!require(c, rid, perm::ManageRoles)) return;
        if (!manageable_role(c, rid, q.role_id, false)) return;
        if (storage_.access_level(q.user_id) < 0 || !storage_.user_exists(q.user_id)) {
            c.fail(rid, err::NotFound, "that person is not a member of this server");
            return;
        }
        // The owner may give roles to anyone, including themselves.
        if (!storage_.is_owner(c.user_id) && !outranks(c, rid, q.user_id)) return;
        storage_.assign_role(q.user_id, q.role_id, q.assign);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_create_channel(Conn& c, uint32_t rid, const wire::CreateChannelT& q) {
        if (!require(c, rid, perm::ManageChannels)) return;
        if (!valid_username(q.name)) {
            c.fail(rid, err::Malformed, "channel names use a-z, 0-9, _ and -, up to 32 characters");
            return;
        }
        if (storage_.channel_name_taken(q.name)) {
            c.fail(rid, err::NameTaken, "a channel with that name already exists");
            return;
        }
        Bytes room_id = storage_.create_channel(q.name);
        broadcast_state();
        c.reply(rid, storage_.room_info(room_id));
    }

    bool channel_exists(Conn& c, uint32_t rid, const Bytes& room_id) {
        if (storage_.kind(room_id) == kChannel) return true;
        c.fail(rid, err::NotFound, "no such channel");
        return false;
    }

    void on_update_channel(Conn& c, uint32_t rid, const wire::UpdateChannelT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        if (!valid_username(q.name) || storage_.channel_name_taken(q.name)) {
            c.fail(rid, err::Malformed, "that channel name is not valid or is already used");
            return;
        }
        storage_.rename_channel(q.room_id, q.name);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_delete_channel(Conn& c, uint32_t rid, const wire::DeleteChannelT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        if (storage_.channels().size() <= 1) {
            c.fail(rid, err::Forbidden, "a server keeps at least one channel");
            return;
        }
        storage_.delete_channel(q.room_id);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_set_override(Conn& c, uint32_t rid, const wire::SetOverrideT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        // Exceptions are limited to what makes sense per channel.
        constexpr uint64_t allowed = perm::ViewChannel | perm::SendMessages | perm::AddReactions |
                                     perm::AttachFiles | perm::MentionEveryone | perm::ManageMessages;
        if (!storage_.role(q.role_id) || ((q.allow | q.deny) & ~allowed) || (q.allow & q.deny)) {
            c.fail(rid, err::Malformed, "unknown role or unsupported permission for a channel");
            return;
        }
        storage_.set_override(q.room_id, q.role_id, q.allow, q.deny);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_kick(Conn& c, uint32_t rid, const wire::KickT& q) {
        if (!require(c, rid, perm::KickMembers)) return;
        if (!storage_.user_exists(q.user_id) || storage_.access_level(q.user_id) < 0) {
            c.fail(rid, err::NotFound, "that person is not a member of this server");
            return;
        }
        if (!outranks(c, rid, q.user_id)) return;
        storage_.kick(q.user_id);
        drop_connection_of(q.user_id, "you were removed from this server");
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_ban_user(Conn& c, uint32_t rid, const wire::BanUserT& q) {
        if (!require(c, rid, perm::BanMembers)) return;
        if (!storage_.user_exists(q.user_id)) {
            c.fail(rid, err::NotFound, "unknown user");
            return;
        }
        if (!q.banned) {
            // Lifting a ban makes them a member again, with no roles.
            if (storage_.access_level(q.user_id) == kBanned) {
                storage_.set_access_level(q.user_id, kUser);
                broadcast_state();
            }
            c.reply(rid, wire::OkT{});
            return;
        }
        if (!outranks(c, rid, q.user_id)) return;
        storage_.kick(q.user_id);
        storage_.set_access_level(q.user_id, kBanned);
        drop_connection_of(q.user_id, "this account is banned from the server");
        spdlog::info("{} banned a member", c.username);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_list_rooms(Conn& c, uint32_t rid) {
        c.reply(rid, room_list(c.user_id));
    }

    void on_send_room_event(Conn& c, uint32_t rid, const wire::SendRoomEventT& ev) {
        if (ev.room_id.size() != 16 || ev.event_id.size() != 16) {
            c.fail(rid, err::Malformed, "bad event");
            return;
        }
        if (!storage_.is_member(ev.room_id, c.user_id)) {
            c.fail(rid, err::Forbidden, "not a member of that room");
            return;
        }
        bool channel = storage_.kind(ev.room_id) == kChannel;
        if (channel && !(storage_.permissions(c.user_id, ev.room_id) & perm::SendMessages)) {
            c.fail(rid, err::Forbidden, "you cannot post in this channel");
            return;
        }
        // A channel may have nobody else in it yet; other rooms always do.
        if (ev.recipients.empty() && !channel) {
            c.fail(rid, err::Malformed, "bad event");
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
        else if (a == "--owner") opt.owner_name = next();
        else if (a == "--verbose") spdlog::set_level(spdlog::level::debug);
        else {
            std::fprintf(stderr,
                         "usage: cordedd [--host ADDR] [--port N] [--data DIR] [--name NAME]\n"
                         "               [--invite-code CODE | --closed] [--owner USERNAME] [--verbose]\n\n"
                         "  --name NAME         what this community is called\n"
                         "  --owner USERNAME    the account that owns this server and has every\n"
                         "                      permission; without it, the first to register owns it\n"
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

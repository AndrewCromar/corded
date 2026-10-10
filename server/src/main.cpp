// cordedd: the Corded relay server. It authenticates devices, hands out prekey
// bundles, tracks room membership, and stores and forwards ciphertext.
//
// Prototype limits: one device per user, a single I/O thread.
#include "storage.hpp"
#include "update.hpp"

#include "corded/common/sig.hpp"
#include "corded/common/tls.hpp"

#include <asio.hpp>
#include <spdlog/spdlog.h>

#include <deque>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <map>
#include <memory>
#include <set>
#include <thread>

// Where this program was started from, noted before anything can replace it.
inline std::string g_program_path;

namespace corded::server {

using asio::ip::tcp;

class Server;

inline constexpr const char* kVersion = CORDED_BUILD_VERSION;

// Settings are kept in the server's database so they survive restarts and can
// be changed from the owner's client. Command-line flags set the stored value.
struct SettingSpec {
    const char* key;
    const char* fallback;
    bool owner_only;      // not even a manage_server role may change it
    bool needs_restart;
    const char* description;
};
inline constexpr SettingSpec kSettings[] = {
    {"name", "corded", false, false, "what this community is called"},
    {"description", "", false, false, "a short line about this community, shown under its name"},
    {"icon", "", false, false, "a small picture for this community (set from the app)"},
    {"scope", "machine", true, true,
     "who can reach the server: machine (this computer only), network (the local network), internet (anyone)"},
    {"registration", "open", false, false, "who may create an account: open, invite, closed"},
    {"history_sharing", "on", false, false, "whether newcomers may ask members for earlier messages: on, off"},
    {"retention_days", "30", false, false,
     "days the server keeps stored messages and files; 0 keeps them forever"},
    {"max_file_mb", "25", false, false, "the largest file a member may send, in megabytes; 0 sets no limit"},
    {"storage_limit_mb", "0", false, false,
     "how much space files may take in all, in megabytes; 0 sets no limit"},
    {"restart", "off", false, false,
     "scheduled restart: off, \"daily HH:MM\" or \"weekly <mon..sun> HH:MM\" in the server's local time"},
    {"housekeeping", "on", false, false, "hourly tidying of expired invites and old messages: on, off"},
};

struct Options {
    std::string host = "0.0.0.0", data_dir = "./cordedd-data", name = "corded";
    bool host_given = false;
    // Settings given as flags on this start; they are stored and then apply
    // on later starts too.
    std::map<std::string, std::string> flags;
    std::string scope = "machine";
    int retention_days = 30;
    std::string restart_schedule = "off";
    bool housekeeping = true;
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
    // Whether newcomers may ask existing members' clients for earlier messages.
    bool history_sharing = true;
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
    std::string presence = "online";  // what this device says: online, away, dnd or invisible
    bool presence_known = false;      // it has said so itself (older clients never do)

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
        load_settings();
        // An explicit --host wins; otherwise the scope decides where to listen.
        const std::string host = options_.host_given ? options_.host
                                                     : options_.scope == "machine" ? "127.0.0.1" : "0.0.0.0";
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
        spdlog::info("scope: {}", options_.scope == "machine"   ? "machine (only this computer can connect; "
                                                                   "use --scope network or --scope internet to widen it)"
                                  : options_.scope == "network" ? "network (only local network addresses can connect)"
                                                                : "internet (anyone can connect)");
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
            asio::ip::address remote = sock.remote_endpoint(ec).address();
            std::string address = remote.to_string();
            // Network scope: turn away anything that is not a local address.
            if (options_.scope != "internet" && !is_local_address(remote)) {
                spdlog::warn("refused {}: outside this server's scope ({})", address, options_.scope);
                continue;
            }
            int cap = options_.scope == "internet" ? std::min(options_.max_connections_per_address, 10)
                                                   : options_.max_connections_per_address;
            if (per_address_[address] >= cap) {
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

    // Every few seconds, drop stored copies of disappearing messages that are due.
    asio::awaitable<void> sweep_loop() {
        asio::steady_timer timer(io_);
        for (;;) {
            timer.expires_after(std::chrono::seconds(5));
            asio::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            try {
                if (int n = storage_.sweep_expired(); n > 0) spdlog::debug("dropped {} expired copies", n);
            } catch (const std::exception& e) {
                spdlog::warn("expiry sweep failed: {}", e.what());
            }
        }
    }

    void stop() {
        asio::error_code ec;
        acceptor_.close(ec);
        for (auto& [id, weak] : online_)
            if (auto c = weak.lock()) c->close();
    }

    const std::string& name() const { return name_; }
    bool restart_requested() const { return restart_; }

    // Loopback, private, link-local and carrier-NAT ranges count as local.
    static bool is_local_address(asio::ip::address a) {
        if (a.is_v6()) {
            auto v6 = a.to_v6();
            if (v6.is_v4_mapped()) a = asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
            else {
                auto b = v6.to_bytes();
                return v6.is_loopback() || v6.is_link_local() || (b[0] & 0xFE) == 0xFC;
            }
        }
        auto b = a.to_v4().to_bytes();
        return b[0] == 127 || b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) ||
               (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254) ||
               (b[0] == 100 && (b[1] & 0xC0) == 64);
    }

    // ------------------------------------------------------------ settings
    static const SettingSpec* spec(const std::string& key) {
        for (const auto& s : kSettings)
            if (key == s.key) return &s;
        return nullptr;
    }
    std::string setting(const std::string& key) {
        if (auto stored = storage_.info("set:" + key)) return to_string(*stored);
        const SettingSpec* s = spec(key);
        return s ? s->fallback : "";
    }
    // Returns an empty string if the value is acceptable, else what is wrong.
    static std::string validate(const std::string& key, const std::string& value) {
        auto one_of = [&](std::initializer_list<const char*> options) {
            for (const char* o : options)
                if (value == o) return true;
            return false;
        };
        if (key == "name") return (value.empty() || value.size() > 64) ? "a name is 1 to 64 characters" : "";
        if (key == "description") return value.size() > 200 ? "a description is at most 200 characters" : "";
        if (key == "icon") {
            // A small picture as base64, shrunk by the client that sets it.
            if (value.size() > 40 * 1024) return "the picture is too large; shrink it to about 128 pixels";
            if (value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
                std::string::npos)
                return "the picture must be base64";
            return "";
        }
        if (key == "scope") return one_of({"machine", "network", "internet"}) ? "" : "use machine, network or internet";
        if (key == "registration") return one_of({"open", "invite", "closed"}) ? "" : "use open, invite or closed";
        if (key == "history_sharing" || key == "housekeeping") return one_of({"on", "off"}) ? "" : "use on or off";
        if (key == "retention_days") {
            if (value.empty() || value.size() > 5 || value.find_first_not_of("0123456789") != std::string::npos)
                return "use a number of days, or 0 to keep messages forever";
            return "";
        }
        if (key == "max_file_mb" || key == "storage_limit_mb") {
            if (value.empty() || value.size() > 7 || value.find_first_not_of("0123456789") != std::string::npos)
                return "use a number of megabytes";
            return "";
        }
        if (key == "restart") return (value == "off" || parse_schedule(value)) ? "" :
                                     "use off, \"daily HH:MM\" or \"weekly <mon..sun> HH:MM\"";
        return "there is no such setting";
    }
    struct Schedule {
        int weekday = -1;  // -1 = every day; otherwise 0 = Sunday
        int hour = 0, minute = 0;
    };
    static std::optional<Schedule> parse_schedule(const std::string& text) {
        Schedule out;
        char day[8] = {0};
        if (std::sscanf(text.c_str(), "daily %d:%d", &out.hour, &out.minute) == 2) {
        } else if (std::sscanf(text.c_str(), "weekly %3s %d:%d", day, &out.hour, &out.minute) == 3) {
            static const char* days[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
            for (int i = 0; i < 7; ++i)
                if (std::string(day) == days[i]) out.weekday = i;
            if (out.weekday < 0) return std::nullopt;
        } else {
            return std::nullopt;
        }
        if (out.hour < 0 || out.hour > 23 || out.minute < 0 || out.minute > 59) return std::nullopt;
        return out;
    }
    // Reads the stored settings (after applying this start's flags) into options_.
    void load_settings() {
        // A server that already has members but predates the scope setting was
        // reachable from the network; keep it that way rather than cut people off.
        if (!storage_.info("set:scope") && storage_.member_count() > 0)
            storage_.set_info("set:scope", to_bytes(std::string("network")));
        // A new server writes its scope down at once. Otherwise the rule above
        // would take it for an old one at its first restart, once it has
        // members, and open it to the network without anyone asking.
        if (!storage_.info("set:scope")) storage_.set_info("set:scope", to_bytes(std::string("machine")));
        for (const auto& [key, value] : options_.flags) {
            if (std::string problem = validate(key, value); !problem.empty())
                throw std::runtime_error("bad value for " + key + ": " + problem);
            storage_.set_info("set:" + key, to_bytes(value));
        }
        // On the internet, strangers should not be able to register unless the
        // operator has said so.
        if (setting("scope") == "internet" && !storage_.info("set:registration"))
            storage_.set_info("set:registration", to_bytes(std::string("invite")));
        apply_settings();
    }
    void apply_settings() {
        using Reg = Options::Registration;
        name_ = setting("name");
        options_.scope = setting("scope");
        std::string reg = setting("registration");
        options_.registration = reg == "invite" ? Reg::Invite : reg == "closed" ? Reg::Closed : Reg::Open;
        options_.history_sharing = setting("history_sharing") == "on";
        options_.retention_days = std::atoi(setting("retention_days").c_str());
        options_.restart_schedule = setting("restart");
        options_.housekeeping = setting("housekeeping") == "on";
    }
    void notify_all(const std::string& message) {
        wire::NoticeT notice;
        notice.message = message;
        for (auto& [device, weak] : online_)
            if (auto conn = weak.lock()) conn->reply(0, wire::NoticeT(notice));
    }
    // Tells members, then stops; main() starts the program again.
    void restart_soon(const std::string& why, int seconds) {
        if (restart_pending_) return;
        restart_pending_ = true;
        notify_all(why);
        spdlog::info("restarting in {}s: {}", seconds, why);
        restart_timer_.expires_after(std::chrono::seconds(seconds));
        restart_timer_.async_wait([this](asio::error_code ec) {
            if (ec) return;
            restart_ = true;
            stop();
            io_.stop();
        });
    }
    // Once a minute: is a scheduled restart due? Once an hour: tidy up.
    asio::awaitable<void> maintenance_loop() {
        asio::steady_timer timer(io_);
        uint64_t last_tidy = 0;
        for (;;) {
            timer.expires_after(std::chrono::seconds(20));
            asio::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            try {
                if (options_.housekeeping && now_ms() - last_tidy > 3600 * 1000) {
                    last_tidy = now_ms();
                    storage_.housekeeping(options_.retention_days);
                    for (const auto& id : storage_.blobs_to_drop(options_.retention_days)) drop_blob(id);
                    storage_.prune_empty_rooms();
                }
                auto schedule = parse_schedule(options_.restart_schedule);
                if (!schedule) continue;
                std::time_t t = std::time(nullptr);
                std::tm local = local_time(t);
                if (local.tm_hour != schedule->hour || local.tm_min != schedule->minute ||
                    (schedule->weekday >= 0 && local.tm_wday != schedule->weekday))
                    continue;
                // Once per scheduled minute, even though we come back up inside it.
                char stamp[32];
                std::strftime(stamp, sizeof stamp, "%Y%m%d%H%M", &local);
                if (storage_.info("last_scheduled_restart").value_or(Bytes{}) == to_bytes(std::string(stamp))) continue;
                storage_.set_info("last_scheduled_restart", to_bytes(std::string(stamp)));
                restart_soon("scheduled maintenance: the server restarts in one minute; you will be reconnected", 60);
            } catch (const std::exception& e) {
                spdlog::warn("maintenance failed: {}", e.what());
            }
        }
    }
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
        announce_presence(c.user_id);
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
                case wire::FrameBody_ListDevices: on_list_devices(*c, rid, *f.body.AsListDevices()); break;
                case wire::FrameBody_RemoveDevice: on_remove_device(*c, rid, *f.body.AsRemoveDevice()); break;
                case wire::FrameBody_LookupUser: on_lookup_user(*c, rid, *f.body.AsLookupUser()); break;
                case wire::FrameBody_CreateRoom: on_create_room(*c, rid, *f.body.AsCreateRoom()); break;
                case wire::FrameBody_ListRooms: on_list_rooms(*c, rid); break;
                case wire::FrameBody_AddMember: on_add_member(*c, rid, *f.body.AsAddMember()); break;
                case wire::FrameBody_LeaveRoom: on_leave_room(*c, rid, *f.body.AsLeaveRoom()); break;
                case wire::FrameBody_BanUser: on_ban_user(*c, rid, *f.body.AsBanUser()); break;
                case wire::FrameBody_Kick: on_kick(*c, rid, *f.body.AsKick()); break;
                case wire::FrameBody_RemoveAccount: on_remove_account(*c, rid, *f.body.AsRemoveAccount()); break;
                case wire::FrameBody_SetNickname: on_set_nickname(*c, rid, *f.body.AsSetNickname()); break;
                case wire::FrameBody_Ephemeral: on_ephemeral(*c, *f.body.AsEphemeral()); break;
                case wire::FrameBody_GetMembers: on_get_members(*c, rid); break;
                case wire::FrameBody_NewRole: on_create_role(*c, rid, *f.body.AsNewRole()); break;
                case wire::FrameBody_EditRole: on_update_role(*c, rid, *f.body.AsEditRole()); break;
                case wire::FrameBody_RemoveRole: on_delete_role(*c, rid, *f.body.AsRemoveRole()); break;
                case wire::FrameBody_GrantRole: on_assign_role(*c, rid, *f.body.AsGrantRole()); break;
                case wire::FrameBody_CreateChannel: on_create_channel(*c, rid, *f.body.AsCreateChannel()); break;
                case wire::FrameBody_UpdateChannel: on_update_channel(*c, rid, *f.body.AsUpdateChannel()); break;
                case wire::FrameBody_DeleteChannel: on_delete_channel(*c, rid, *f.body.AsDeleteChannel()); break;
                case wire::FrameBody_SetOverride: on_set_override(*c, rid, *f.body.AsSetOverride()); break;
                case wire::FrameBody_GetOverrides: on_get_overrides(*c, rid, *f.body.AsGetOverrides()); break;
                case wire::FrameBody_SetChannelNsfw: on_set_channel_nsfw(*c, rid, *f.body.AsSetChannelNsfw()); break;
                case wire::FrameBody_SetChannelArchived:
                    on_set_channel_archived(*c, rid, *f.body.AsSetChannelArchived());
                    break;
                case wire::FrameBody_PutBlob: on_put_blob(*c, rid, *f.body.AsPutBlob()); break;
                case wire::FrameBody_GetBlob: on_get_blob(*c, rid, *f.body.AsGetBlob()); break;
                case wire::FrameBody_DeleteBlob: on_delete_blob(*c, rid, *f.body.AsDeleteBlob()); break;
                case wire::FrameBody_NewInvite: on_create_invite(*c, rid, *f.body.AsNewInvite()); break;
                case wire::FrameBody_RevokeInvite: on_revoke_invite(*c, rid, *f.body.AsRevokeInvite()); break;
                case wire::FrameBody_HistoryRequest: on_history_request(*c, rid, *f.body.AsHistoryRequest()); break;
                case wire::FrameBody_GetSettings: on_get_settings(*c, rid); break;
                case wire::FrameBody_SetSetting: on_set_setting(*c, rid, *f.body.AsSetSetting()); break;
                case wire::FrameBody_Restart: on_restart(*c, rid); break;
                case wire::FrameBody_UpdateServer: on_update(*c, rid); break;
                case wire::FrameBody_CancelScheduled: on_cancel_scheduled(*c, rid, *f.body.AsCancelScheduled()); break;
                case wire::FrameBody_EditSection: on_edit_section(*c, rid, *f.body.AsEditSection()); break;
                case wire::FrameBody_SetChannelSection:
                    on_set_channel_section(*c, rid, *f.body.AsSetChannelSection());
                    break;
                case wire::FrameBody_SetChannelFeatured:
                    on_set_channel_featured(*c, rid, *f.body.AsSetChannelFeatured());
                    break;
                case wire::FrameBody_GetStatus: on_get_status(*c, rid); break;
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
        info.history_sharing = options_.history_sharing;
        info.description = setting("description");
        info.icon = setting("icon");
        for (auto& r : storage_.roles()) info.roles.push_back(std::make_unique<wire::RoleT>(std::move(r)));
        for (auto& s : storage_.sections()) info.sections.push_back(std::make_unique<wire::SectionT>(std::move(s)));
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
        bool used_invite = false;
        auto before = storage_.find_device(r.device_id);
        // The person may already be a member through another device.
        bool known_user = storage_.user_exists(r.user_id);
        int level = known_user ? storage_.access_level(r.user_id) : kUser;
        if (level == kBanned) {
            c->fail(rid, err::Forbidden, "this account is banned from the server");
            return;
        }
        // Someone who was kicked may come back, on the same terms as a newcomer.
        bool rejoining = known_user && level == kKicked;
        bool new_device = known_user && !before && level >= 0;
        if (!known_user || rejoining) {
            using Reg = Options::Registration;
            bool allowed = options_.registration == Reg::Open;
            if (options_.registration == Reg::Invite) {
                // Either the fixed code from the command line, or one a member made.
                bool fixed = !options_.invite_code.empty() &&
                             r.invite.size() == options_.invite_code.size() &&
                             sodium_memcmp(r.invite.data(), options_.invite_code.data(), r.invite.size()) == 0;
                used_invite = !fixed && !r.invite.empty() && storage_.invite_valid(r.invite);
                // The named owner must be able to get in to make the first invite.
                bool founding = !options_.owner_name.empty() && r.username == options_.owner_name &&
                                storage_.owner().empty();
                allowed = fixed || used_invite || founding;
            }
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
        if (used_invite) storage_.use_invite(r.invite);
        storage_.set_device_caps(r.device_id, r.caps);
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
        if (!known_user || rejoining) broadcast_state();
        if (new_device) {
            // Everyone who sends to this person must now include the new device.
            spdlog::info("{} added a device", dev->username);
            wire::DevicesChangedT changed;
            changed.user_id = dev->user_id;
            for (auto& [device, weak] : online_)
                if (auto conn = weak.lock()) conn->reply(0, wire::DevicesChangedT(changed));
        }
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
        storage_.set_device_caps(a.device_id, a.caps);
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
        auto bundle = storage_.take_bundle(q.user_id, q.device_id);
        if (!bundle) {
            c.fail(rid, err::NotFound, "no keys published for that user");
            return;
        }
        c.reply(rid, std::move(*bundle));
    }

    void on_list_devices(Conn& c, uint32_t rid, const wire::ListDevicesT& q) {
        wire::DeviceListT list;
        list.user_id = q.user_id;
        if (storage_.access_level(q.user_id) >= 0)
            for (const auto& d : storage_.devices_of_user(q.user_id)) {
                auto ref = std::make_unique<wire::DeviceRefT>();
                ref->device_id = d.device_id;
                ref->created_at = storage_.device_created_at(d.device_id);
                ref->caps = storage_.device_caps(d.device_id);
                list.devices.push_back(std::move(ref));
            }
        c.reply(rid, std::move(list));
    }

    // Only a person's own devices, and never the one asking: that one signs
    // itself out by erasing itself.
    void on_remove_device(Conn& c, uint32_t rid, const wire::RemoveDeviceT& q) {
        auto device = storage_.find_device(q.device_id);
        if (!device || device->user_id != c.user_id) {
            c.fail(rid, err::NotFound, "that is not one of your devices");
            return;
        }
        if (q.device_id == c.device_id) {
            c.fail(rid, err::Forbidden, "this is the device you are using; sign out on it instead");
            return;
        }
        auto it = online_.find(q.device_id);
        if (it != online_.end()) {
            if (auto conn = it->second.lock()) {
                conn->fail(0, err::Forbidden, "this device was signed out from another of your devices");
                conn->close_when_flushed();
            }
            online_.erase(it);
        }
        storage_.remove_device(q.device_id);
        spdlog::info("{} removed one of their devices", c.username);
        // Everyone should stop sending to it.
        wire::DevicesChangedT changed;
        changed.user_id = c.user_id;
        for (auto& [id, weak] : online_)
            if (auto conn = weak.lock()) conn->reply(0, wire::DevicesChangedT(changed));
        c.reply(rid, wire::OkT{});
    }

    // To every device of this person that is online.
    template <typename T>
    void push_user(const Bytes& user_id, const T& body) {
        for (const auto& d : storage_.devices_of_user(user_id)) push(d.device_id, body);
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
                push_user(m->user_id, info);
            }
        }
        c.reply(rid, std::move(info));
    }

    // Sends the room's current membership to every member who is online.
    void announce_room(const wire::RoomInfoT& info, const Bytes& except_user) {
        for (const auto& m : info.members) {
            if (m->user_id == except_user) continue;
            push_user(m->user_id, info);
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
        if (storage_.kind(q.room_id) == kChannel) {
            c.fail(rid, err::Forbidden, "a channel cannot be left; its access is set by roles");
            return;
        }
        // Leaving takes the chat off the leaver's list only. Whoever is left
        // keeps it, and their copy of what was said, until they leave too.
        storage_.remove_member(q.room_id, c.user_id);
        announce_room(storage_.room_info(q.room_id), c.user_id);
        storage_.prune_empty_rooms();
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
        for (const auto& dev : storage_.devices_of_user(user_id)) {
            auto it = online_.find(dev.device_id);
            if (it == online_.end()) continue;
            if (auto conn = it->second.lock()) {
                conn->fail(0, err::Forbidden, why);
                conn->close_when_flushed();
            }
            online_.erase(it);
        }
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
        if (!q.channel_type.empty() && q.channel_type != "tasks") {
            c.fail(rid, err::Malformed, "this server does not know that kind of channel");
            return;
        }
        Bytes room_id = storage_.create_channel(q.name, q.channel_type);
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

    void on_set_channel_archived(Conn& c, uint32_t rid, const wire::SetChannelArchivedT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        storage_.set_channel_archived(q.room_id, q.archived);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    // ---- files ----
    std::string blob_path(const Bytes& id) const {
        static const char* hex = "0123456789abcdef";
        std::string name;
        for (uint8_t b : id) {
            name += hex[b >> 4];
            name += hex[b & 15];
        }
        return options_.data_dir + "/blobs/" + name;
    }
    void drop_blob(const Bytes& id) {
        std::error_code ec;
        std::filesystem::remove(blob_path(id), ec);
        storage_.blob_forget(id);
    }

    // One piece of an upload. The first piece opens the file; each later one
    // must come from the same person and continue exactly where it stopped.
    void on_put_blob(Conn& c, uint32_t rid, const wire::PutBlobT& q) {
        if (q.blob_id.size() != 16 || q.data.empty() || q.data.size() > kBlobChunkBytes || q.total == 0 ||
            q.offset + q.data.size() > q.total) {
            c.fail(rid, err::Malformed, "bad file piece");
            return;
        }
        auto row = storage_.blob(q.blob_id);
        if (!row) {
            uint64_t largest = std::strtoull(setting("max_file_mb").c_str(), nullptr, 10) * 1024 * 1024;
            uint64_t limit = std::strtoull(setting("storage_limit_mb").c_str(), nullptr, 10) * 1024 * 1024;
            if (q.offset != 0) {
                c.fail(rid, err::NotFound, "no such upload");
                return;
            }
            if (!require(c, rid, perm::AttachFiles)) return;
            // 0 means no limit, as with the other limits. Who may send files
            // at all is a permission (attach_files), not a size.
            // The encrypted form is a few bytes longer than the file itself.
            if (largest > 0 && q.total > largest + 1024) {
                c.fail(rid, err::Forbidden,
                       "that file is larger than this server allows (" + setting("max_file_mb") + " MB)");
                return;
            }
            if (limit > 0 && storage_.blob_bytes() + q.total > limit) {
                c.fail(rid, err::Forbidden, "this server has no room left for files");
                return;
            }
            std::filesystem::create_directories(options_.data_dir + "/blobs");
            uint64_t before = storage_.blob_bytes();
            storage_.blob_begin(q.blob_id, c.user_id, q.total);
            // Those who run the server hear when the space for files is nearly
            // gone: once on passing 80%, once on passing 95%.
            if (limit > 0)
                for (int mark : {95, 80})
                    if (before * 100 < limit * mark && (before + q.total) * 100 >= limit * mark) {
                        wire::NoticeT note;
                        note.message = "files now take " + std::to_string(mark) + "% of the space this server allows (" +
                                       setting("storage_limit_mb") +
                                       " MB). Raise storage_limit_mb, shorten retention_days, or delete old files.";
                        for (auto& [device, weak] : online_)
                            if (auto conn = weak.lock(); conn && (storage_.permissions(conn->user_id) & perm::ManageServer))
                                conn->reply(0, wire::NoticeT(note));
                        break;
                    }
            row = storage_.blob(q.blob_id);
        }
        if (row->owner != c.user_id || row->total != q.total || row->stored != q.offset) {
            c.fail(rid, err::Forbidden, "that piece does not continue this upload");
            return;
        }
        std::ofstream out(blob_path(q.blob_id), std::ios::binary | (q.offset == 0 ? std::ios::trunc : std::ios::app));
        out.write(reinterpret_cast<const char*>(q.data.data()), static_cast<std::streamsize>(q.data.size()));
        out.close();
        if (!out) {
            drop_blob(q.blob_id);
            c.fail(rid, err::Internal, "the server could not store the file");
            return;
        }
        storage_.blob_stored(q.blob_id, q.offset + q.data.size());
        c.reply(rid, wire::OkT{});
    }

    // Anyone signed in who knows a file's id may fetch it. The id is random
    // and only travels inside encrypted messages, and the bytes are useless
    // without the key that travels with it.
    void on_get_blob(Conn& c, uint32_t rid, const wire::GetBlobT& q) {
        auto row = q.blob_id.size() == 16 ? storage_.blob(q.blob_id) : std::nullopt;
        if (!row || !row->complete() || q.offset >= row->total) {
            c.fail(rid, err::NotFound, "that file is no longer on the server");
            return;
        }
        wire::BlobT out;
        out.blob_id = q.blob_id;
        out.offset = q.offset;
        out.total = row->total;
        out.data.resize(static_cast<size_t>(std::min<uint64_t>(kBlobChunkBytes, row->total - q.offset)));
        std::ifstream in(blob_path(q.blob_id), std::ios::binary);
        in.seekg(static_cast<std::streamoff>(q.offset));
        in.read(reinterpret_cast<char*>(out.data.data()), static_cast<std::streamsize>(out.data.size()));
        if (static_cast<size_t>(in.gcount()) != out.data.size()) {
            c.fail(rid, err::NotFound, "that file is no longer on the server");
            return;
        }
        c.reply(rid, std::move(out));
    }

    void on_delete_blob(Conn& c, uint32_t rid, const wire::DeleteBlobT& q) {
        auto row = q.blob_id.size() == 16 ? storage_.blob(q.blob_id) : std::nullopt;
        if (!row) {
            c.reply(rid, wire::OkT{});  // already gone
            return;
        }
        if (row->owner != c.user_id && !require(c, rid, perm::ManageMessages)) return;
        drop_blob(q.blob_id);
        c.reply(rid, wire::OkT{});
    }

    // Sections group channels in everyone's list. Whoever manages channels manages them.
    void on_edit_section(Conn& c, uint32_t rid, const wire::EditSectionT& q) {
        if (!require(c, rid, perm::ManageChannels)) return;
        if (q.section_id != 0 && !storage_.section_exists(q.section_id)) {
            c.fail(rid, err::NotFound, "no such section");
            return;
        }
        if (q.remove) {
            storage_.remove_section(q.section_id);
        } else {
            if (q.name.empty() || q.name.size() > 40) {
                c.fail(rid, err::Malformed, "a section's name is 1 to 40 characters");
                return;
            }
            if (q.section_id == 0) {
                if (storage_.section_count() >= 50) {
                    c.fail(rid, err::Forbidden, "a server has at most 50 sections");
                    return;
                }
                storage_.create_section(q.name);
            } else {
                storage_.update_section(q.section_id, q.name, q.position);
            }
        }
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_set_channel_section(Conn& c, uint32_t rid, const wire::SetChannelSectionT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        if (q.section_id != 0 && !storage_.section_exists(q.section_id)) {
            c.fail(rid, err::NotFound, "no such section");
            return;
        }
        storage_.set_channel_section(q.room_id, q.section_id);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_set_channel_featured(Conn& c, uint32_t rid, const wire::SetChannelFeaturedT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        storage_.set_channel_featured(q.room_id, q.featured);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    void on_set_channel_nsfw(Conn& c, uint32_t rid, const wire::SetChannelNsfwT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        storage_.set_channel_nsfw(q.room_id, q.nsfw);
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

    void on_get_overrides(Conn& c, uint32_t rid, const wire::GetOverridesT& q) {
        if (!require(c, rid, perm::ManageChannels) || !channel_exists(c, rid, q.room_id)) return;
        wire::OverridesT out;
        out.room_id = q.room_id;
        for (const auto& row : storage_.overrides(q.room_id)) {
            auto entry = std::make_unique<wire::OverrideT>();
            entry->role_id = row.role_id;
            entry->allow = row.allow;
            entry->deny = row.deny;
            out.entries.push_back(std::move(entry));
        }
        c.reply(rid, std::move(out));
    }

    void on_create_invite(Conn& c, uint32_t rid, const wire::NewInviteT& q) {
        if (!require(c, rid, perm::CreateInvite)) return;
        if (q.max_uses > 100000 || q.expires_in_s > 366ull * 24 * 3600) {
            c.fail(rid, err::Malformed, "invite limits are out of range");
            return;
        }
        wire::InviteT invite;
        invite.code = b64(random_bytes(16));
        invite.max_uses = q.max_uses;
        invite.expires_at = q.expires_in_s ? now_ms() + q.expires_in_s * 1000 : 0;
        storage_.add_invite(invite.code, c.user_id, invite.max_uses, invite.expires_at);
        c.reply(rid, std::move(invite));
    }

    void on_revoke_invite(Conn& c, uint32_t rid, const wire::RevokeInviteT& q) {
        if (!require(c, rid, perm::CreateInvite)) return;
        if (!storage_.revoke_invite(q.code)) {
            c.fail(rid, err::NotFound, "no such invite");
            return;
        }
        c.reply(rid, wire::OkT{});
    }

    // The server cannot read history, so it only carries the question to
    // members who are online. Whether they answer is up to each of them.
    void on_history_request(Conn& c, uint32_t rid, const wire::HistoryRequestT& q) {
        if (!options_.history_sharing) {
            c.fail(rid, err::Forbidden, "this server does not allow sharing earlier messages");
            return;
        }
        if (!storage_.is_member(q.room_id, c.user_id)) {
            c.fail(rid, err::Forbidden, "not a member of that room");
            return;
        }
        // A direct message has no newcomers: only the asker's own other
        // devices are asked, never the other person.
        bool direct = storage_.kind(q.room_id) == kDirect;
        wire::HistoryWantedT wanted;
        wanted.room_id = q.room_id;
        wanted.requester = c.user_id;
        wanted.limit = std::min<uint32_t>(q.limit ? q.limit : 200, 200);
        int asked = 0;
        // The asker's own other devices first: they hold exactly their history.
        std::vector<Bytes> candidates{c.user_id};
        if (!direct)
            for (const auto& m : storage_.room_info(q.room_id).members)
                if (m->user_id != c.user_id) candidates.push_back(m->user_id);
        for (const auto& user : candidates)
            for (const auto& dev : storage_.devices_of_user(user)) {
                if (asked >= 2) break;  // two answers are plenty
                if (dev.device_id == c.device_id || !online_.count(dev.device_id)) continue;
                push(dev.device_id, wanted);
                ++asked;
            }
        if (asked == 0) {
            c.fail(rid, err::NotFound, "nobody who could share earlier messages is online right now");
            return;
        }
        c.reply(rid, wire::OkT{});
    }

    void on_get_settings(Conn& c, uint32_t rid) {
        if (!require(c, rid, perm::ManageServer)) return;
        wire::SettingsT out;
        for (const auto& s : kSettings) {
            auto e = std::make_unique<wire::SettingT>();
            e->key = s.key;
            // The picture goes out with the server's info, not in the settings list.
            e->value = std::string(s.key) == "icon" ? (setting("icon").empty() ? "" : "(set)") : setting(s.key);
            e->owner_only = s.owner_only;
            e->needs_restart = s.needs_restart;
            e->description = s.description;
            out.entries.push_back(std::move(e));
        }
        c.reply(rid, std::move(out));
    }

    void on_set_setting(Conn& c, uint32_t rid, const wire::SetSettingT& q) {
        if (!require(c, rid, perm::ManageServer)) return;
        const SettingSpec* s = spec(q.key);
        if (!s) {
            c.fail(rid, err::NotFound, "there is no setting called " + q.key);
            return;
        }
        if (s->owner_only && !storage_.is_owner(c.user_id)) {
            c.fail(rid, err::Forbidden, "only the server's owner can change " + q.key);
            return;
        }
        if (std::string problem = validate(q.key, q.value); !problem.empty()) {
            c.fail(rid, err::Malformed, problem);
            return;
        }
        storage_.set_info("set:" + q.key, to_bytes(q.value));
        spdlog::info("{} set {} to {}", c.username, q.key, q.value);
        if (!s->needs_restart) {
            apply_settings();
            broadcast_state();
        }
        c.reply(rid, wire::OkT{});
        wire::NoticeT note;
        note.message = s->needs_restart ? q.key + " is now " + q.value + "; it takes effect when the server restarts (/reboot)"
                                        : q.key + " is now " + q.value;
        c.reply(0, std::move(note));
    }

    // Only the owner: this replaces the programs the server runs.
    void on_update(Conn& c, uint32_t rid) {
        if (!storage_.is_owner(c.user_id)) {
            c.fail(rid, err::Forbidden, "only the owner can update the server");
            return;
        }
        if (updating_ || restart_pending_) {
            c.fail(rid, err::Forbidden, "an update or a restart is already under way");
            return;
        }
        std::error_code ec;
        std::filesystem::path install_dir = std::filesystem::path(g_program_path).parent_path();
        if (g_program_path.empty() || !std::filesystem::exists(install_dir / "cordedd", ec)) {
            c.fail(rid, err::Internal, "this server cannot tell where its own program is");
            return;
        }
        updating_ = true;
        c.reply(rid, wire::OkT{});
        spdlog::info("{} asked for an update; looking for the newest release", c.username);
        // Downloading takes a while; it happens off the network thread.
        std::thread([this, device = c.device_id, who = c.username, install_dir,
                     work = std::filesystem::path(options_.data_dir) / "update"] {
            update::Outcome outcome = update::install_newest(kVersion, install_dir, work, CORDED_RELEASE_KEY);
            asio::post(io_, [this, device, who, outcome] {
                updating_ = false;
                spdlog::info("update: {}", outcome.message);
                if (auto it = online_.find(device); it != online_.end())
                    if (auto conn = it->second.lock()) {
                        wire::NoticeT note;
                        note.message = "update: " + outcome.message;
                        conn->reply(0, std::move(note));
                    }
                if (outcome.installed)
                    restart_soon(who + " updated the server; it restarts in a moment and you will be reconnected", 3);
            });
        }).detach();
    }

    void on_restart(Conn& c, uint32_t rid) {
        if (!require(c, rid, perm::ManageServer)) return;
        c.reply(rid, wire::OkT{});
        restart_soon(c.username + " is restarting the server; you will be reconnected in a moment", 1);
    }

    void on_get_status(Conn& c, uint32_t rid) {
        if (!require(c, rid, perm::ManageServer)) return;
        wire::StatusT st;
        st.version = kVersion;
        st.started_at = started_at_;
        st.members = storage_.member_count();
        st.online = static_cast<uint32_t>(online_.size());
        st.scope = options_.scope;
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(options_.data_dir, ec))
            if (entry.is_regular_file(ec)) st.stored_bytes += entry.file_size(ec);
        if (auto last = storage_.info("last_housekeeping")) st.last_housekeeping = std::stoull(to_string(*last));
        c.reply(rid, std::move(st));
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

    // Relayed to members who are online right now; nothing is written to disk.
    // What others are told about a person: the most present of their
    // connected devices. Invisible and not connected both read as offline.
    std::string presence_of(const Bytes& user_id) {
        int best = 0;  // 0 offline, 1 away, 2 online, 3 dnd
        for (auto& [device, weak] : online_)
            if (auto conn = weak.lock(); conn && conn->user_id == user_id) {
                int rank = conn->presence == "dnd" ? 3 : conn->presence == "online" ? 2 : conn->presence == "away" ? 1 : 0;
                best = std::max(best, rank);
            }
        static const char* names[] = {"offline", "away", "online", "dnd"};
        return names[best];
    }

    // Tells everyone connected when a person's presence changed.
    void announce_presence(const Bytes& user_id) {
        std::string now = presence_of(user_id);
        auto known = announced_.find(user_id);
        if (known != announced_.end() ? known->second == now : now == "offline") return;
        if (now == "offline") announced_.erase(user_id);
        else announced_[user_id] = now;
        wire::EphemeralT out;
        out.kind = "presence:" + now;
        out.sender_user = user_id;
        for (auto& [device, weak] : online_)
            if (auto conn = weak.lock()) conn->reply(0, wire::EphemeralT(out));
    }

    void on_presence(Conn& c, const std::string& status) {
        if (status != "online" && status != "away" && status != "dnd" && status != "invisible") return;
        c.presence = status;
        if (!c.presence_known) {
            // First word from this device: tell it where everyone else stands.
            c.presence_known = true;
            for (const auto& [user, state] : announced_) {
                wire::EphemeralT out;
                out.kind = "presence:" + state;
                out.sender_user = user;
                c.reply(0, std::move(out));
            }
        }
        announce_presence(c.user_id);
    }

    void on_ephemeral(Conn& c, const wire::EphemeralT& q) {
        if (q.kind.rfind("presence:", 0) == 0) {
            on_presence(c, q.kind.substr(9));
            return;
        }
        if (q.kind.empty() || q.kind.size() > 32 || !storage_.is_member(q.room_id, c.user_id)) return;
        wire::EphemeralT out;
        out.room_id = q.room_id;
        out.kind = q.kind;
        out.sender_user = c.user_id;
        for (const auto& m : storage_.room_info(q.room_id).members)
            for (const auto& dev : storage_.devices_of_user(m->user_id))
                if (dev.device_id != c.device_id) push(dev.device_id, out);
    }

    // Anyone may set their own display name. Setting someone else's needs
    // manage_nicknames and a higher rank than theirs.
    void on_set_nickname(Conn& c, uint32_t rid, const wire::SetNicknameT& q) {
        Bytes target = q.user_id.empty() ? c.user_id : q.user_id;
        if (target != c.user_id) {
            if (!require(c, rid, perm::ManageNicknames)) return;
            if (!storage_.user_exists(target) || storage_.access_level(target) < 0) {
                c.fail(rid, err::NotFound, "that person is not a member of this server");
                return;
            }
            if (!outranks(c, rid, target)) return;
        }
        if (!q.nickname.empty() && !valid_name(q.nickname, 32)) {
            c.fail(rid, err::Malformed, "a display name is up to 32 characters");
            return;
        }
        storage_.set_nickname(target, q.nickname);
        broadcast_state();
        c.reply(rid, wire::OkT{});
    }

    // Stronger than a kick: the account is gone and its name can be taken by
    // someone else. Needs the ban permission and a higher rank than the target.
    void on_remove_account(Conn& c, uint32_t rid, const wire::RemoveAccountT& q) {
        if (!require(c, rid, perm::BanMembers)) return;
        if (!storage_.user_exists(q.user_id)) {
            c.fail(rid, err::NotFound, "unknown user");
            return;
        }
        if (!outranks(c, rid, q.user_id)) return;
        drop_connection_of(q.user_id, "this account was removed from the server");
        storage_.remove_account(q.user_id);
        storage_.prune_empty_rooms();
        spdlog::info("{} removed an account", c.username);
        // Everyone should stop sending to the removed account's devices.
        wire::DevicesChangedT changed;
        changed.user_id = q.user_id;
        for (auto& [device, weak] : online_)
            if (auto conn = weak.lock()) conn->reply(0, wire::DevicesChangedT(changed));
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

    // Why this person may not send this event to this room now, or nothing.
    std::pair<uint16_t, std::string> may_send(const wire::SendRoomEventT& ev, const Bytes& user) {
        if (ev.room_id.size() != 16 || ev.event_id.size() != 16) return {err::Malformed, "bad event"};
        if (!storage_.is_member(ev.room_id, user)) return {err::Forbidden, "not a member of that room"};
        bool channel = storage_.kind(ev.room_id) == kChannel;
        if (channel && storage_.is_archived(ev.room_id))
            return {err::Forbidden, "this channel is archived; it can be read but not written in"};
        if (channel && !(storage_.permissions(user, ev.room_id) & perm::SendMessages))
            return {err::Forbidden, "you cannot post in this channel"};
        // A channel may have nobody else in it yet; other rooms always do.
        if (ev.recipients.empty() && !channel) return {err::Malformed, "bad event"};
        return {0, ""};
    }

    // Gives the event its place in the room and hands each device its copy.
    StoredEvent deliver(const wire::SendRoomEventT& ev, const Bytes& sender_user, const Bytes& sender_device) {
        // Copies addressed to someone who is not (or no longer) in the room are
        // dropped: a sender may not have heard yet that a member left.
        wire::SendRoomEventT accepted;
        accepted.room_id = ev.room_id;
        accepted.event_id = ev.event_id;
        accepted.expires_at = ev.expires_at;
        accepted.shared = ev.shared;
        for (const auto& r : ev.recipients) {
            auto dev = r ? storage_.find_device(r->device_id) : std::nullopt;
            if (!dev || !storage_.is_member(ev.room_id, dev->user_id) || r->ciphertext.empty()) continue;
            accepted.recipients.push_back(std::make_unique<wire::RecipientT>(*r));
        }
        StoredEvent stored = storage_.store_event(accepted, sender_user, sender_device);
        if (!stored.existed) {
            for (const auto& r : accepted.recipients) {
                wire::RoomEventT out;
                out.room_id = ev.room_id;
                out.seq = stored.seq;
                out.event_id = ev.event_id;
                out.sender_user = sender_user;
                out.sender_device = sender_device;
                out.server_ts = stored.server_ts;
                out.ciphertext = r->ciphertext;
                out.shared = ev.shared;
                push(r->device_id, out);
            }
        }
        return stored;
    }

    void on_send_room_event(Conn& c, uint32_t rid, const wire::SendRoomEventT& ev) {
        if (auto [code, why] = may_send(ev, c.user_id); code != 0) {
            c.fail(rid, code, why);
            return;
        }
        // For later: kept aside, encrypted as it came, until its time.
        if (ev.send_at > now_ms() + 2000) {
            constexpr uint64_t kFurthest = uint64_t{30} * 24 * 3600 * 1000;
            if (ev.send_at > now_ms() + kFurthest) {
                c.fail(rid, err::Malformed, "a message can be scheduled up to 30 days ahead");
                return;
            }
            if (storage_.scheduled_count(c.user_id) >= 50) {
                c.fail(rid, err::Forbidden, "you already have 50 messages waiting to be sent");
                return;
            }
            flatbuffers::FlatBufferBuilder fbb(1024);
            fbb.Finish(wire::SendRoomEvent::Pack(fbb, &ev));
            storage_.schedule_event(ev.room_id, ev.event_id, c.user_id, c.device_id, ev.send_at,
                                    ByteView(fbb.GetBufferPointer(), fbb.GetSize()));
            wire::ScheduledT waiting;
            waiting.room_id = ev.room_id;
            waiting.event_id = ev.event_id;
            waiting.send_at = ev.send_at;
            c.reply(rid, std::move(waiting));
            return;
        }
        StoredEvent stored = deliver(ev, c.user_id, c.device_id);
        wire::SendOkT ok;
        ok.room_id = ev.room_id;
        ok.event_id = ev.event_id;
        ok.seq = stored.seq;
        ok.server_ts = stored.server_ts;
        c.reply(rid, std::move(ok));
    }

    void on_cancel_scheduled(Conn& c, uint32_t rid, const wire::CancelScheduledT& q) {
        if (!storage_.cancel_scheduled(q.room_id, q.event_id, c.user_id)) {
            c.fail(rid, err::NotFound, "that message is not waiting any more; it may already have been sent");
            return;
        }
        c.reply(rid, wire::OkT{});
    }

    // Sends what has come due. The sender may be gone, may have left the room
    // or lost the right to post since; then the event is simply dropped.
    void deliver_scheduled() {
        for (const auto& row : storage_.scheduled_due(now_ms())) {
            storage_.forget_scheduled(row.room_id, row.event_id);
            flatbuffers::Verifier verifier(row.frame.data(), row.frame.size());
            if (!verifier.VerifyBuffer<wire::SendRoomEvent>()) continue;
            wire::SendRoomEventT ev;
            flatbuffers::GetRoot<wire::SendRoomEvent>(row.frame.data())->UnPackTo(&ev);
            if (may_send(ev, row.sender_user).first != 0) continue;
            StoredEvent stored = deliver(ev, row.sender_user, row.sender_device);
            wire::SendOkT ok;
            ok.room_id = ev.room_id;
            ok.event_id = ev.event_id;
            ok.seq = stored.seq;
            ok.server_ts = stored.server_ts;
            auto it = online_.find(row.sender_device);
            auto conn = it == online_.end() ? nullptr : it->second.lock();
            if (conn) conn->reply(0, std::move(ok));
            else storage_.add_pending_ack(row.sender_device, ev.room_id, ev.event_id, stored.seq, stored.server_ts);
        }
    }

public:
    // Every few seconds: is a scheduled message due?
    asio::awaitable<void> schedule_loop() {
        asio::steady_timer timer(io_);
        for (;;) {
            timer.expires_after(std::chrono::seconds(3));
            asio::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            try {
                deliver_scheduled();
            } catch (const std::exception& e) {
                spdlog::warn("scheduled delivery failed: {}", e.what());
            }
        }
    }

private:

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
        // Where this device's scheduled messages landed while it was away.
        for (auto& ack : storage_.take_pending_acks(c.device_id)) c.reply(0, std::move(ack));
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
    asio::steady_timer restart_timer_{io_};
    bool restart_pending_ = false, restart_ = false;
    bool updating_ = false;  // an update is being downloaded and checked
    uint64_t started_at_ = now_ms();
    std::map<Bytes, std::weak_ptr<Conn>> online_;
    std::map<Bytes, std::string> announced_;  // person -> the presence others were last told
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

// Runs the server until it stops. Returns true if it stopped in order to restart.
static bool run_server(const corded::server::Options& opt) {
    asio::io_context io(1);
    corded::server::Server server(io, opt);
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](asio::error_code, int) {
        spdlog::info("shutting down");
        server.stop();
        io.stop();
    });
    asio::co_spawn(io, server.accept_loop(), asio::detached);
    asio::co_spawn(io, server.sweep_loop(), asio::detached);
    asio::co_spawn(io, server.maintenance_loop(), asio::detached);
    asio::co_spawn(io, server.schedule_loop(), asio::detached);
    io.run();
    return server.restart_requested();
}


int main(int argc, char** argv) {
#ifndef _WIN32
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            g_program_path.assign(buf, static_cast<size_t>(n));
            // After an update the link reads "... (deleted)"; the path itself is what we want.
            const std::string gone = " (deleted)";
            if (g_program_path.size() > gone.size() &&
                g_program_path.compare(g_program_path.size() - gone.size(), gone.size(), gone) == 0)
                g_program_path.resize(g_program_path.size() - gone.size());
        }
    }
#endif
    corded::server::Options opt;
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
        else if (a == "--host") {
            opt.host = next();
            opt.host_given = true;
        } else if (a == "--data") opt.data_dir = next();
        else if (a == "--name") opt.flags["name"] = next();
        else if (a == "--scope") opt.flags["scope"] = next();
        else if (a == "--invite-code") {
            opt.invite_code = next();
            opt.flags["registration"] = "invite";
        } else if (a == "--invite-only") opt.flags["registration"] = "invite";
        else if (a == "--open-registration") opt.flags["registration"] = "open";
        else if (a == "--closed") opt.flags["registration"] = "closed";
        else if (a == "--no-history-sharing") opt.flags["history_sharing"] = "off";
        else if (a == "--retention-days") opt.flags["retention_days"] = next();
        else if (a == "--restart") opt.flags["restart"] = next();
        else if (a == "--owner") opt.owner_name = next();
        else if (a == "--verbose") spdlog::set_level(spdlog::level::debug);
        else {
            std::fprintf(stderr,
                         "usage: cordedd [--data DIR] [--port N] [--scope machine|network|internet]\n"
                         "               [--name NAME] [--owner USERNAME]\n"
                         "               [--invite-only | --invite-code CODE | --open-registration | --closed]\n"
                         "               [--no-history-sharing] [--retention-days N] [--restart SCHEDULE]\n"
                         "               [--host ADDR] [--verbose]\n\n"
                         "Settings given here are remembered, so they need not be repeated on later\n"
                         "starts, and most can be changed afterwards from the owner's client (/settings).\n\n"
                         "  --scope SCOPE       who can reach the server: machine (default; this computer\n"
                         "                      only), network (the local network) or internet (anyone;\n"
                         "                      registration then needs an invite unless you say otherwise)\n"
                         "  --name NAME         what this community is called\n"
                         "  --owner USERNAME    the account that owns this server and has every\n"
                         "                      permission; without it, the first to register owns it\n"
                         "  --invite-only       new accounts need an invite made by a member (/invite in the\n"
                         "                      client); the named owner can always get in\n"
                         "  --invite-code CODE  only people who give this code can create an account\n"
                         "  --closed            nobody can create an account; existing users still sign in\n"
                         "  --no-history-sharing  newcomers cannot ask members for earlier messages\n"
                         "  --retention-days N  how long stored messages are kept (default 30; 0 = forever)\n"
                         "  --restart SCHEDULE  restart regularly: \"daily 04:00\" or \"weekly sun 04:00\"\n"
                         "  --host ADDR         listen on this address instead of the one the scope implies\n");
            return a == "--help" ? 0 : 2;
        }
    }
    if (!opt.invite_code.empty() && opt.invite_code.size() < 8) {
        std::fprintf(stderr, "the invite code must be at least 8 characters\n");
        return 2;
    }
    bool restart = false;
    try {
        if (sodium_init() < 0) throw std::runtime_error("libsodium failed to initialise");
        std::filesystem::create_directories(opt.data_dir);
        restart = run_server(opt);
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
    if (restart) {
        // Everything is closed by now. Become a fresh copy of this program,
        // with the same arguments.
        spdlog::info("restarting now");
#ifdef _WIN32
        _execv(argv[0], argv);
#else
        // The program's path as it was at start, so that a newer build put in
        // its place is what comes up. Falls back to the running copy.
        if (!g_program_path.empty()) execv(g_program_path.c_str(), argv);
        execv("/proc/self/exe", argv);
#endif
        spdlog::critical("could not restart: {}", std::strerror(errno));
        return 1;
    }
    return 0;
}

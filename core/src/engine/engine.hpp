// The engine: owns the vault, the connection and all session state. One thread
// runs everything, so engine state needs no locks. Frontends talk to it only
// through commands (in) and JSON events (out).
#pragma once

#include "corded/common/frame.hpp"
#include "corded/common/tls.hpp"
#include "vault/vault.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace corded {

struct EngineConfig {
    std::string vault_dir;
    bool fast_kdf = false;  // tests only
};

class Engine {
public:
    explicit Engine(EngineConfig config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool vault_exists() const;

    // All of these return a request id at once; the outcome arrives later as a
    // `command_result` event carrying the same id.
    uint64_t vault_create(Bytes passphrase, std::string username);
    uint64_t vault_unlock(Bytes passphrase);
    uint64_t command(std::string json_text);

    // Blocks for up to timeout_ms. Returns nothing on timeout.
    std::optional<std::string> next_event(int timeout_ms);

private:
    using json = nlohmann::json;
    using Handler = std::function<void(wire::FrameT&)>;
    enum class Conn { Disconnected, Connecting, Authenticating, Syncing, Live };

    // events out
    void emit(json j);
    void ok(uint64_t req, json data = json::object());
    void fail(uint64_t req, const std::string& code, const std::string& message);
    void emit_vault_state();
    void set_conn(Conn c, const std::string& detail = "");

    // commands in
    void run_command(uint64_t req, const std::string& text);
    void cmd_connect(uint64_t req, const json& cmd);
    void cmd_start_chat(uint64_t req, const json& cmd);
    void cmd_create_room(uint64_t req, const json& cmd);
    void lookup_next(uint64_t req, std::shared_ptr<std::vector<std::string>> names,
                     std::shared_ptr<wire::CreateRoomT> create, std::string room_name);
    void create_room(uint64_t req, wire::CreateRoomT create, std::string room_name);
    void apply_state(const EventRow& e);
    void cmd_send_event(uint64_t req, const json& cmd);
    void after_unlock();

    // network
    void start_connect();
    bool check_server_identity();
    void drop_connection(const std::string& reason);
    void schedule_reconnect();
    void read_header(uint64_t gen);
    void read_body(uint64_t gen, uint32_t n);
    void write_next(uint64_t gen);
    void send_frame(const wire::FrameT& f);
    template <typename T>
    void request(T&& body, Handler handler) {
        uint32_t id = next_wire_id_++;
        if (next_wire_id_ == 0) next_wire_id_ = 1;
        pending_[id] = std::move(handler);
        send_frame(make_frame(id, std::forward<T>(body)));
    }

    // protocol
    void on_frame(wire::FrameT& f);
    void on_hello(const wire::HelloT& hello);
    void send_register();
    void on_auth_ok();
    void publish_prekeys();
    void store_room(const wire::RoomInfoT& info);
    void on_room_event(const wire::RoomEventT& ev);
    void pump_outbox();
    void fail_outbox(const OutboxRow& row, const std::string& message);

    json room_json(const RoomRow& room);
    json event_json(const EventRow& e);

    EngineConfig config_;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::context tls_ctx_;
    std::shared_ptr<tls::Stream> stream_;  // one per connection attempt
    asio::steady_timer reconnect_timer_;
    std::thread thread_;

    Vault vault_;

    // connection state (engine thread only)
    Conn conn_ = Conn::Disconnected;
    bool want_connection_ = false;
    uint64_t conn_gen_ = 0;
    int backoff_s_ = 1;
    std::string host_, port_;
    std::string server_fingerprint_;
    Bytes tls_exporter_;
    Bytes challenge_auth_msg_;
    std::array<uint8_t, 4> hdr_{};
    Bytes body_;
    std::deque<std::shared_ptr<Bytes>> out_;
    bool writing_ = false;
    uint32_t next_wire_id_ = 1;
    std::map<uint32_t, Handler> pending_;
    bool sending_ = false;
    std::set<Bytes> bundle_requested_;

    // event queue (shared with caller threads)
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> events_;
    std::atomic<uint64_t> next_request_{1};
    std::atomic<uint64_t> event_seq_{1};
};

}  // namespace corded

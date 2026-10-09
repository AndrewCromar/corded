// End-to-end test: a real cordedd process and two engines driven only through
// the public C ABI, exactly as a frontend would use them.
#include "corded/corded.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("corded-test-" + std::to_string(getpid()) + "-" + std::to_string(counter()++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    static int& counter() {
        static int c = 0;
        return c;
    }
};

bool port_open(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    close(fd);
    return ok;
}

struct Server {
    pid_t pid = -1;
    int port;
    std::string data;
    std::string extra_flag, extra_value;
    Server(int p, std::string d, std::string flag = "", std::string value = "")
        : port(p), data(std::move(d)), extra_flag(std::move(flag)), extra_value(std::move(value)) {
        start();
    }
    ~Server() { stop(); }
    void start() {
        pid = fork();
        if (pid == 0) {
            std::string port_s = std::to_string(port);
            execl(CORDEDD_PATH, "cordedd", "--host", "127.0.0.1", "--port", port_s.c_str(), "--data",
                  data.c_str(), extra_flag.empty() ? static_cast<char*>(nullptr) : extra_flag.c_str(),
                  extra_value.empty() ? static_cast<char*>(nullptr) : extra_value.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        for (int i = 0; i < 100 && !port_open(port); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(port_open(port));
    }
    void stop() {
        if (pid <= 0) return;
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
        pid = -1;
    }
};

struct Client {
    corded_engine* engine = nullptr;
    std::string vault;
    std::vector<json> seen;

    explicit Client(std::string vault_dir) : vault(std::move(vault_dir)) { open(); }
    ~Client() { close(); }

    void open() {
        corded_config cfg{};
        cfg.struct_size = sizeof cfg;
        cfg.vault_dir = vault.c_str();
        cfg.fast_kdf = 1;
        REQUIRE(corded_engine_create(&cfg, &engine) == CORDED_OK);
    }
    void close() {
        if (engine) corded_engine_destroy(engine);
        engine = nullptr;
    }

    // Reads events until one matches, or fails the test after the timeout.
    json wait(const std::string& what, const std::function<bool(const json&)>& match,
              int timeout_ms = 10000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const char* text = nullptr;
            if (corded_next_event(engine, 100, &text, nullptr) != CORDED_OK) continue;
            json ev = json::parse(text);
            corded_event_free(text);
            seen.push_back(ev);
            try {
                if (match(ev)) return ev;
            } catch (const std::exception& ex) {
                FAIL("matcher threw '" << ex.what() << "' on event: " << ev.dump());
            }
        }
        FAIL("timed out waiting for: " << what);
        return {};
    }
    // Like wait, but also satisfied by an event that has already arrived.
    json have(const std::string& what, const std::function<bool(const json&)>& match) {
        for (const auto& e : seen)
            if (match(e)) return e;
        return wait(what, match);
    }
    json result(corded_request req, int timeout_ms = 10000) {
        return wait("result of request " + std::to_string(req), [&](const json& e) {
            return e["event"] == "command_result" && e["request"] == req;
        }, timeout_ms);
    }
    json cmd(const json& c) {
        std::string text = c.dump();
        corded_request req = 0;
        REQUIRE(corded_command(engine, text.data(), text.size(), &req) == CORDED_OK);
        return result(req);
    }
    json create(const std::string& name, const std::string& pass = "correct horse") {
        corded_request req = 0;
        REQUIRE(corded_vault_create(engine, reinterpret_cast<const uint8_t*>(pass.data()),
                                    pass.size(), name.c_str(), &req) == CORDED_OK);
        return result(req);
    }
    json unlock(const std::string& pass = "correct horse") {
        corded_request req = 0;
        REQUIRE(corded_vault_unlock(engine, reinterpret_cast<const uint8_t*>(pass.data()),
                                    pass.size(), &req) == CORDED_OK);
        return result(req);
    }
    void wait_live() {
        wait("live connection", [](const json& e) {
            return e["event"] == "connection_state" && e["state"] == "live";
        });
    }
    json wait_message(const std::string& body) {
        return wait("message '" + body + "'", [&](const json& e) {
            return e["event"] == "event_received" && e["data"]["content"].value("body", "") == body &&
                   e["data"]["mine"] == false;
        });
    }
    // Like wait_message, but also satisfied by a message that already arrived
    // while the test was waiting for something else.
    json have_message(const std::string& body) {
        for (const auto& e : seen)
            if (e["event"] == "event_received" && e["data"]["content"].value("body", "") == body &&
                e["data"]["mine"] == false)
                return e;
        return wait_message(body);
    }
    json wait_sent(const std::string& event_id) {
        return wait("send confirmation", [&](const json& e) {
            return e["event"] == "event_send_status" && e["event_id"] == event_id;
        });
    }
};

bool tree_contains(const fs::path& root, const std::string& needle) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Asks the kernel for a port nobody is using.
int test_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof addr;
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    close(fd);
    return ntohs(addr.sin_port);
}

}  // namespace

TEST_CASE("two clients talk end to end through a real server") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string());
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};

    Client alice((tmp.path / "alice").string());
    Client bob((tmp.path / "bob").string());
    REQUIRE(alice.create("alice")["ok"] == true);
    REQUIRE(bob.create("bob")["ok"] == true);
    REQUIRE(alice.cmd(connect)["ok"] == true);
    REQUIRE(bob.cmd(connect)["ok"] == true);
    alice.wait_live();
    bob.wait_live();

    // Alice opens a chat with Bob by username.
    json chat = alice.cmd({{"cmd", "start_chat"}, {"username", "bob"}});
    REQUIRE(chat["ok"] == true);
    std::string room = chat["data"]["room"]["room_id"];
    REQUIRE(chat["data"]["room"]["title"] == "bob");
    REQUIRE(alice.cmd({{"cmd", "start_chat"}, {"username", "nobody"}})["ok"] == false);

    // First message: sets up the encrypted session (X3DH) on the fly.
    json sent = alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "hello bob, this is secret"}});
    REQUIRE(sent["ok"] == true);
    std::string first_id = sent["data"]["event_id"];
    REQUIRE(alice.wait_sent(first_id)["status"] == "sent");
    json got = bob.wait_message("hello bob, this is secret");
    REQUIRE(got["data"]["type"] == "m.text");
    REQUIRE(got["data"]["sender_name"] == "alice");
    REQUIRE(got["data"]["status"] == "ok");
    REQUIRE(got["data"]["event_id"] == first_id);

    // Bob replies, using the relation mechanism that threads and replies build on.
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "hi alice"}, {"reply_to", first_id}})["ok"] == true);
    json reply = alice.wait_message("hi alice");
    REQUIRE(reply["data"]["relation"]["kind"] == "reply");
    REQUIRE(reply["data"]["relation"]["target"] == first_id);

    // A reaction is just another event type with an annotation relation.
    REQUIRE(alice.cmd({{"cmd", "send_event"}, {"room_id", room}, {"type", "m.reaction"},
                       {"content", {{"key", "+1"}}},
                       {"relation", {{"kind", "annotation"}, {"target", reply["data"]["event_id"]}, {"key", "+1"}}}})["ok"] == true);
    json reaction = bob.wait("reaction", [](const json& e) {
        return e["event"] == "event_received" && e["data"]["type"] == "m.reaction" && e["data"]["mine"] == false;
    });
    REQUIRE(reaction["data"]["relation"]["key"] == "+1");

    // An event type the core has never heard of passes through untouched.
    REQUIRE(bob.cmd({{"cmd", "send_event"}, {"room_id", room}, {"type", "com.example.dice.roll"},
                     {"content", {{"sides", 20}, {"result", 17}}}, {"fallback_text", "rolled a 17"}})["ok"] == true);
    json custom = alice.wait("custom event", [](const json& e) {
        return e["event"] == "event_received" && e["data"]["type"] == "com.example.dice.roll";
    });
    REQUIRE(custom["data"]["known_type"] == false);
    REQUIRE(custom["data"]["content"]["result"] == 17);
    REQUIRE(custom["data"]["fallback_text"] == "rolled a 17");

    // Threads: messages hang off the one that started the thread.
    {
        json s = alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "shall we plan the trip?"}});
        std::string root = s["data"]["event_id"];
        bob.wait_message("shall we plan the trip?");
        REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "yes, friday?"}, {"thread", root}})["ok"] == true);
        json first = alice.wait_message("yes, friday?");
        REQUIRE(first["data"]["relation"]["kind"] == "thread");
        REQUIRE(first["data"]["relation"]["target"] == root);
        REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "friday works"}, {"thread", root}})["ok"] == true);
        bob.wait_message("friday works");
        // The first message reports how many replies its thread has.
        bob.wait("thread count", [&](const json& e) {
            return e["event"] == "event_updated" && e["data"]["event_id"] == root && e["data"]["thread_count"] == 2;
        });
        for (Client* c : {&alice, &bob}) {
            json th = c->cmd({{"cmd", "fetch_thread"}, {"room_id", room}, {"event_id", root}});
            REQUIRE(th["ok"] == true);
            REQUIRE(th["data"]["root"]["content"]["body"] == "shall we plan the trip?");
            REQUIRE(th["data"]["root"]["thread_count"] == 2);
            REQUIRE(th["data"]["thread"].size() == 2);
            REQUIRE(th["data"]["thread"][0]["content"]["body"] == "yes, friday?");
            REQUIRE(th["data"]["thread"][1]["content"]["body"] == "friday works");
        }
    }

    // Edits and deletions: only the original sender can change a message.
    {
        json s = alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "a typo hre"}});
        std::string id = s["data"]["event_id"];
        bob.wait_message("a typo hre");
        auto updated = [&](const json& e) { return e["event"] == "event_updated" && e["data"]["event_id"] == id; };

        // Bob tries to rewrite Alice's message. Nothing changes for anyone.
        REQUIRE(bob.cmd({{"cmd", "edit_event"}, {"room_id", room}, {"event_id", id}, {"body", "forged"}})["ok"] == true);
        REQUIRE(bob.cmd({{"cmd", "delete_event"}, {"room_id", room}, {"event_id", id}})["ok"] == true);

        REQUIRE(alice.cmd({{"cmd", "edit_event"}, {"room_id", room}, {"event_id", id}, {"body", "a typo here"}})["ok"] == true);
        json edited = bob.wait("edit", updated);
        REQUIRE(edited["data"]["content"]["body"] == "a typo here");
        REQUIRE(edited["data"]["edited"] == true);
        REQUIRE(edited["data"]["status"] == "ok");
        for (const auto& e : alice.seen)
            if (e["event"] == "event_updated" && e["data"]["event_id"] == id)
                REQUIRE(e["data"]["content"]["body"] != "forged");

        REQUIRE(alice.cmd({{"cmd", "delete_event"}, {"room_id", room}, {"event_id", id}})["ok"] == true);
        json gone = bob.wait("deletion", [&](const json& e) { return updated(e) && e["data"]["status"] == "redacted"; });
        REQUIRE(gone["data"]["content"].empty());
        // It stays deleted in history too.
        json tl = bob.cmd({{"cmd", "fetch_timeline"}, {"room_id", room}, {"limit", 500}});
        for (const auto& e : tl["data"]["events"])
            if (e["event_id"] == id) {
                REQUIRE(e["status"] == "redacted");
                REQUIRE(e["content"].empty());
            }
    }

    // Both people see the same safety number, and marking it checked sticks.
    {
        json a = alice.cmd({{"cmd", "safety_numbers"}, {"room_id", room}});
        json b = bob.cmd({{"cmd", "safety_numbers"}, {"room_id", room}});
        REQUIRE(a["data"]["safety_numbers"].size() == 1);
        REQUIRE(a["data"]["safety_numbers"][0]["username"] == "bob");
        REQUIRE(a["data"]["safety_numbers"][0]["verified"] == false);
        REQUIRE(a["data"]["safety_numbers"][0]["safety_number"] == b["data"]["safety_numbers"][0]["safety_number"]);
        REQUIRE(alice.cmd({{"cmd", "set_verified"}, {"user_id", a["data"]["safety_numbers"][0]["user_id"]}})["ok"] == true);
        REQUIRE(alice.cmd({{"cmd", "safety_numbers"}, {"room_id", room}})["data"]["safety_numbers"][0]["verified"] == true);
        REQUIRE(bob.cmd({{"cmd", "safety_numbers"}, {"room_id", room}})["data"]["safety_numbers"][0]["verified"] == false);
    }

    // A burst in both directions keeps its order.
    for (int i = 0; i < 20; ++i) {
        REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "a" + std::to_string(i)}})["ok"] == true);
        REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "b" + std::to_string(i)}})["ok"] == true);
    }
    bob.wait_message("a19");
    alice.wait_message("b19");
    {
        json tl = bob.cmd({{"cmd", "fetch_timeline"}, {"room_id", room}, {"limit", 500}});
        std::vector<std::string> from_alice;
        for (const auto& e : tl["data"]["events"])
            if (e["type"] == "m.text" && e["mine"] == false && e["status"] == "ok" &&
                e["content"].value("body", "x")[0] == 'a' && e["content"].value("body", "").size() <= 3)
                from_alice.push_back(e["content"]["body"]);
        REQUIRE(from_alice.size() == 20);
        for (int i = 0; i < 20; ++i) REQUIRE(from_alice[static_cast<size_t>(i)] == "a" + std::to_string(i));
    }

    SECTION("messages sent while the recipient is offline arrive on return") {
        bob.close();
        std::string last_id;
        for (int i = 0; i < 5; ++i) {
            json s = alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "while away " + std::to_string(i)}});
            last_id = s["data"]["event_id"];
        }
        REQUIRE(alice.wait_sent(last_id)["status"] == "sent");
        bob.open();
        REQUIRE(bob.unlock()["ok"] == true);  // reconnects to the saved server on its own
        for (int i = 0; i < 5; ++i) bob.wait_message("while away " + std::to_string(i));
        // And the session still works afterwards, in both directions.
        REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "i am back"}})["ok"] == true);
        alice.wait_message("i am back");
    }

    SECTION("a server restart loses nothing and queued messages go out") {
        server.stop();
        alice.wait("disconnect", [](const json& e) {
            return e["event"] == "connection_state" && e["state"] == "disconnected";
        });
        json queued = alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "sent during the outage"}});
        REQUIRE(queued["ok"] == true);
        server.start();
        bob.wait_message("sent during the outage");
        REQUIRE(alice.wait_sent(queued["data"]["event_id"])["status"] == "sent");
    }

    SECTION("nothing readable is stored on the server or in the vault files") {
        alice.close();
        bob.close();
        server.stop();
        for (const char* needle : {"hello bob, this is secret", "hi alice", "rolled a 17", "dice.roll", "m.text",
                                   "a typo"}) {
            INFO("looking for: " << needle);
            REQUIRE_FALSE(tree_contains(tmp.path / "server", needle));
            REQUIRE_FALSE(tree_contains(tmp.path / "alice", needle));
            REQUIRE_FALSE(tree_contains(tmp.path / "bob", needle));
        }
        // Sanity check of the scanner itself: usernames are public on the server.
        REQUIRE(tree_contains(tmp.path / "server", "alice"));
    }
}

TEST_CASE("the vault rejects a wrong passphrase and reopens with the right one") {
    TempDir tmp;
    std::string dir = (tmp.path / "v").string();
    std::string user_id;
    {
        Client c(dir);
        REQUIRE(c.create("carol", "open sesame")["ok"] == true);
        json st = c.cmd({{"cmd", "status"}});
        user_id = st["data"]["user_id"];
        REQUIRE(st["data"]["vault"] == "unlocked");
    }
    {
        Client c(dir);
        json bad = c.unlock("not the passphrase");
        REQUIRE(bad["ok"] == false);
        REQUIRE(bad["error"]["code"] == "wrong_passphrase");
        REQUIRE(c.cmd({{"cmd", "list_rooms"}})["error"]["code"] == "vault_locked");
        REQUIRE(c.unlock("open sesame")["ok"] == true);
        json st = c.cmd({{"cmd", "status"}});
        REQUIRE(st["data"]["user_id"] == user_id);
        REQUIRE(st["data"]["username"] == "carol");
    }
}

TEST_CASE("a second user cannot take an existing username") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string());
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client first((tmp.path / "one").string());
    Client second((tmp.path / "two").string());
    REQUIRE(first.create("dave")["ok"] == true);
    REQUIRE(first.cmd(connect)["ok"] == true);
    first.wait_live();
    REQUIRE(second.create("dave")["ok"] == true);
    REQUIRE(second.cmd(connect)["ok"] == true);
    json state = second.wait("registration failure", [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" && e.contains("detail");
    });
    REQUIRE(state["detail"].get<std::string>().find("taken") != std::string::npos);
}

TEST_CASE("a client refuses a server whose identity has changed") {
    TempDir tmp;
    int port = test_port();
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client erin((tmp.path / "erin").string());
    REQUIRE(erin.create("erin")["ok"] == true);
    std::string pinned;
    {
        Server genuine(port, (tmp.path / "genuine").string());
        REQUIRE(erin.cmd(connect)["ok"] == true);
        erin.wait_live();
        for (const auto& e : erin.seen)
            if (e["event"] == "server_pinned") pinned = e["fingerprint"];
        REQUIRE(pinned.size() == 43);
    }
    // Something else now answers on the same address, with a different key.
    Server impostor(port, (tmp.path / "impostor").string());
    json refused = erin.wait("refusal", [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("identity") != std::string::npos;
    }, 20000);
    REQUIRE(refused["detail"].get<std::string>().find(pinned) != std::string::npos);

    // A wrong fingerprint given up front is refused on the first connection too.
    Client frank((tmp.path / "frank").string());
    REQUIRE(frank.create("frank")["ok"] == true);
    json with_pin = connect;
    with_pin["fingerprint"] = pinned;
    REQUIRE(frank.cmd(with_pin)["ok"] == true);
    frank.wait("refusal", [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("identity") != std::string::npos;
    });
}

TEST_CASE("three people share a group room") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string());
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string()),
        carol((tmp.path / "carol").string());
    for (auto [client, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}, {&carol, "carol"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        client->wait_live();
    }

    json made = alice.cmd({{"cmd", "create_room"}, {"usernames", {"bob", "carol"}}, {"name", "the secret plan"}});
    REQUIRE(made["ok"] == true);
    std::string room = made["data"]["room"]["room_id"];
    REQUIRE(made["data"]["room"]["is_group"] == true);
    REQUIRE(made["data"]["room"]["members"].size() == 3);
    REQUIRE(alice.cmd({{"cmd", "create_room"}, {"usernames", {"bob", "nobody"}}})["ok"] == false);

    // The name reaches the others as an encrypted event.
    auto named = [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == room &&
               e["room"]["title"] == "the secret plan";
    };
    bob.wait("room name", named);
    carol.wait("room name", named);

    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "hello everyone"}})["ok"] == true);
    REQUIRE(bob.wait_message("hello everyone")["data"]["sender_name"] == "alice");
    REQUIRE(carol.wait_message("hello everyone")["data"]["sender_name"] == "alice");

    // Bob and Carol have never talked; their session is set up on demand.
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "bob here"}})["ok"] == true);
    alice.wait_message("bob here");
    REQUIRE(carol.wait_message("bob here")["data"]["sender_name"] == "bob");
    REQUIRE(carol.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "carol here"}})["ok"] == true);
    alice.wait_message("carol here");
    bob.wait_message("carol here");

    // Everyone ends up with the same history in the same order.
    for (int i = 0; i < 10; ++i) {
        alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "a" + std::to_string(i)}});
        bob.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "b" + std::to_string(i)}});
        carol.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "c" + std::to_string(i)}});
    }
    alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "last one"}});
    for (Client* c : {&alice, &bob, &carol})
        for (const char* body : {"a9", "b9", "c9", "last one"})
            if (!(c == &alice && (body[0] == 'a' || body[0] == 'l')) && !(c == &bob && body[0] == 'b') &&
                !(c == &carol && body[0] == 'c'))
                c->have_message(body);
    // Wait until nobody has anything pending, then insist nothing went wrong.
    for (Client* c : {&alice, &bob, &carol}) {
        std::string problems;
        for (int attempt = 0; attempt < 100; ++attempt) {
            problems.clear();
            json tl = c->cmd({{"cmd", "fetch_timeline"}, {"room_id", room}, {"limit", 500}});
            for (const auto& e : tl["data"]["events"])
                if (e["status"] != "ok")
                    problems += e["status"].get<std::string>() + ":" + e["type"].get<std::string>() + " ";
            if (problems.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        INFO("events that never settled: " << problems);
        REQUIRE(problems.empty());
    }
    auto bodies = [&](Client& c) {
        std::vector<std::string> out;
        json tl = c.cmd({{"cmd", "fetch_timeline"}, {"room_id", room}, {"limit", 500}});
        for (const auto& e : tl["data"]["events"])
            if (e["type"] == "m.text") out.push_back(e["content"]["body"]);
        return out;
    };
    auto a = bodies(alice);
    REQUIRE(a.size() == 34);
    REQUIRE(bodies(bob) == a);
    REQUIRE(bodies(carol) == a);

    REQUIRE_FALSE(tree_contains(tmp.path / "server", "hello everyone"));
    REQUIRE_FALSE(tree_contains(tmp.path / "server", "the secret plan"));
}

TEST_CASE("an invite-only server turns away people without the code") {
    TempDir tmp;
    int port = test_port();
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    auto refused = [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("registration failed") != std::string::npos;
    };
    Client member((tmp.path / "member").string());
    REQUIRE(member.create("member")["ok"] == true);
    {
        Server server(port, (tmp.path / "server").string(), "--invite-code", "let-me-in-please");
        Client stranger((tmp.path / "stranger").string());
        REQUIRE(stranger.create("stranger")["ok"] == true);
        REQUIRE(stranger.cmd(connect)["ok"] == true);
        REQUIRE(stranger.wait("refusal", refused)["detail"].get<std::string>().find("invite") != std::string::npos);

        json wrong = connect;
        wrong["invite"] = "let-me-in-pleasf";
        REQUIRE(stranger.cmd(wrong)["ok"] == true);
        stranger.wait("refusal", refused);

        json right = connect;
        right["invite"] = "let-me-in-please";
        REQUIRE(member.cmd(right)["ok"] == true);
        member.wait_live();
    }
    // Closed to new accounts, but the existing member still gets in.
    Server closed(port, (tmp.path / "server").string(), "--closed");
    member.wait_live();
    Client late((tmp.path / "late").string());
    REQUIRE(late.create("late")["ok"] == true);
    REQUIRE(late.cmd(connect)["ok"] == true);
    late.wait("refusal", refused);
}

TEST_CASE("people can be added to a group and can leave it") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string());
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string()),
        carol((tmp.path / "carol").string()), dave((tmp.path / "dave").string());
    for (auto [client, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}, {&carol, "carol"}, {&dave, "dave"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        client->wait_live();
    }
    json made = alice.cmd({{"cmd", "create_room"}, {"usernames", {"bob", "carol"}}});
    std::string room = made["data"]["room"]["room_id"];
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "before dave joined"}})["ok"] == true);
    bob.wait_message("before dave joined");
    carol.wait_message("before dave joined");

    // Bob (not the creator) adds Dave. Everyone learns the new membership.
    REQUIRE(bob.cmd({{"cmd", "add_member"}, {"room_id", room}, {"username", "dave"}})["ok"] == true);
    auto four_members = [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == room && e["room"]["members"].size() == 4;
    };
    alice.wait("membership", four_members);
    carol.wait("membership", four_members);
    dave.wait("membership", four_members);
    json notice = alice.wait("join notice", [](const json& e) {
        return e["event"] == "event_received" && e["data"]["type"] == "m.room.member";
    });
    REQUIRE(notice["data"]["content"]["username"] == "dave");

    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "welcome dave"}})["ok"] == true);
    dave.wait_message("welcome dave");
    REQUIRE(dave.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "thanks all"}})["ok"] == true);
    alice.wait_message("thanks all");
    bob.wait_message("thanks all");
    carol.wait_message("thanks all");
    // Dave has nothing from before he joined.
    json dave_tl = dave.cmd({{"cmd", "fetch_timeline"}, {"room_id", room}, {"limit", 500}});
    for (const auto& e : dave_tl["data"]["events"]) REQUIRE(e["content"].value("body", "") != "before dave joined");

    // Carol leaves. She stops receiving; the others carry on.
    REQUIRE(carol.cmd({{"cmd", "leave_room"}, {"room_id", room}})["ok"] == true);
    carol.have("room removed", [&](const json& e) { return e["event"] == "room_removed" && e["room_id"] == room; });
    auto three_members = [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == room && e["room"]["members"].size() == 3;
    };
    alice.wait("membership", three_members);
    bob.wait("membership", three_members);
    dave.wait("membership", three_members);
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "after carol left"}})["ok"] == true);
    bob.wait_message("after carol left");
    dave.wait_message("after carol left");
    REQUIRE(carol.cmd({{"cmd", "list_rooms"}})["data"]["rooms"].empty());
    REQUIRE(carol.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "am i still here"}})["ok"] == false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (const auto& e : carol.seen)
        if (e["event"] == "event_received") REQUIRE(e["data"]["content"].value("body", "") != "after carol left");

    // Two-person chats cannot gain members or be left.
    json dm = alice.cmd({{"cmd", "start_chat"}, {"username", "bob"}});
    std::string dm_room = dm["data"]["room"]["room_id"];
    REQUIRE(alice.cmd({{"cmd", "add_member"}, {"room_id", dm_room}, {"username", "dave"}})["ok"] == false);
    REQUIRE(alice.cmd({{"cmd", "leave_room"}, {"room_id", dm_room}})["ok"] == false);
}

TEST_CASE("the account named at server start is an administrator") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--admin", "alice");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string()),
        carol((tmp.path / "carol").string()), dave((tmp.path / "dave").string());
    auto account = [](const json& e) { return e["event"] == "account"; };
    for (auto [client, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}, {&carol, "carol"}, {&dave, "dave"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        // Only the named account is told it is an administrator.
        REQUIRE(client->wait("account", account)["is_admin"] == (std::string(name) == "alice"));
        client->have("live", [](const json& e) { return e["event"] == "connection_state" && e["state"] == "live"; });
    }
    json made = alice.cmd({{"cmd", "create_room"}, {"usernames", {"bob", "carol", "dave"}}});
    std::string room = made["data"]["room"]["room_id"];
    for (const auto& m : made["data"]["room"]["members"]) REQUIRE(m["is_admin"] == (m["username"] == "alice"));
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "hello all"}})["ok"] == true);
    for (Client* c : {&bob, &carol, &dave}) c->wait_message("hello all");

    // An ordinary member is refused every administrator action.
    for (const json& attempt : {json{{"cmd", "kick_member"}, {"room_id", room}, {"username", "carol"}},
                                json{{"cmd", "ban_user"}, {"username", "carol"}},
                                json{{"cmd", "set_admin"}, {"username", "bob"}},
                                json{{"cmd", "set_admin"}, {"username", "carol"}}}) {
        json r = bob.cmd(attempt);
        REQUIRE(r["ok"] == false);
        REQUIRE(r["error"]["message"].get<std::string>().find("administrator") != std::string::npos);
    }

    // The administrator removes Carol from the room.
    REQUIRE(alice.cmd({{"cmd", "kick_member"}, {"room_id", room}, {"username", "carol"}})["ok"] == true);
    json removed = carol.wait("removal", [&](const json& e) { return e["event"] == "room_removed" && e["room_id"] == room; });
    REQUIRE(removed["reason"] == "removed");
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", room}, {"body", "carol is gone"}})["ok"] == true);
    bob.wait_message("carol is gone");
    dave.wait_message("carol is gone");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (const auto& e : carol.seen)
        if (e["event"] == "event_received") REQUIRE(e["data"]["content"].value("body", "") != "carol is gone");

    // The administrator bans Bob: he is disconnected and cannot sign back in.
    REQUIRE(alice.cmd({{"cmd", "ban_user"}, {"username", "bob"}})["ok"] == true);
    json banned = bob.wait("ban", [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("banned") != std::string::npos;
    }, 20000);
    REQUIRE(banned["detail"].get<std::string>().find("sign-in failed") != std::string::npos);
    // Unbanned, he can connect again.
    REQUIRE(alice.cmd({{"cmd", "ban_user"}, {"username", "bob"}, {"banned", false}})["ok"] == true);
    REQUIRE(bob.cmd(connect)["ok"] == true);
    bob.wait_live();

    // Administrators can make others administrators, but cannot be banned.
    REQUIRE(alice.cmd({{"cmd", "set_admin"}, {"username", "dave"}})["ok"] == true);
    REQUIRE(dave.cmd({{"cmd", "ban_user"}, {"username", "alice"}})["ok"] == false);
    REQUIRE(dave.cmd({{"cmd", "kick_member"}, {"room_id", room}, {"username", "bob"}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "set_admin"}, {"username", "dave"}, {"admin", false}})["ok"] == true);
    REQUIRE(dave.cmd({{"cmd", "ban_user"}, {"username", "bob"}})["ok"] == false);
    REQUIRE(alice.cmd({{"cmd", "set_admin"}, {"username", "alice"}, {"admin", false}})["ok"] == false);
}

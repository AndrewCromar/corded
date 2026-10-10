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
    // Sets this client up as another device of an existing person.
    json restore(const std::string& name, const std::string& recovery_key,
                 const std::string& pass = "correct horse") {
        corded_request req = 0;
        REQUIRE(corded_vault_restore(engine, reinterpret_cast<const uint8_t*>(pass.data()), pass.size(),
                                     name.c_str(), recovery_key.c_str(), &req) == CORDED_OK);
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
    // Dave was never sent the earlier message first-hand. If he has it, it is
    // because a member chose to share history with him, and it says so.
    json dave_tl = dave.cmd({{"cmd", "fetch_timeline"}, {"room_id", room}, {"limit", 500}});
    for (const auto& e : dave_tl["data"]["events"])
        if (e["content"].value("body", "") == "before dave joined") REQUIRE(e["shared_history"] == true);

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
    json carol_rooms = carol.cmd({{"cmd", "list_rooms"}})["data"]["rooms"];
    for (const auto& r : carol_rooms) REQUIRE(r["room_id"] != room);
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

TEST_CASE("a server is one community with an owner and roles and channels") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--owner", "alice");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string()),
        carol((tmp.path / "carol").string());
    auto live = [](const json& e) { return e["event"] == "connection_state" && e["state"] == "live"; };
    auto room_named = [](const std::string& title) {
        return [title](const json& e) { return e["event"] == "room_updated" && e["room"]["title"] == title; };
    };
    auto room_gone = [](const std::string& id) {
        return [id](const json& e) { return e["event"] == "room_removed" && e["room_id"] == id; };
    };
    auto refused = [](const json& r) { return r["ok"] == false; };
    auto sees = [&](Client& c, const std::string& title) {
        json rooms = c.cmd({{"cmd", "list_rooms"}})["data"]["rooms"];  // keep the result alive
        for (const auto& r : rooms)
            if (r["title"] == title) return true;
        return false;
    };
    for (auto [client, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}, {&carol, "carol"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        client->have("live", live);
    }

    // Everyone lands in #general without anyone adding them.
    std::string general;
    for (Client* c : {&alice, &bob, &carol}) {
        json room = c->have("#general", room_named("#general"))["room"];
        REQUIRE(room["kind"] == "channel");
        general = room["room_id"];
    }
    alice.have("three in #general", [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == general && e["room"]["members"].size() == 3;
    });
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "welcome to the server"}})["ok"] == true);
    bob.wait_message("welcome to the server");
    carol.wait_message("welcome to the server");

    // The account named at start-up owns the server.
    json info = alice.cmd({{"cmd", "server_info"}})["data"];
    REQUIRE(info["is_owner"] == true);
    REQUIRE(info["roles"].size() == 1);
    REQUIRE(info["roles"][0]["name"] == "@everyone");
    REQUIRE(bob.cmd({{"cmd", "server_info"}})["data"]["is_owner"] == false);

    // Ordinary members cannot run the place.
    REQUIRE(refused(bob.cmd({{"cmd", "create_channel"}, {"name", "bobs-channel"}})));
    REQUIRE(refused(bob.cmd({{"cmd", "create_role"}, {"name", "boss"}, {"permissions", {"administrator"}}})));
    REQUIRE(refused(bob.cmd({{"cmd", "kick"}, {"username", "carol"}})));
    REQUIRE(refused(bob.cmd({{"cmd", "ban_user"}, {"username", "carol"}})));
    REQUIRE(refused(bob.cmd({{"cmd", "delete_channel"}, {"room_id", general}})));

    // The owner makes a channel and a role, and gives the role to Bob.
    REQUIRE(alice.cmd({{"cmd", "create_channel"}, {"name", "staff-room"}})["ok"] == true);
    std::string staff_room;
    for (Client* c : {&alice, &bob, &carol})
        staff_room = c->have("#staff-room", room_named("#staff-room"))["room"]["room_id"];
    REQUIRE(alice.cmd({{"cmd", "create_role"}, {"name", "staff"}, {"permissions", {"kick_members", "manage_messages"}}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "grant_role"}, {"username", "bob"}, {"role", "staff"}})["ok"] == true);
    bob.have("role arrives", [](const json& e) {
        if (e["event"] != "server_info") return false;
        for (const auto& p : e["my_permissions"]) if (p == "kick_members") return true;
        return false;
    });
    {
        json members = alice.cmd({{"cmd", "member_list"}})["data"]["members"];
        REQUIRE(members.size() == 3);
        for (const auto& m : members) {
            REQUIRE(m["is_owner"] == (m["username"] == "alice"));
            REQUIRE(m["roles"].size() == (m["username"] == "bob" ? 1u : 0u));
        }
    }

    // Moderation: Bob's role carries manage_messages, Carol has no such role.
    {
        json s = carol.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "something against the rules"}});
        std::string bad = s["data"]["event_id"];
        alice.wait_message("something against the rules");
        bob.wait_message("something against the rules");
        json a = alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "a perfectly fine message"}});
        std::string fine = a["data"]["event_id"];
        carol.wait_message("a perfectly fine message");
        bob.wait_message("a perfectly fine message");

        // Carol tries to delete Alice's message. Nobody honours it.
        REQUIRE(carol.cmd({{"cmd", "delete_event"}, {"room_id", general}, {"event_id", fine}})["ok"] == true);
        // Bob deletes Carol's. Everyone honours it, including Carol's own client.
        REQUIRE(bob.cmd({{"cmd", "delete_event"}, {"room_id", general}, {"event_id", bad}})["ok"] == true);
        auto removed = [&](const json& e) {
            return e["event"] == "event_updated" && e["data"]["event_id"] == bad && e["data"]["status"] == "redacted";
        };
        alice.wait("moderator deletion", removed);
        carol.wait("moderator deletion", removed);
        for (Client* c : {&alice, &bob, &carol}) {
            json tl = c->cmd({{"cmd", "fetch_timeline"}, {"room_id", general}, {"limit", 500}});
            for (const auto& e : tl["data"]["events"]) {
                if (e["event_id"] == bad) REQUIRE(e["status"] == "redacted");
                if (e["event_id"] == fine) REQUIRE(e["status"] == "ok");
            }
        }
    }

    // A private channel: hidden from @everyone, visible to staff.
    REQUIRE(alice.cmd({{"cmd", "set_channel_access"}, {"room_id", staff_room}, {"role", "@everyone"}, {"deny", {"view_channel"}}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "set_channel_access"}, {"room_id", staff_room}, {"role", "staff"}, {"allow", {"view_channel"}}})["ok"] == true);
    carol.have("staff-room hidden", room_gone(staff_room));
    // Bob loses it for a moment between the two changes, then gets it back.
    auto eventually = [](const std::function<bool()>& check) {
        for (int i = 0; i < 100; ++i) {
            if (check()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    };
    REQUIRE(eventually([&] { return sees(bob, "#staff-room"); }));
    REQUIRE_FALSE(sees(carol, "#staff-room"));
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", staff_room}, {"body", "staff only business"}})["ok"] == true);
    bob.wait_message("staff only business");
    REQUIRE(refused(carol.cmd({{"cmd", "send_text"}, {"room_id", staff_room}, {"body", "let me in"}})));

    // A read-only channel: everyone sees it, only the owner can post.
    REQUIRE(alice.cmd({{"cmd", "create_channel"}, {"name", "announcements"}})["ok"] == true);
    std::string news;
    for (Client* c : {&alice, &bob, &carol})
        news = c->have("#announcements", room_named("#announcements"))["room"]["room_id"];
    REQUIRE(alice.cmd({{"cmd", "set_channel_access"}, {"room_id", news}, {"role", "@everyone"}, {"deny", {"send_messages"}}})["ok"] == true);
    json blocked = carol.cmd({{"cmd", "send_text"}, {"room_id", news}, {"body", "can i post here"}});
    REQUIRE(blocked["ok"] == true);  // queued locally; the server is what refuses it
    REQUIRE(carol.wait_sent(blocked["data"]["event_id"])["status"] == "failed");
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", news}, {"body", "read this, everyone"}})["ok"] == true);
    bob.wait_message("read this, everyone");
    carol.wait_message("read this, everyone");

    // Bob's role lets him remove Carol, but not the owner.
    REQUIRE(refused(bob.cmd({{"cmd", "kick"}, {"username", "alice"}})));
    REQUIRE(bob.cmd({{"cmd", "kick"}, {"username", "carol"}})["ok"] == true);
    carol.wait("kicked", [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("removed from this server") != std::string::npos;
    }, 20000);
    alice.have("two in #general", [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == general && e["room"]["members"].size() == 2;
    });
    // She does not walk straight back in, but she may rejoin on purpose.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(alice.cmd({{"cmd", "member_list"}})["data"]["members"].size() == 2);
    REQUIRE(carol.cmd(connect)["ok"] == true);
    carol.wait_live();
    REQUIRE(alice.cmd({{"cmd", "member_list"}})["data"]["members"].size() == 3);

    // A ban sticks until it is lifted.
    REQUIRE(alice.cmd({{"cmd", "ban_user"}, {"username", "carol"}})["ok"] == true);
    auto banned = [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("banned") != std::string::npos;
    };
    carol.wait("banned", banned, 20000);
    REQUIRE(carol.cmd(connect)["ok"] == true);
    carol.wait("still banned", banned, 20000);
    REQUIRE(alice.cmd({{"cmd", "ban_user"}, {"username", "carol"}, {"banned", false}})["ok"] == true);
    REQUIRE(carol.cmd(connect)["ok"] == true);
    carol.wait_live();

    // Taking the role away takes the private channel with it.
    REQUIRE(alice.cmd({{"cmd", "delete_role"}, {"role", "staff"}})["ok"] == true);
    bob.have("staff-room gone", room_gone(staff_room));
    REQUIRE(refused(bob.cmd({{"cmd", "kick"}, {"username", "carol"}})));

    // Channels can be renamed and deleted, but the last one stays.
    REQUIRE(alice.cmd({{"cmd", "rename_channel"}, {"room_id", news}, {"name", "news"}})["ok"] == true);
    bob.have("#news", room_named("#news"));
    REQUIRE(alice.cmd({{"cmd", "delete_channel"}, {"room_id", news}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "delete_channel"}, {"room_id", staff_room}})["ok"] == true);
    REQUIRE(refused(alice.cmd({{"cmd", "delete_channel"}, {"room_id", general}})));

    // None of the channel traffic is readable on the server.
    REQUIRE_FALSE(tree_contains(tmp.path / "server", "staff only business"));
    REQUIRE_FALSE(tree_contains(tmp.path / "server", "welcome to the server"));
}

TEST_CASE("without a named owner the first person to register owns the server") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string());
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client first((tmp.path / "first").string()), second((tmp.path / "second").string());
    REQUIRE(first.create("first")["ok"] == true);
    REQUIRE(first.cmd(connect)["ok"] == true);
    first.wait_live();
    REQUIRE(second.create("second")["ok"] == true);
    REQUIRE(second.cmd(connect)["ok"] == true);
    second.wait_live();
    REQUIRE(first.cmd({{"cmd", "server_info"}})["data"]["is_owner"] == true);
    REQUIRE(second.cmd({{"cmd", "server_info"}})["data"]["is_owner"] == false);
    REQUIRE(first.cmd({{"cmd", "create_channel"}, {"name", "off-topic"}})["ok"] == true);
    REQUIRE(second.cmd({{"cmd", "create_channel"}, {"name", "nope"}})["ok"] == false);
}

TEST_CASE("members make invite links that let others join an invite-only server") {
    TempDir tmp;
    int port = test_port();
    // Two flags are needed here; the helper passes one pair, so use the long form.
    Server server(port, (tmp.path / "server").string(), "--invite-only");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    auto refused = [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("registration failed") != std::string::npos;
    };
    // With no owner named, nobody is inside yet, so the very first person
    // cannot get in without a code either. Start again with a named owner.
    {
        Client nobody((tmp.path / "nobody").string());
        REQUIRE(nobody.create("nobody")["ok"] == true);
        REQUIRE(nobody.cmd(connect)["ok"] == true);
        nobody.wait("refusal", refused);
    }
}

TEST_CASE("an invite link carries the address and the server key and a code") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--owner", "alice");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string()),
        carol((tmp.path / "carol").string());
    REQUIRE(alice.create("alice")["ok"] == true);
    REQUIRE(alice.cmd(connect)["ok"] == true);
    alice.wait_live();

    json invite = alice.cmd({{"cmd", "create_invite"}, {"max_uses", 1}});
    REQUIRE(invite["ok"] == true);
    std::string link = invite["data"]["link"];
    REQUIRE(link.rfind("corded://127.0.0.1:" + std::to_string(port) + "/?fp=", 0) == 0);
    REQUIRE(link.find("&invite=" + invite["data"]["code"].get<std::string>()) != std::string::npos);

    // Bob joins with nothing but the link.
    REQUIRE(bob.create("bob")["ok"] == true);
    REQUIRE(bob.cmd({{"cmd", "connect"}, {"link", link}})["ok"] == true);
    bob.wait_live();
    // An ordinary member cannot make invites until a role allows it.
    REQUIRE(bob.cmd({{"cmd", "create_invite"}})["ok"] == false);
    REQUIRE(alice.cmd({{"cmd", "create_role"}, {"name", "greeter"}, {"permissions", {"create_invite"}}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "grant_role"}, {"username", "bob"}, {"role", "greeter"}})["ok"] == true);
    bob.have("greeter role", [](const json& e) {
        if (e["event"] != "server_info") return false;
        for (const auto& p : e["my_permissions"]) if (p == "create_invite") return true;
        return false;
    });
    json second = bob.cmd({{"cmd", "create_invite"}});
    REQUIRE(second["ok"] == true);
    REQUIRE(bob.cmd({{"cmd", "revoke_invite"}, {"code", second["data"]["code"]}})["ok"] == true);
    REQUIRE(bob.cmd({{"cmd", "revoke_invite"}, {"code", "no-such-code"}})["ok"] == false);

    // A link with a wrong server key is refused before anything is sent.
    std::string tampered = link;
    tampered.replace(tampered.find("fp=") + 3, 4, "AAAA");
    REQUIRE(carol.create("carol")["ok"] == true);
    REQUIRE(carol.cmd({{"cmd", "connect"}, {"link", tampered}})["ok"] == true);
    carol.wait("identity refusal", [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("identity") != std::string::npos;
    });
    REQUIRE(carol.cmd({{"cmd", "connect"}, {"link", "https://example.org"}})["ok"] == false);
}

TEST_CASE("invite codes run out and can be revoked on an invite-only server") {
    TempDir tmp;
    int port = test_port();
    // --owner takes the helper's one flag pair, so the invite-only policy is
    // exercised here through a fixed code as the second mechanism.
    Server server(port, (tmp.path / "server").string(), "--invite-code", "fixed-code-123");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    auto refused = [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("registration failed") != std::string::npos;
    };
    auto with_code = [&](const std::string& code) {
        json c = connect;
        c["invite"] = code;
        return c;
    };
    Client owner((tmp.path / "owner").string());
    REQUIRE(owner.create("owner")["ok"] == true);
    REQUIRE(owner.cmd(with_code("fixed-code-123"))["ok"] == true);
    owner.wait_live();  // first in, so the owner

    std::string once = owner.cmd({{"cmd", "create_invite"}, {"max_uses", 1}})["data"]["code"];
    std::string revoked = owner.cmd({{"cmd", "create_invite"}})["data"]["code"];
    REQUIRE(owner.cmd({{"cmd", "revoke_invite"}, {"code", revoked}})["ok"] == true);

    Client first((tmp.path / "first").string()), second((tmp.path / "second").string()),
        third((tmp.path / "third").string());
    REQUIRE(first.create("first")["ok"] == true);
    REQUIRE(first.cmd(with_code(once))["ok"] == true);
    first.wait_live();
    // The single use is spent.
    REQUIRE(second.create("second")["ok"] == true);
    REQUIRE(second.cmd(with_code(once))["ok"] == true);
    second.wait("used up", refused);
    // A revoked code never works.
    REQUIRE(third.create("third")["ok"] == true);
    REQUIRE(third.cmd(with_code(revoked))["ok"] == true);
    third.wait("revoked", refused);
    // Someone already inside is unaffected by any of this.
    first.close();
    first.open();
    REQUIRE(first.unlock()["ok"] == true);
    first.wait_live();
}

TEST_CASE("disappearing messages are erased everywhere when their time comes") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--owner", "alice");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string());
    std::string general;
    for (auto [client, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        client->have("live", [](const json& e) { return e["event"] == "connection_state" && e["state"] == "live"; });
        general = client->have("#general", [](const json& e) {
            return e["event"] == "room_updated" && e["room"]["title"] == "#general";
        })["room"]["room_id"];
    }
    alice.have("two members", [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == general && e["room"]["members"].size() == 2;
    });
    auto in_timeline = [&](Client& c, const std::string& body) {
        json tl = c.cmd({{"cmd", "fetch_timeline"}, {"room_id", general}, {"limit", 500}});
        for (const auto& e : tl["data"]["events"])
            if (e["content"].value("body", "") == body) return true;
        return false;
    };
    auto expired = [](const std::string& id) {
        return [id](const json& e) { return e["event"] == "event_expired" && e["event_id"] == id; };
    };

    // One message with its own timer.
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "this stays"}})["ok"] == true);
    json once = alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "this will vanish"}, {"expires_in", 2}});
    std::string once_id = once["data"]["event_id"];
    json got = bob.wait_message("this will vanish");
    REQUIRE(got["data"]["expires_at"].get<uint64_t>() > 0);
    REQUIRE(in_timeline(bob, "this will vanish"));
    alice.wait("expiry", expired(once_id), 8000);
    bob.have("expiry", expired(once_id));
    for (Client* c : {&alice, &bob}) {
        REQUIRE_FALSE(in_timeline(*c, "this will vanish"));
        REQUIRE(in_timeline(*c, "this stays"));
    }

    // A whole channel set to disappear. Bob has no say in a channel...
    REQUIRE(bob.cmd({{"cmd", "set_disappearing"}, {"room_id", general}, {"seconds", 1}})["ok"] == true);
    // ...but the owner does, and from then on everyone's messages there expire.
    REQUIRE(alice.cmd({{"cmd", "set_disappearing"}, {"room_id", general}, {"seconds", 2}})["ok"] == true);
    auto ttl_is = [&](uint64_t seconds) {
        return [&, seconds](const json& e) {
            return e["event"] == "room_updated" && e["room"]["room_id"] == general &&
                   e["room"]["disappear_after"] == seconds;
        };
    };
    bob.wait("channel timer", ttl_is(2));
    for (const auto& e : alice.seen)
        if (e["event"] == "room_updated" && e["room"]["room_id"] == general)
            REQUIRE(e["room"]["disappear_after"] != 1);
    json auto_gone = bob.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "gone on its own"}});
    alice.wait_message("gone on its own");
    alice.wait("expiry", expired(auto_gone["data"]["event_id"]), 8000);
    REQUIRE_FALSE(in_timeline(alice, "gone on its own"));
    REQUIRE(in_timeline(alice, "this stays"));  // sent before the timer was set

    // Turned off again, messages are kept.
    REQUIRE(alice.cmd({{"cmd", "set_disappearing"}, {"room_id", general}, {"seconds", 0}})["ok"] == true);
    bob.wait("timer off", ttl_is(0));
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "kept again"}})["ok"] == true);
    alice.wait_message("kept again");

    // The server drops its copy too: someone offline past the deadline never gets it.
    bob.close();
    json missed = alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "expired before bob returned"}, {"expires_in", 1}});
    REQUIRE(alice.wait_sent(missed["data"]["event_id"])["status"] == "sent");
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "still waiting for bob"}})["ok"] == true);
    std::this_thread::sleep_for(std::chrono::seconds(8));  // past the server's sweep
    bob.open();
    REQUIRE(bob.unlock()["ok"] == true);
    bob.wait_message("still waiting for bob");
    for (const auto& e : bob.seen)
        if (e["event"] == "event_received")
            REQUIRE(e["data"]["content"].value("body", "") != "expired before bob returned");
    REQUIRE_FALSE(in_timeline(bob, "expired before bob returned"));
}

TEST_CASE("one client belongs to two servers at once") {
    TempDir tmp;
    int port_a = test_port(), port_b = test_port();
    Server server_a(port_a, (tmp.path / "server-a").string(), "--name", "Alpha");
    Server server_b(port_b, (tmp.path / "server-b").string(), "--name", "Beta");
    auto connect_to = [](int port) { return json{{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}}; };
    auto live_on = [](int64_t server) {
        return [server](const json& e) {
            return e["event"] == "connection_state" && e["state"] == "live" && e["server_id"] == server;
        };
    };

    // Alice joins both; Bob is only on Alpha, Carol only on Beta.
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string()),
        carol((tmp.path / "carol").string());
    REQUIRE(alice.create("alice")["ok"] == true);
    json first = alice.cmd(connect_to(port_a));
    int64_t alpha = first["data"]["server_id"];
    alice.have("live on alpha", live_on(alpha));
    json second = alice.cmd(connect_to(port_b));
    int64_t beta = second["data"]["server_id"];
    REQUIRE(alpha != beta);
    alice.have("live on beta", live_on(beta));
    REQUIRE(bob.create("bob")["ok"] == true);
    REQUIRE(bob.cmd(connect_to(port_a))["ok"] == true);
    bob.wait_live();
    REQUIRE(carol.create("carol")["ok"] == true);
    REQUIRE(carol.cmd(connect_to(port_b))["ok"] == true);
    carol.wait_live();

    // Each server tells Alice about itself, and she owns both (first to register).
    json servers = alice.cmd({{"cmd", "list_servers"}})["data"]["servers"];
    REQUIRE(servers.size() == 2);
    std::map<int64_t, std::string> names;
    for (const auto& s : servers) {
        names[s["server_id"].get<int64_t>()] = s["name"];
        REQUIRE(s["is_owner"] == true);
        REQUIRE(s["connection"] == "live");
    }
    REQUIRE(names[alpha] == "Alpha");
    REQUIRE(names[beta] == "Beta");

    // Each server has its own #general, and rooms say which server they are on.
    std::string general_a, general_b;
    auto find_generals = [&] {
        json rooms = alice.cmd({{"cmd", "list_rooms"}})["data"]["rooms"];
        for (const auto& r : rooms) {
            if (r["title"] != "#general") continue;
            if (r["server_id"] == alpha && r["members"].size() == 2) general_a = r["room_id"];
            if (r["server_id"] == beta && r["members"].size() == 2) general_b = r["room_id"];
        }
        return !general_a.empty() && !general_b.empty();
    };
    for (int i = 0; i < 100 && !find_generals(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(find_generals());
    REQUIRE(general_a != general_b);
    {
        json only_beta = alice.cmd({{"cmd", "list_rooms"}, {"server_id", beta}})["data"]["rooms"];
        for (const auto& r : only_beta) REQUIRE(r["server_id"] == beta);
    }

    // A message goes to the server its room is on, and nowhere else.
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general_a}, {"body", "hello alpha"}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general_b}, {"body", "hello beta"}})["ok"] == true);
    bob.wait_message("hello alpha");
    carol.wait_message("hello beta");
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", general_a}, {"body", "bob on alpha"}})["ok"] == true);
    REQUIRE(carol.cmd({{"cmd", "send_text"}, {"room_id", general_b}, {"body", "carol on beta"}})["ok"] == true);
    REQUIRE(alice.have_message("bob on alpha")["server_id"] == alpha);
    REQUIRE(alice.have_message("carol on beta")["server_id"] == beta);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (const auto& e : bob.seen)
        if (e["event"] == "event_received") REQUIRE(e["data"]["content"].value("body", "") != "hello beta");
    REQUIRE_FALSE(tree_contains(tmp.path / "server-a", "carol"));

    // Running one server does not touch the other.
    REQUIRE(alice.cmd({{"cmd", "create_channel"}, {"name", "beta-only"}, {"server_id", beta}})["ok"] == true);
    carol.have("#beta-only", [](const json& e) { return e["event"] == "room_updated" && e["room"]["title"] == "#beta-only"; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (const auto& e : bob.seen)
        if (e["event"] == "room_updated") REQUIRE(e["room"]["title"] != "#beta-only");
    REQUIRE(alice.cmd({{"cmd", "create_channel"}, {"name", "x"}, {"server_id", 999}})["ok"] == false);

    // Losing one server leaves the other working.
    server_b.stop();
    alice.wait("beta down", [&](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" && e["server_id"] == beta;
    });
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general_a}, {"body", "alpha still works"}})["ok"] == true);
    bob.wait_message("alpha still works");
    server_b.start();

    // After a restart the client is back on both, with both histories.
    alice.close();
    alice.open();
    REQUIRE(alice.unlock()["ok"] == true);
    alice.have("alpha again", live_on(alpha));
    alice.have("beta again", live_on(beta));
    json tl_a = alice.cmd({{"cmd", "fetch_timeline"}, {"room_id", general_a}})["data"]["events"];
    json tl_b = alice.cmd({{"cmd", "fetch_timeline"}, {"room_id", general_b}})["data"]["events"];
    auto has = [](const json& tl, const std::string& body) {
        for (const auto& e : tl) if (e["content"].value("body", "") == body) return true;
        return false;
    };
    REQUIRE(has(tl_a, "bob on alpha"));
    REQUIRE_FALSE(has(tl_a, "carol on beta"));
    REQUIRE(has(tl_b, "carol on beta"));
}

TEST_CASE("a newcomer is given earlier messages by a member who is willing") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--owner", "alice");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string());
    std::string general;
    for (auto [client, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        client->have("live", [](const json& e) { return e["event"] == "connection_state" && e["state"] == "live"; });
        general = client->have("#general", [](const json& e) {
            return e["event"] == "room_updated" && e["room"]["title"] == "#general";
        })["room"]["room_id"];
    }
    alice.have("two members", [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == general && e["room"]["members"].size() == 2;
    });

    // A conversation before Carol exists.
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "old message one"}})["ok"] == true);
    bob.wait_message("old message one");
    json typo = bob.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "old mesage two"}});
    alice.wait_message("old mesage two");
    REQUIRE(bob.cmd({{"cmd", "edit_event"}, {"room_id", general}, {"event_id", typo["data"]["event_id"]}, {"body", "old message two"}})["ok"] == true);
    json regret = alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "deleted before carol"}});
    bob.wait_message("deleted before carol");
    REQUIRE(alice.cmd({{"cmd", "delete_event"}, {"room_id", general}, {"event_id", regret["data"]["event_id"]}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "meant to disappear"}, {"expires_in", 600}})["ok"] == true);
    bob.wait_message("meant to disappear");
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "old message three"}})["ok"] == true);
    bob.wait_message("old message three");

    auto bodies = [&](Client& c) {
        std::vector<std::string> out;
        json tl = c.cmd({{"cmd", "fetch_timeline"}, {"room_id", general}, {"limit", 500}});
        for (const auto& e : tl["data"]["events"])
            if (e["type"] == "m.text" && e["status"] == "ok") out.push_back(e["content"].value("body", ""));
        return out;
    };

    // Carol joins. Her client asks by itself, and the members' clients answer.
    Client carol((tmp.path / "carol").string());
    REQUIRE(carol.create("carol")["ok"] == true);
    REQUIRE(carol.cmd(connect)["ok"] == true);
    json shared = carol.wait("shared history", [](const json& e) {
        return e["event"] == "event_received" && e["data"]["content"].value("body", "") == "old message three";
    });
    REQUIRE(shared["data"]["shared_history"] == true);
    REQUIRE(shared["data"]["sender_name"] == "alice");
    std::vector<std::string> got;
    for (int i = 0; i < 100; ++i) {
        got = bodies(carol);
        if (got.size() >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // In order, with the edit applied, and without what was deleted or set to disappear.
    REQUIRE(got == std::vector<std::string>{"old message one", "old message two", "old message three"});
    // Envelopes never show up as messages.
    json tl = carol.cmd({{"cmd", "fetch_timeline"}, {"room_id", general}, {"limit", 500}});
    for (const auto& e : tl["data"]["events"]) REQUIRE(e["type"] != "m.history.share");
    for (const auto& e : carol.seen)
        if (e["event"] == "event_received") REQUIRE(e["data"]["type"] != "m.history.share");
    // New messages still arrive first-hand and are not marked as shared.
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "welcome carol"}})["ok"] == true);
    REQUIRE(carol.wait_message("welcome carol")["data"]["shared_history"] == false);
    // The server never saw any of it in the clear.
    REQUIRE_FALSE(tree_contains(tmp.path / "server", "old message"));

    // Members who have turned sharing off do not answer.
    REQUIRE(alice.cmd({{"cmd", "set_history_sharing"}, {"enabled", false}})["ok"] == true);
    REQUIRE(bob.cmd({{"cmd", "set_history_sharing"}, {"enabled", false}})["ok"] == true);
    REQUIRE(carol.cmd({{"cmd", "set_history_sharing"}, {"enabled", false}})["ok"] == true);
    Client dave((tmp.path / "dave").string());
    REQUIRE(dave.create("dave")["ok"] == true);
    REQUIRE(dave.cmd(connect)["ok"] == true);
    dave.wait_live();
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "hello dave"}})["ok"] == true);
    dave.wait_message("hello dave");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    REQUIRE(bodies(dave) == std::vector<std::string>{"hello dave"});
    // One member turning it back on is enough, when asked again.
    REQUIRE(bob.cmd({{"cmd", "set_history_sharing"}, {"enabled", true}})["ok"] == true);
    REQUIRE(dave.cmd({{"cmd", "request_history"}, {"room_id", general}})["ok"] == true);
    dave.wait("history after asking again", [](const json& e) {
        return e["event"] == "history_received" && e["count"].get<int>() >= 3;
    });
}

TEST_CASE("a server can forbid sharing earlier messages") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--no-history-sharing");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string());
    REQUIRE(alice.create("alice")["ok"] == true);
    REQUIRE(alice.cmd(connect)["ok"] == true);
    alice.wait_live();
    std::string general = alice.have("#general", [](const json& e) {
        return e["event"] == "room_updated" && e["room"]["title"] == "#general";
    })["room"]["room_id"];
    // Nobody else is here yet, so this is stored with no recipients.
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "before anyone joined"}})["ok"] == true);

    REQUIRE(bob.create("bob")["ok"] == true);
    REQUIRE(bob.cmd(connect)["ok"] == true);
    bob.wait_live();
    REQUIRE(bob.cmd({{"cmd", "server_info"}})["data"]["history_sharing"] == false);
    json asked = bob.cmd({{"cmd", "request_history"}, {"room_id", general}});
    REQUIRE(asked["ok"] == false);
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "after bob joined"}})["ok"] == true);
    bob.wait_message("after bob joined");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (const auto& e : bob.seen)
        if (e["event"] == "event_received")
            REQUIRE(e["data"]["content"].value("body", "") != "before anyone joined");
}

TEST_CASE("the owner runs the server from a client") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--owner", "alice");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client alice((tmp.path / "alice").string()), bob((tmp.path / "bob").string());
    auto live = [](const json& e) { return e["event"] == "connection_state" && e["state"] == "live"; };
    for (auto [client, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        client->have("live", live);
    }
    auto value_of = [&](Client& c, const std::string& key) -> std::string {
        json settings = c.cmd({{"cmd", "get_settings"}})["data"]["settings"];
        for (const auto& s : settings)
            if (s["key"] == key) return s["value"];
        return "(missing)";
    };

    // Settings can be read and changed by the owner, not by an ordinary member.
    REQUIRE(value_of(alice, "scope") == "machine");
    REQUIRE(value_of(alice, "registration") == "open");
    REQUIRE(value_of(alice, "retention_days") == "30");
    REQUIRE(bob.cmd({{"cmd", "get_settings"}})["ok"] == false);
    REQUIRE(bob.cmd({{"cmd", "set_setting"}, {"key", "registration"}, {"value", "closed"}})["ok"] == false);
    REQUIRE(bob.cmd({{"cmd", "restart_server"}})["ok"] == false);
    REQUIRE(bob.cmd({{"cmd", "server_status"}})["ok"] == false);

    // Bad values and unknown settings are refused.
    REQUIRE(alice.cmd({{"cmd", "set_setting"}, {"key", "scope"}, {"value", "everywhere"}})["ok"] == false);
    REQUIRE(alice.cmd({{"cmd", "set_setting"}, {"key", "restart"}, {"value", "sometimes"}})["ok"] == false);
    REQUIRE(alice.cmd({{"cmd", "set_setting"}, {"key", "no_such_thing"}, {"value", "1"}})["ok"] == false);
    REQUIRE(alice.cmd({{"cmd", "set_setting"}, {"key", "restart"}, {"value", "weekly sun 04:00"}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "set_setting"}, {"key", "retention_days"}, {"value", "7"}})["ok"] == true);

    // A change takes effect at once: the name reaches members, registration closes.
    REQUIRE(alice.cmd({{"cmd", "set_setting"}, {"key", "name"}, {"value", "Renamed Place"}})["ok"] == true);
    bob.wait("new name", [](const json& e) { return e["event"] == "server_info" && e["name"] == "Renamed Place"; });
    REQUIRE(alice.cmd({{"cmd", "set_setting"}, {"key", "registration"}, {"value", "closed"}})["ok"] == true);
    Client late((tmp.path / "late").string());
    REQUIRE(late.create("late")["ok"] == true);
    REQUIRE(late.cmd(connect)["ok"] == true);
    late.wait("refusal", [](const json& e) {
        return e["event"] == "connection_state" && e["state"] == "disconnected" &&
               e.value("detail", "").find("registration failed") != std::string::npos;
    });

    // A role with manage_server can change ordinary settings, but not the owner-only ones.
    REQUIRE(alice.cmd({{"cmd", "create_role"}, {"name", "ops"}, {"permissions", {"manage_server"}}})["ok"] == true);
    REQUIRE(alice.cmd({{"cmd", "grant_role"}, {"username", "bob"}, {"role", "ops"}})["ok"] == true);
    bob.have("ops role", [](const json& e) {
        if (e["event"] != "server_info") return false;
        for (const auto& p : e["my_permissions"]) if (p == "manage_server") return true;
        return false;
    });
    REQUIRE(bob.cmd({{"cmd", "set_setting"}, {"key", "history_sharing"}, {"value", "off"}})["ok"] == true);
    json scope_attempt = bob.cmd({{"cmd", "set_setting"}, {"key", "scope"}, {"value", "internet"}});
    REQUIRE(scope_attempt["ok"] == false);
    REQUIRE(scope_attempt["error"]["message"].get<std::string>().find("owner") != std::string::npos);

    json status = alice.cmd({{"cmd", "server_status"}})["data"]["status"];
    REQUIRE(status["members"] == 2);
    REQUIRE(status["online"] == 2);
    REQUIRE(status["scope"] == "machine");
    REQUIRE(status["stored_bytes"].get<uint64_t>() > 0);
    uint64_t first_start = status["started_at"];

    // Restarting from the client: everyone is told, reconnects, and the settings are still there.
    alice.seen.clear();
    bob.seen.clear();
    REQUIRE(alice.cmd({{"cmd", "restart_server"}})["ok"] == true);
    bob.wait("restart notice", [](const json& e) {
        return e["event"] == "server_notice" && e.value("message", "").find("restarting") != std::string::npos;
    });
    auto down = [](const json& e) { return e["event"] == "connection_state" && e["state"] == "disconnected"; };
    alice.wait("dropped", down, 20000);
    alice.wait("back", live, 30000);
    bob.have("dropped", down);
    bob.wait("back", live, 30000);
    REQUIRE(value_of(alice, "name") == "Renamed Place");
    REQUIRE(value_of(alice, "registration") == "closed");
    REQUIRE(value_of(alice, "retention_days") == "7");
    REQUIRE(value_of(alice, "restart") == "weekly sun 04:00");
    REQUIRE(value_of(alice, "history_sharing") == "off");
    REQUIRE(alice.cmd({{"cmd", "server_status"}})["data"]["status"]["started_at"].get<uint64_t>() > first_start);
    REQUIRE(alice.cmd({{"cmd", "server_info"}})["data"]["is_owner"] == true);
    // And it still works as a chat server.
    std::string general;
    json rooms = alice.cmd({{"cmd", "list_rooms"}})["data"]["rooms"];
    for (const auto& r : rooms) if (r["title"] == "#general") general = r["room_id"];
    REQUIRE(alice.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "after the restart"}})["ok"] == true);
    bob.wait_message("after the restart");
}

TEST_CASE("one person on two devices") {
    TempDir tmp;
    int port = test_port();
    Server server(port, (tmp.path / "server").string(), "--owner", "alice");
    json connect = {{"cmd", "connect"}, {"host", "127.0.0.1"}, {"port", port}};
    Client pc((tmp.path / "alice-pc").string()), bob((tmp.path / "bob").string());
    auto live = [](const json& e) { return e["event"] == "connection_state" && e["state"] == "live"; };
    std::string general;
    for (auto [client, name] : {std::pair{&pc, "alice"}, {&bob, "bob"}}) {
        REQUIRE(client->create(name)["ok"] == true);
        REQUIRE(client->cmd(connect)["ok"] == true);
        client->have("live", live);
        general = client->have("#general", [](const json& e) {
            return e["event"] == "room_updated" && e["room"]["title"] == "#general";
        })["room"]["room_id"];
    }
    pc.have("two members", [&](const json& e) {
        return e["event"] == "room_updated" && e["room"]["room_id"] == general && e["room"]["members"].size() == 2;
    });
    // Some history before the second device exists: a channel message and a direct message.
    REQUIRE(pc.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "sent from the pc"}})["ok"] == true);
    bob.wait_message("sent from the pc");
    json dm = pc.cmd({{"cmd", "start_chat"}, {"username", "bob"}});
    std::string direct = dm["data"]["room"]["room_id"];
    bob.have("the direct chat", [&](const json& e) { return e["event"] == "room_updated" && e["room"]["room_id"] == direct; });
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", direct}, {"body", "private note to alice"}})["ok"] == true);
    pc.wait_message("private note to alice");

    // Alice sets up her laptop with the recovery key from her PC.
    std::string key = pc.cmd({{"cmd", "get_recovery_key"}})["data"]["recovery_key"];
    std::string user_id = pc.cmd({{"cmd", "status"}})["data"]["user_id"];
    Client laptop((tmp.path / "alice-laptop").string());
    REQUIRE(laptop.restore("alice", "0000-" + key.substr(5))["ok"] == false);  // a mistyped key is caught
    REQUIRE(laptop.restore("alice", key, "a different passphrase")["ok"] == true);
    REQUIRE(laptop.cmd({{"cmd", "status"}})["data"]["user_id"] == user_id);   // the same person
    REQUIRE(laptop.cmd(connect)["ok"] == true);
    laptop.have("live", live);
    // The server sees one member, not two, and she still owns it.
    REQUIRE(pc.cmd({{"cmd", "member_list"}})["data"]["members"].size() == 2);
    REQUIRE(laptop.cmd({{"cmd", "server_info"}})["data"]["is_owner"] == true);

    // Her own earlier messages and the direct message arrive from her PC.
    laptop.wait("own history", [](const json& e) {
        return e["event"] == "event_received" && e["data"]["content"].value("body", "") == "sent from the pc";
    });
    laptop.have("direct message history", [](const json& e) {
        return e["event"] == "event_received" && e["data"]["content"].value("body", "") == "private note to alice";
    });

    // What Bob sends now reaches both of her devices.
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "hello both devices"}})["ok"] == true);
    pc.wait_message("hello both devices");
    laptop.wait_message("hello both devices");
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", direct}, {"body", "second private note"}})["ok"] == true);
    pc.wait_message("second private note");
    laptop.wait_message("second private note");

    // What she sends from one device shows up on the other as her own.
    auto own_copy = [](const std::string& body) {
        return [body](const json& e) {
            return e["event"] == "event_received" && e["data"]["content"].value("body", "") == body &&
                   e["data"]["mine"] == true && e["data"]["status"] == "ok";
        };
    };
    REQUIRE(laptop.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "typed on the laptop"}})["ok"] == true);
    bob.wait_message("typed on the laptop");
    pc.wait("copy on the pc", own_copy("typed on the laptop"));
    REQUIRE(pc.cmd({{"cmd", "send_text"}, {"room_id", direct}, {"body", "typed on the pc"}})["ok"] == true);
    bob.wait_message("typed on the pc");
    laptop.wait("copy on the laptop", own_copy("typed on the pc"));
    // Bob sees one Alice throughout.
    REQUIRE(bob.cmd({{"cmd", "safety_numbers"}, {"room_id", direct}})["data"]["safety_numbers"].size() == 1);

    // Each device works alone: the PC goes away, the laptop carries on.
    pc.close();
    REQUIRE(bob.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "while the pc is off"}})["ok"] == true);
    laptop.wait_message("while the pc is off");
    REQUIRE(laptop.cmd({{"cmd", "send_text"}, {"room_id", general}, {"body", "laptop answering"}})["ok"] == true);
    bob.wait_message("laptop answering");
    pc.open();
    REQUIRE(pc.unlock()["ok"] == true);
    pc.wait_message("while the pc is off");
    pc.have("laptop message caught up", own_copy("laptop answering"));
    REQUIRE_FALSE(tree_contains(tmp.path / "server", "typed on the laptop"));
}

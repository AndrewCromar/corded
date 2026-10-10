// corded-tui: a terminal client. It talks to the core only through the public
// C header: commands go in as JSON, events come out as JSON.
#include "corded/corded.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;
using nlohmann::json;

namespace {

struct Message {
    std::string event_id, sender, body, status, type;
    std::string reply_to;
    std::string thread_root;  // set on messages that belong to a thread
    uint64_t ts = 0;
    bool mine = false;
    bool edited = false;
    bool disappearing = false;
    bool from_history = false;  // handed over by another member, not received first-hand
    int num = 0;  // short number shown next to the message, for commands like /reply 12
};

// What the client knows about one of the servers it belongs to.
struct ServerState {
    std::string name, address, connection, fingerprint;
    bool is_owner = false;
    std::set<std::string> permissions;
    json roles = json::array();
};

struct Room {
    std::string id, title;
    std::string kind = "direct";  // channel, direct or group
    uint64_t disappear_after = 0;  // seconds; 0 = messages are kept
    int64_t server_id = 0;
    int next_num = 1;
    std::map<std::string, std::map<std::string, std::string>> my_reactions;  // message -> emoji -> my reaction's id
    std::map<std::string, uint64_t> typing;        // display name -> when the notice lapses (ms)
    std::map<std::string, std::string> read_upto;  // display name -> id of the newest message they read
    std::string last_marked;                       // newest message we have told others we read
    std::vector<Message> messages;
    // target event id -> reaction key -> ids of the reaction events
    std::map<std::string, std::map<std::string, std::set<std::string>>> reactions;
    int unread = 0;
    bool loaded = false;
};

std::string clock_time(uint64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[8];
    std::strftime(buf, sizeof buf, "%H:%M", &tm);
    return buf;
}

// "30s", "5m", "2h", "1d" or a bare number of seconds. Returns 0 if not understood.
double parse_duration(const std::string& s) {
    if (s.empty()) return 0;
    char unit = s.back();
    double scale = unit == 's' ? 1 : unit == 'm' ? 60 : unit == 'h' ? 3600 : unit == 'd' ? 86400 : 0;
    std::string digits = scale > 0 ? s.substr(0, s.size() - 1) : s;
    if (scale == 0) scale = 1;
    if (digits.empty() || digits.find_first_not_of("0123456789.") != std::string::npos) return 0;
    return std::atof(digits.c_str()) * scale;
}

std::string describe_duration(uint64_t seconds) {
    if (seconds % 86400 == 0) return std::to_string(seconds / 86400) + "d";
    if (seconds % 3600 == 0) return std::to_string(seconds / 3600) + "h";
    if (seconds % 60 == 0) return std::to_string(seconds / 60) + "m";
    return std::to_string(seconds) + "s";
}

std::string snippet(const std::string& s, size_t n = 40) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

std::atomic<bool> g_screen_open{false};

uint64_t now_ms_() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// Whether a message calls for the attention of `username`: "@name", or
// "@everyone", not as part of a longer word.
bool mentions_user(const std::string& body, std::string username) {
    auto lower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    auto word = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-'; };
    std::string text = lower(body);
    username = lower(username);
    if (username.empty()) return false;
    for (const std::string& name : {username, std::string("everyone")}) {
        size_t at = 0;
        while ((at = text.find("@" + name, at)) != std::string::npos) {
            size_t end = at + 1 + name.size();
            bool starts = at == 0 || (!word(text[at - 1]) && text[at - 1] != '@');
            if (starts && (end == text.size() || !word(text[end]))) return true;
            at = end;
        }
    }
    return false;
}

// A wrapped paragraph in which web addresses are links: terminals that
// support it open them on click, the rest show the address as plain text.
Element linked_paragraph(const std::string& s) {
    Elements words;
    std::istringstream in(s);
    std::string word;
    while (in >> word) {
        bool link = word.rfind("https://", 0) == 0 || word.rfind("http://", 0) == 0;
        if (!link) {
            words.push_back(text(word));
            continue;
        }
        // Punctuation that ends a sentence is not part of the address.
        std::string url = word;
        while (!url.empty() && std::string(".,;:!?)\"'").find(url.back()) != std::string::npos) url.pop_back();
        words.push_back(hyperlink(url, text(url) | underlined));
        if (url.size() < word.size()) words.back() = hbox({words.back(), text(word.substr(url.size()))});
    }
    return flexbox(std::move(words), FlexboxConfig().SetGap(1, 0));
}

class TuiApp {
public:
    TuiApp(std::string vault_dir, std::string server, std::string name, std::string fingerprint,
           std::string invite, std::string join_link, std::string recovery_key)
        : join_link_(std::move(join_link)),
          recovery_key_(std::move(recovery_key)),
          vault_dir_(std::move(vault_dir)),
          server_(std::move(server)),
          name_(std::move(name)),
          fingerprint_(std::move(fingerprint)),
          invite_(std::move(invite)) {}

    int run() {
        corded_config cfg{};
        cfg.struct_size = sizeof cfg;
        cfg.vault_dir = vault_dir_.c_str();
        if (corded_engine_create(&cfg, &engine_) != CORDED_OK) {
            std::fprintf(stderr, "could not start the engine\n");
            return 1;
        }
        int32_t exists = 0;
        corded_vault_exists(engine_, &exists);
        creating_ = exists == 0;

        build_ui();

        std::thread pump([this] {
            while (running_) {
                const char* text = nullptr;
                if (corded_next_event(engine_, 100, &text, nullptr) != CORDED_OK) {
                    if (typing_until_.load() > now_ms_() && ++idle_ticks_ % 10 == 0) screen_.PostEvent(Event::Custom);
                    continue;
                }
                json ev = json::parse(text, nullptr, false);
                corded_event_free(text);
                if (ev.is_discarded()) continue;
                screen_.Post([this, ev = std::move(ev)] { on_event(ev); });
                screen_.PostEvent(Event::Custom);
            }
        });

#ifndef _WIN32
        // SIGTERM and SIGHUP were blocked in main(), so this thread is the only
        // one that sees them. It asks the screen to close, and the normal
        // shutdown below then locks the vault properly.
        std::thread([this] {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGTERM);
            sigaddset(&set, SIGHUP);
            int sig = 0;
            sigwait(&set, &sig);
            if (!g_screen_open) return;
            screen_.Post([this] { screen_.Exit(); });
            screen_.PostEvent(Event::Custom);
        }).detach();
#endif
        g_screen_open = true;
        screen_.Loop(root_);
        g_screen_open = false;
        running_ = false;
        pump.join();
        corded_engine_destroy(engine_);
        return 0;
    }

private:
    // ------------------------------------------------------------ engine I/O
    corded_request command(json cmd) {
        // Commands that are not about a particular chat go to the server on screen.
        std::string name = cmd.value("cmd", "");
        if (current_server_ != 0 && !cmd.contains("room_id") && !cmd.contains("server_id") &&
            name != "connect" && name != "list_servers" && name != "status" && name != "set_history_sharing")
            cmd["server_id"] = current_server_;
        std::string text = cmd.dump();
        corded_request req = 0;
        corded_command(engine_, text.data(), text.size(), &req);
        return req;
    }

    void on_event(const json& ev) {
        std::string kind = ev.value("event", "");
        if (kind == "vault_state") {
            if (ev.value("state", "") == "unlocked") {
                username_ = ev.value("username", "");
                // --name on an existing vault: ask to use it. The core refuses if a
                // server already knows this vault by its old name.
                if (!name_.empty() && name_ != username_ && !rename_tried_) {
                    rename_tried_ = true;
                    command({{"cmd", "set_username"}, {"username", name_}});
                }
                phase_ = 1;
                input_->TakeFocus();
                if (!join_link_.empty()) command({{"cmd", "connect"}, {"link", join_link_}});
                else if (!server_.empty()) connect_to(server_);
            }
        } else if (kind == "connection_state") {
            int64_t sid = ev.value("server_id", int64_t{0});
            ServerState& st = servers_[sid];
            st.connection = ev.value("state", "");
            if (ev.contains("server")) st.address = ev.value("server", "");
            if (ev.contains("fingerprint")) st.fingerprint = ev.value("fingerprint", "");
            if (current_server_ == 0) current_server_ = sid;
            if (ev.contains("detail")) {
                notice_ = (sid == current_server_ ? "" : server_label(sid) + ": ") + ev.value("detail", "");
                // Say it in the client's own terms.
                auto hint = notice_.find("(set_username)");
                if (hint != std::string::npos) notice_.replace(hint, 14, "with /username <name>");
            }
            else if (sid == current_server_ && st.connection == "live") notice_.clear();
            apply_current();
        } else if (kind == "room_updated") {
            const json& r = ev.at("room");
            std::string id = r.value("room_id", "");
            {
                Room& room = room_for(id);
                room.title = r.value("title", "?");
                room.kind = r.value("kind", "direct");
                room.server_id = r.value("server_id", int64_t{0});
                room.disappear_after = r.value("disappear_after", uint64_t{0});
                room.unread = r.value("unread", room.unread);
            }
            if (members_selected()) command({{"cmd", "member_list"}});
            refresh_titles();  // re-sorts, so look the room up again
            Room& room = room_for(id);
            if (!room.loaded) {
                room.loaded = true;
                command({{"cmd", "fetch_timeline"}, {"room_id", room.id}, {"limit", 500}});
                command({{"cmd", "fetch_receipts"}, {"room_id", room.id}});
            }
        } else if (kind == "server_info") {
            int64_t sid = ev.value("server_id", int64_t{0});
            ServerState& st = servers_[sid];
            st.name = ev.value("name", "");
            if (ev.contains("address")) st.address = ev.value("address", "");
            st.is_owner = ev.value("is_owner", false);
            st.permissions.clear();
            for (const auto& p : ev.value("my_permissions", json::array())) st.permissions.insert(p.get<std::string>());
            st.roles = ev.value("roles", json::array());
            if (current_server_ == 0) current_server_ = sid;
            apply_current();
            refresh_titles();
        } else if (kind == "presence" || kind == "presence_reset") {
            if (members_selected()) command({{"cmd", "member_list"}});  // the page shows who is around
        } else if (kind == "server_removed") {
            int64_t gone = ev.value("server_id", int64_t{0});
            servers_.erase(gone);
            rooms_.erase(std::remove_if(rooms_.begin(), rooms_.end(), [&](const Room& r) { return r.server_id == gone; }),
                         rooms_.end());
            if (current_server_ == gone) current_server_ = servers_.empty() ? 0 : servers_.begin()->first;
            visible_.clear();
            selected_ = 0;
            refresh_titles();
        } else if (kind == "room_removed") {
            if (ev.value("reason", "") == "removed") notice_ = "you were removed from a chat";
            std::string id = ev.value("room_id", "");
            rooms_.erase(std::remove_if(rooms_.begin(), rooms_.end(), [&](const Room& r) { return r.id == id; }),
                         rooms_.end());
            visible_.clear();
            selected_ = 0;
            refresh_titles();
        } else if (kind == "event_received") {
            const json& d = ev.at("data");
            Room& room = room_for(d.value("room_id", ""));
            bool is_new = add_event(room, d);
            room.typing.erase(d.value("sender_name", ""));  // they sent it; no longer typing
            if (is_new && !d.value("mine", false) && current() != &room) {
                room.unread = ev.value("unread", room.unread + 1);
                refresh_titles();
            }
            if (current() == &room) mark_read(room);
        } else if (kind == "event_expired") {
            // A disappearing message's time came: it is gone from the vault.
            Room& room = room_for(ev.value("room_id", ""));
            std::string id = ev.value("event_id", "");
            room.messages.erase(std::remove_if(room.messages.begin(), room.messages.end(),
                                               [&](const Message& m) { return m.event_id == id; }),
                                room.messages.end());
            room.reactions.erase(id);
        } else if (kind == "event_updated") {
            // An edit or a deletion changed an existing message.
            const json& d = ev.at("data");
            Room& room = room_for(d.value("room_id", ""));
            std::string id = d.value("event_id", "");
            for (auto& m : room.messages) {
                if (m.event_id != id) continue;
                m.status = d.value("status", m.status);
                m.edited = d.value("edited", false);
        m.disappearing = d.value("expires_at", uint64_t{0}) != 0;
                m.body = m.status == "redacted" ? "[deleted]" : d["content"].value("body", m.body);
            }
            for (auto& [target, keys] : room.reactions)
                for (auto& [key, ids] : keys) ids.erase(id);
        } else if (kind == "event_send_status") {
            Room& room = room_for(ev.value("room_id", ""));
            for (auto& m : room.messages)
                if (m.event_id == ev.value("event_id", ""))
                    m.status = ev.value("status", "") == "sent" ? "ok" : ev.value("status", "");
            if (ev.value("status", "") == "failed") notice_ = "send failed: " + ev.value("message", "");
        } else if (kind == "command_result") {
            on_result(ev);
        } else if (kind == "server_pinned") {
            pin_notice_ = "First connection to this server. Its key " + ev.value("fingerprint", "") +
                          " is now remembered.";
        } else if (kind == "typing") {
            Room& room = room_for(ev.value("room_id", ""));
            uint64_t until = now_ms_() + 5000;
            room.typing[ev.value("display_name", "someone")] = until;
            typing_until_ = until;
        } else if (kind == "receipt") {
            Room& room = room_for(ev.value("room_id", ""));
            if (ev.value("username", "") != username_)
                room.read_upto[ev.value("display_name", "someone")] = ev.value("event_id", "");
        } else if (kind == "server_notice") {
            notice_ = ev.value("message", "");
        } else if (kind == "history_received") {
            notice_ = std::to_string(ev.value("count", 0)) + " earlier messages were shared with you";
        } else if (kind == "warning") {
            notice_ = ev.value("message", "");
        }
    }

    void on_result(const json& ev) {
        uint64_t req = ev.value("request", uint64_t{0});
        bool ok = ev.value("ok", false);
        std::string message = ok ? "" : ev["error"].value("message", "error");
        if (req == login_request_) {
            busy_ = false;
            if (!ok) login_error_ = message;
            return;
        }
        if (req == join_request_) {
            if (!ok) notice_ = message;
            else switch_server(ev["data"].value("server_id", int64_t{0}));
            return;
        }
        if (req == chat_request_) {
            if (!ok) {
                notice_ = message;
                return;
            }
            std::string id = ev["data"]["room"].value("room_id", "");
            room_for(id).title = ev["data"]["room"].value("title", "?");
            refresh_titles();
            select_room(id);
            return;
        }
        if (ok && ev["data"].contains("recovery_key")) {
            info_box_ = vbox({
                            text("Your recovery key") | bold,
                            text("  " + ev["data"].value("recovery_key", "")) | color(Color::Cyan),
                            text("Use it to set up another device as you:") | dim,
                            text("  corded-tui --vault <new folder> --name " + username_ + " --recovery-key '<the key>' --server ...") | dim,
                            text("Anyone who has this key can become you. Do not share it or leave it on screen.") |
                                color(Color::Red),
                        }) |
                        border;
            show_info_ = true;
            return;
        }
        if (ok && ev["data"].contains("receipts")) {
            Room& room = room_for(ev["data"].value("room_id", ""));
            for (const auto& r : ev["data"]["receipts"])
                if (r.value("username", "") != username_ && r.contains("display_name"))
                    room.read_upto[r.value("display_name", "")] = r.value("event_id", "");
            return;
        }
        if (ok && ev["data"].contains("client_settings")) {
            client_settings_ = ev["data"]["client_settings"];
            return;
        }
        if (settings_selected() && ok && ev["data"].contains("settings")) {
            server_settings_ = ev["data"]["settings"];
            return;
        }
        if (settings_selected() && ok && ev["data"].contains("status")) {
            server_status_ = ev["data"]["status"];
            return;
        }
        if (settings_selected() && !ok && (req == settings_request_ || req == status_request_)) return;  // not permitted; the page just omits it
        if (ok && ev["data"].contains("settings")) {
            Elements rows = {text("Server settings") | bold};
            for (const auto& st : ev["data"]["settings"]) {
                rows.push_back(hbox({text(st.value("key", "") + " = ") | bold, text(st.value("value", "")) | color(Color::Cyan),
                                     text(st.value("owner_only", false) ? "  (owner only)" : "") | color(Color::Magenta),
                                     text(st.value("needs_restart", false) ? "  (needs /reboot)" : "") | dim}));
                rows.push_back(text("    " + st.value("description", "")) | dim);
            }
            rows.push_back(text("/set <name> <value> changes one") | dim);
            info_box_ = vbox(std::move(rows)) | border;
            show_info_ = true;
            return;
        }
        if (ok && ev["data"].contains("status")) {
            const json& st = ev["data"]["status"];
            uint64_t up = st.value("started_at", uint64_t{0});
            uint64_t now = static_cast<uint64_t>(std::time(nullptr)) * 1000;
            uint64_t tidy = st.value("last_housekeeping", uint64_t{0});
            info_box_ = vbox({
                            text("Server status") | bold,
                            text("version " + st.value("version", "?") + ", scope " + st.value("scope", "?")),
                            text("running for " + describe_duration(up && now > up ? (now - up) / 1000 : 0)),
                            text(std::to_string(st.value("members", 0)) + " members, " +
                                 std::to_string(st.value("online", 0)) + " online"),
                            text("storage used: " + std::to_string(st.value("stored_bytes", uint64_t{0}) / 1024) + " KB"),
                            text(tidy ? "last tidied " + describe_duration((now - tidy) / 1000) + " ago"
                                      : "not tidied yet since first start"),
                        }) |
                        border;
            show_info_ = true;
            return;
        }
        if (ok && ev["data"].contains("link")) {
            // The link is longer than most terminals are wide, so it is also
            // saved to a file where it can be copied in one piece.
            std::string link = ev["data"].value("link", "");
            std::string path = vault_dir_ + "/last-invite.txt";
            bool saved = false;
            if (FILE* f = std::fopen(path.c_str(), "w")) {
                saved = std::fputs((link + "\n").c_str(), f) >= 0;
                std::fclose(f);
            }
            Elements rows = {text("Invite link") | bold};
            for (size_t pos = 0; pos < link.size(); pos += 70) rows.push_back(text("  " + link.substr(pos, 70)));
            rows.push_back(text(saved ? "Saved in one piece to " + path : "(could not save it to a file)") | dim);
            rows.push_back(text("The person you invite starts their client with:") | dim);
            rows.push_back(text("  corded-tui --name <their name> --join '<the whole link>'") | dim);
            info_box_ = vbox(std::move(rows)) | border;
            show_info_ = true;
            return;
        }
        if (ok && ev["data"].contains("members")) {
            member_list_ = ev["data"]["members"];
            if (members_selected()) return;  // the page itself shows them
            Elements rows = {text("Members of " + (server_name_.empty() ? std::string("this server") : server_name_)) | bold};
            for (const auto& m : ev["data"]["members"]) {
                std::string roles;
                for (const auto& r : m.value("roles", json::array())) roles += " [" + r.get<std::string>() + "]";
                std::string shown = m.value("display_name", m.value("username", "?"));
                if (shown != m.value("username", "")) shown += " (" + m.value("username", "") + ")";
                rows.push_back(hbox({text(shown) | bold,
                                     text(m.value("is_owner", false) ? "  owner" : "") | color(Color::Magenta),
                                     text(roles) | color(Color::Cyan)}));
            }
            info_box_ = vbox(std::move(rows)) | border;
            show_info_ = true;
            return;
        }
        if (ok && ev["data"].contains("safety_numbers")) {
            verify_list_ = ev["data"]["safety_numbers"];
            show_verify_ = true;
            return;
        }
        if (ok && ev["data"].contains("events")) {
            Room& room = room_for(ev["data"].value("room_id", ""));
            for (const auto& d : ev["data"]["events"]) add_event(room, d);
            if (current() == &room) mark_read(room);
            return;
        }
        if (!ok) notice_ = message;
    }

    // Returns true if this event was not seen before.
    bool add_event(Room& room, const json& d) {
        std::string id = d.value("event_id", "");
        std::string type = d.value("type", "");
        std::string rel_kind, rel_target, rel_key;
        if (d.contains("relation")) {
            rel_kind = d["relation"].value("kind", "");
            rel_target = d["relation"].value("target", "");
            rel_key = d["relation"].value("key", "");
        }
        if (type == "m.reaction" && rel_kind == "annotation") {
            std::string key = rel_key.empty() ? d["content"].value("key", "?") : rel_key;
            if (d.value("mine", false) && d.value("status", "") != "redacted") room.my_reactions[rel_target][key] = id;
            return room.reactions[rel_target][key].insert(id).second;
        }
        // Edits and deletions are not lines of their own; they arrive again as
        // event_updated for the message they change.
        if (type == "m.edit" || type == "m.redaction" || type == "m.history.share") return false;
        for (auto& m : room.messages) {
            if (m.event_id != id) continue;
            m.status = d.value("status", m.status);
            return false;
        }
        Message m;
        m.num = room.next_num++;
        m.event_id = id;
        m.type = type;
        m.mine = d.value("mine", false);
        m.sender = d.value("sender_name", m.mine ? username_ : std::string("?"));
        m.status = d.value("status", "");
        m.ts = d.value("origin_ts", uint64_t{0});
        m.edited = d.value("edited", false);
        m.disappearing = d.value("expires_at", uint64_t{0}) != 0;
        m.from_history = d.value("shared_history", false);
        if (rel_kind == "reply") m.reply_to = rel_target;
        // A reply made inside a thread names what it quotes in its content.
        if (d.contains("content") && d["content"].is_object() && d["content"].contains("reply_to") &&
            d["content"]["reply_to"].is_string())
            m.reply_to = d["content"]["reply_to"].get<std::string>();
        if (rel_kind == "thread") m.thread_root = rel_target;
        if (m.status == "redacted") m.body = "[deleted]";
        else if (type == "m.text") m.body = d["content"].value("body", "");
        else if (type == "m.room.member")
            m.body = d["content"].value("action", "") == "left"
                         ? "left the chat"
                         : d["content"].value("action", "") == "removed"
                               ? "removed " + d["content"].value("username", "someone") + " from the chat"
                               : "added " + d["content"].value("username", "someone") + " to the chat";
        else if (type == "m.room.retention") {
            uint64_t ttl = d["content"].value("ttl_seconds", uint64_t{0});
            m.body = ttl ? "set messages here to disappear after " + describe_duration(ttl)
                         : "turned off disappearing messages";
        } else if (type == "m.room.name") m.body = "named this chat \"" + d["content"].value("name", "") + "\"";
        else if (m.status == "undecryptable") m.body = "[could not decrypt this message]";
        else if (d.contains("fallback_text")) m.body = d.value("fallback_text", "");
        else m.body = "[unsupported message type: " + type + "]";
        // Shared history is older than what is on screen: keep time order.
        if (m.from_history) {
            auto pos = std::find_if(room.messages.begin(), room.messages.end(),
                                    [&](const Message& other) { return other.ts > m.ts; });
            room.messages.insert(pos, std::move(m));
        } else {
            room.messages.push_back(std::move(m));
        }
        return true;
    }

    // ------------------------------------------------------------ model
    Room& room_for(const std::string& id) {
        for (auto& r : rooms_)
            if (r.id == id) return r;
        Room fresh;
        fresh.id = id;
        fresh.title = "...";
        rooms_.push_back(std::move(fresh));
        refresh_titles();
        for (auto& r : rooms_)
            if (r.id == id) return r;
        return rooms_.back();
    }
    // The chat on screen: one of the current server's chats.
    Room* current() {
        // After the last chat come two pages: Members, then Settings.
        selected_ = std::clamp(selected_, 0, static_cast<int>(visible_.size()) + 1);
        if (members_selected() || settings_selected()) return nullptr;
        return &rooms_[visible_[static_cast<size_t>(selected_)]];
    }
    bool members_selected() const { return selected_ == static_cast<int>(visible_.size()); }
    bool settings_selected() const { return selected_ == static_cast<int>(visible_.size()) + 1; }
    // Tells the others in a chat that we have read up to its newest message.
    void mark_read(Room& room) {
        for (auto it = room.messages.rbegin(); it != room.messages.rend(); ++it) {
            if (it->mine || it->from_history) continue;
            if (it->event_id != room.last_marked) {
                room.last_marked = it->event_id;
                command({{"cmd", "mark_read"}, {"room_id", room.id}, {"event_id", it->event_id}});
            }
            return;
        }
    }
    void load_settings_page() {
        command({{"cmd", "client_settings"}});
        settings_request_ = command({{"cmd", "get_settings"}});
        status_request_ = command({{"cmd", "server_status"}});
    }
    std::string current_id() {
        Room* r = current();
        return r ? r->id : std::string();
    }
    std::string server_label(int64_t sid) {
        auto it = servers_.find(sid);
        if (it == servers_.end()) return "server " + std::to_string(sid);
        return !it->second.name.empty() ? it->second.name : it->second.address;
    }
    // Copies the current server's details into the fields the views read.
    void apply_current() {
        servers_.erase(0);  // 0 means "no server yet"; never list it
        auto it = servers_.find(current_server_);
        if (it == servers_.end()) return;
        const ServerState& st = it->second;
        server_name_ = st.name;
        is_owner_ = st.is_owner;
        permissions_ = st.permissions;
        roles_ = st.roles;
        connection_ = st.connection;
        server_shown_ = st.address;
        server_fp_ = st.fingerprint;
    }
    // The chat list shows the current server only: channels first, then the
    // rest, each alphabetical. The open chat stays open if it moves.
    void refresh_titles() {
        std::string open = current_id();
        std::stable_sort(rooms_.begin(), rooms_.end(), [](const Room& a, const Room& b) {
            bool ac = a.kind == "channel", bc = b.kind == "channel";
            return ac != bc ? ac : a.title < b.title;
        });
        titles_.clear();
        visible_.clear();
        for (size_t i = 0; i < rooms_.size(); ++i) {
            const Room& r = rooms_[i];
            if (current_server_ != 0 && r.server_id != current_server_) continue;
            if (r.id == open) selected_ = static_cast<int>(visible_.size());
            visible_.push_back(i);
            titles_.push_back(r.title + (r.unread > 0 ? " (" + std::to_string(r.unread) + ")" : ""));
        }
        titles_.push_back("-- members --");
        titles_.push_back("-- settings --");
        if (open.empty() && page_ > 0) selected_ = static_cast<int>(visible_.size()) + page_ - 1;
    }
    void select_room(const std::string& id) {
        for (const auto& r : rooms_)
            if (r.id == id && r.server_id != current_server_ && r.server_id != 0) switch_server(r.server_id);
        for (size_t i = 0; i < visible_.size(); ++i)
            if (rooms_[visible_[i]].id == id) selected_ = static_cast<int>(i);
        on_room_selected();
    }
    void on_room_selected() {
        page_ = members_selected() ? 1 : settings_selected() ? 2 : 0;
        if (page_ == 1) command({{"cmd", "member_list"}});  // refresh the page
        if (page_ == 2) {
            server_settings_ = json::array();
            server_status_ = json::object();
            load_settings_page();
        }
        if (Room* r = current()) mark_read(*r);
        if (Room* r = current(); r && r->unread > 0) {
            r->unread = 0;
            refresh_titles();
        }
    }
    void switch_server(int64_t sid) {
        if (sid == 0 || sid == current_server_) return;
        open_by_server_[current_server_] = current_id();
        current_server_ = sid;
        visible_.clear();
        selected_ = 0;
        apply_current();
        refresh_titles();
        std::string remembered = open_by_server_[sid];
        for (size_t i = 0; i < visible_.size(); ++i)
            if (rooms_[visible_[i]].id == remembered) selected_ = static_cast<int>(i);
        on_room_selected();
    }
    int unread_elsewhere() {
        int n = 0;
        for (const auto& r : rooms_)
            if (r.server_id != current_server_) n += r.unread;
        return n;
    }
    // Takes a leading message number off `arg` ("12 text" or "#12 text") when
    // this chat has a message with that number, and returns the message.
    const Message* take_target(Room& room, std::string& arg) {
        size_t sp = arg.find(' ');
        std::string first = arg.substr(0, sp);
        if (!first.empty() && first[0] == '#') first.erase(0, 1);
        if (first.empty() || first.size() > 6 || first.find_first_not_of("0123456789") != std::string::npos)
            return nullptr;
        int n = std::atoi(first.c_str());
        for (const auto& m : room.messages)
            if (m.num == n) {
                arg = sp == std::string::npos ? "" : arg.substr(sp + 1);
                return &m;
            }
        return nullptr;
    }
    void connect_to(const std::string& server) {
        auto colon = server.rfind(':');
        std::string host = colon == std::string::npos ? server : server.substr(0, colon);
        int port = colon == std::string::npos ? 7443 : std::atoi(server.substr(colon + 1).c_str());
        json cmd = {{"cmd", "connect"}, {"host", host}, {"port", port}};
        if (!fingerprint_.empty()) cmd["fingerprint"] = fingerprint_;
        if (!invite_.empty()) cmd["invite"] = invite_;
        command(cmd);
    }

    // ------------------------------------------------------------ actions
    void submit_login() {
        if (busy_) return;
        login_error_.clear();
        if (creating_) {
            if (name_.empty()) {
                login_error_ = "choose a username";
                return;
            }
            if (pass_.size() < 8) {
                login_error_ = "use a passphrase of at least 8 characters";
                return;
            }
            if (pass_ != pass2_) {
                login_error_ = "the two passphrases do not match";
                return;
            }
        }
        if (pass_.empty()) return;
        auto* p = reinterpret_cast<const uint8_t*>(pass_.data());
        busy_ = true;
        corded_status st =
            !creating_ ? corded_vault_unlock(engine_, p, pass_.size(), &login_request_)
            : !recovery_key_.empty()
                ? corded_vault_restore(engine_, p, pass_.size(), name_.c_str(), recovery_key_.c_str(), &login_request_)
                : corded_vault_create(engine_, p, pass_.size(), name_.c_str(), &login_request_);
        if (st != CORDED_OK) {
            busy_ = false;
            login_error_ = corded_status_message(st);
        }
        std::fill(pass_.begin(), pass_.end(), '\0');
        std::fill(pass2_.begin(), pass2_.end(), '\0');
        pass_.clear();
        pass2_.clear();
    }

    const Message* last_from_other(Room& room) {
        for (auto it = room.messages.rbegin(); it != room.messages.rend(); ++it)
            if (!it->mine) return &*it;
        return nullptr;
    }

    void submit_input() {
        std::string line = input_text_;
        input_text_.clear();
        if (line.empty()) return;
        notice_.clear();
        pin_notice_.clear();
        show_verify_ = false;
        show_info_ = false;
        Room* room = current();

        if (line[0] == '/') {
            auto space = line.find(' ');
            std::string cmd = line.substr(0, space);
            std::string arg = space == std::string::npos ? "" : line.substr(space + 1);
            if (cmd == "/quit" || cmd == "/exit" || cmd == "/q") {
                screen_.Exit();
            } else if (room && (cmd == "/reply" || cmd == "/thread" || cmd == "/react" || cmd == "/edit" ||
                                cmd == "/delete" || cmd == "/remove")) {
                // These act on one message. Give its number ("/reply 12 agreed"), or
                // leave it out to mean the latest: the last one received, or for
                // /edit and /delete the last one you sent.
                std::string rest = arg;
                const Message* target = take_target(*room, rest);
                bool own_default = cmd == "/edit" || cmd == "/delete";
                if (!target && own_default) {
                    for (auto it = room->messages.rbegin(); it != room->messages.rend() && !target; ++it)
                        if (it->mine && it->type == "m.text" && it->status == "ok") target = &*it;
                } else if (!target) {
                    target = last_from_other(*room);
                }
                bool needs_text = cmd != "/delete" && cmd != "/remove";
                if (!target) notice_ = "there is no message for that yet";
                else if (needs_text && rest.empty()) notice_ = "use: " + cmd + " [message number] <text>";
                else if (cmd == "/edit" && !target->mine) notice_ = "you can only edit your own messages";
                else if (cmd == "/reply")
                    command({{"cmd", "send_text"}, {"room_id", room->id}, {"body", rest}, {"reply_to", target->event_id}});
                else if (cmd == "/thread")
                    command({{"cmd", "send_text"}, {"room_id", room->id}, {"body", rest},
                             {"thread", target->thread_root.empty() ? target->event_id : target->thread_root}});
                else if (cmd == "/react" && room->my_reactions[target->event_id].count(rest) &&
                         room->reactions[target->event_id][rest].count(room->my_reactions[target->event_id][rest])) {
                    // The same reaction again takes it back.
                    command({{"cmd", "delete_event"}, {"room_id", room->id},
                             {"event_id", room->my_reactions[target->event_id][rest]}});
                    room->my_reactions[target->event_id].erase(rest);
                } else if (cmd == "/react")
                    command({{"cmd", "send_event"}, {"room_id", room->id}, {"type", "m.reaction"},
                             {"content", {{"key", rest}}},
                             {"relation", {{"kind", "annotation"}, {"target", target->event_id}, {"key", rest}}}});
                else if (cmd == "/edit")
                    command({{"cmd", "edit_event"}, {"room_id", room->id}, {"event_id", target->event_id}, {"body", rest}});
                else  // /delete and /remove: your own message, or as a moderator someone else's
                    command({{"cmd", "delete_event"}, {"room_id", room->id}, {"event_id", target->event_id}});
            } else if (cmd == "/servers" || (cmd == "/server" && (arg.empty() || arg == "list"))) {
                Elements rows = {text("Servers") | bold};
                int n = 1;
                for (const auto& [sid, st] : servers_) {
                    int unread = 0;
                    for (const auto& r : rooms_)
                        if (r.server_id == sid) unread += r.unread;
                    rows.push_back(hbox({
                        text((sid == current_server_ ? "* " : "  ") + std::to_string(n++) + "  ") | dim,
                        text(st.name.empty() ? "(connecting)" : st.name) | bold,
                        text("  " + st.address + "  ") | dim,
                        text(st.connection) | color(st.connection == "live" ? Color::Green : Color::Yellow),
                        text(st.is_owner ? "  owner" : "") | color(Color::Magenta),
                        text(unread ? "  " + std::to_string(unread) + " unread" : "") | color(Color::Yellow),
                    }));
                }
                rows.push_back(text("/server switch <name or number>   /server leave <name or number>   /server join <link or host:port>") | dim);
                info_box_ = vbox(std::move(rows)) | border;
                show_info_ = true;
            } else if (cmd == "/server") {
                auto sp = arg.find(' ');
                std::string sub = arg.substr(0, sp), rest = sp == std::string::npos ? "" : arg.substr(sp + 1);
                if ((sub == "switch" || sub == "leave") && !rest.empty()) {
                    int64_t found = 0;
                    int n = 1;
                    for (const auto& [sid, st] : servers_) {
                        std::string lowered = st.name, want = rest;
                        for (auto& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        for (auto& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        if (!found && (want == std::to_string(n) || (!lowered.empty() && lowered.find(want) != std::string::npos)))
                            found = sid;
                        ++n;
                    }
                    if (found && sub == "leave") {
                        // Off this device only; the account on the server stays.
                        command({{"cmd", "forget_server"}, {"server_id", found}});
                        notice_ = "left " + servers_[found].name + " on this device";
                    } else if (found) switch_server(found);
                    else notice_ = "no server matches \"" + rest + "\"; /servers lists them";
                } else if (sub == "join" && !rest.empty()) {
                    // An invite link, or a plain address for a server that needs no invite.
                    if (rest.rfind("corded://", 0) == 0) {
                        join_request_ = command({{"cmd", "connect"}, {"link", rest}});
                    } else {
                        auto colon = rest.rfind(':');
                        join_request_ = command({{"cmd", "connect"},
                                                 {"host", colon == std::string::npos ? rest : rest.substr(0, colon)},
                                                 {"port", colon == std::string::npos ? 7443 : std::atoi(rest.substr(colon + 1).c_str())}});
                    }
                } else {
                    notice_ = "/servers | /server switch <name or number> | /server join <invite link or host:port>";
                }
            } else if (cmd == "/chat" && !arg.empty()) {
                chat_request_ = command({{"cmd", "start_chat"}, {"username", arg}});
            } else if (cmd == "/group" && !arg.empty()) {
                // "/group bob carol" or "/group bob carol : Weekend plans"
                std::string people = arg, room_name;
                if (auto colon = arg.find(':'); colon != std::string::npos) {
                    people = arg.substr(0, colon);
                    room_name = arg.substr(colon + 1);
                    room_name.erase(0, room_name.find_first_not_of(' '));
                }
                json names = json::array();
                size_t pos = 0;
                while (pos < people.size()) {
                    size_t end = people.find(' ', pos);
                    if (end == std::string::npos) end = people.size();
                    if (end > pos) names.push_back(people.substr(pos, end - pos));
                    pos = end + 1;
                }
                json c = {{"cmd", "create_room"}, {"usernames", names}};
                if (!room_name.empty()) c["name"] = room_name;
                chat_request_ = command(c);
            } else if (cmd == "/open" && !arg.empty()) {
                // Open the first chat whose name contains the text.
                auto lower = [](std::string v) {
                    for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return v;
                };
                bool found = false;
                for (size_t vi : visible_) {
                    const Room& r = rooms_[vi];
                    if (!found && lower(r.title).find(lower(arg)) != std::string::npos) {
                        std::string id = r.id;
                        select_room(id);
                        found = true;
                    }
                }
                if (!found) notice_ = "no chat matches \"" + arg + "\"";
            } else if (cmd == "/remove-account" && !arg.empty()) {
                command({{"cmd", "remove_account"}, {"username", arg}});
            } else if (cmd == "/kick" && !arg.empty()) {
                command({{"cmd", "kick"}, {"username", arg}});
            } else if ((cmd == "/ban" || cmd == "/unban") && !arg.empty()) {
                command({{"cmd", "ban_user"}, {"username", arg}, {"banned", cmd == "/ban"}});
            } else if (cmd == "/invite") {
                // "/invite" for an unlimited link, "/invite 3" for three uses.
                json c = {{"cmd", "create_invite"}};
                if (!arg.empty()) c["max_uses"] = std::atoi(arg.c_str());
                command(c);
            } else if (cmd == "/nick") {
                // Your own display name on this server; no name clears it.
                command({{"cmd", "set_nickname"}, {"nickname", arg}});
                if (members_selected()) command({{"cmd", "member_list"}});
            } else if (cmd == "/setnick" && !arg.empty()) {
                auto sp = arg.find(' ');
                command({{"cmd", "set_nickname"}, {"username", arg.substr(0, sp)},
                         {"nickname", sp == std::string::npos ? "" : arg.substr(sp + 1)}});
                if (members_selected()) command({{"cmd", "member_list"}});
            } else if (cmd == "/username" && !arg.empty()) {
                command({{"cmd", "set_username"}, {"username", arg}});
            } else if (cmd == "/recovery-key") {
                command({{"cmd", "get_recovery_key"}});
            } else if (cmd == "/settings") {
                selected_ = static_cast<int>(visible_.size()) + 1;
                on_room_selected();
            } else if (cmd == "/set" && arg.find(' ') != std::string::npos) {
                auto sp = arg.find(' ');
                command({{"cmd", "set_setting"}, {"key", arg.substr(0, sp)}, {"value", arg.substr(sp + 1)}});
                if (settings_selected()) load_settings_page();
            } else if (cmd == "/reboot") {
                command({{"cmd", "restart_server"}});
            } else if (cmd == "/presence" && (arg == "auto" || arg == "dnd" || arg == "invisible")) {
                command({{"cmd", "set_presence"}, {"status", arg}});
                notice_ = arg == "auto" ? "others see you as online while this is open"
                          : arg == "dnd" ? "others see you as not to be disturbed"
                                         : "others see you as offline";
            } else if (cmd == "/presence") {
                notice_ = "/presence auto | dnd | invisible";
            } else if (cmd == "/status") {
                command({{"cmd", "server_status"}});
            } else if (cmd == "/members") {
                selected_ = static_cast<int>(visible_.size());
                on_room_selected();
            } else if (cmd == "/roles") {
                Elements rows = {text("Roles") | bold};
                for (const auto& r : roles_) {
                    std::string perms;
                    for (const auto& p : r.value("permissions", json::array())) perms += " " + p.get<std::string>();
                    rows.push_back(hbox({text(r.value("name", "?")) | bold | color(Color::Cyan), text(perms) | dim}));
                }
                info_box_ = vbox(std::move(rows)) | border;
                show_info_ = true;
            } else if (cmd == "/channel" && !arg.empty()) {
                // /channel new <name> | rename <name> | delete | private <role> | readonly | open
                auto sp = arg.find(' ');
                std::string sub = arg.substr(0, sp), rest = sp == std::string::npos ? "" : arg.substr(sp + 1);
                bool on_channel = room && room->kind == "channel";
                if (sub == "new" && !rest.empty()) {
                    command({{"cmd", "create_channel"}, {"name", rest}});
                } else if (!on_channel) {
                    notice_ = "open a channel first";
                } else if (sub == "rename" && !rest.empty()) {
                    command({{"cmd", "rename_channel"}, {"room_id", room->id}, {"name", rest}});
                } else if (sub == "delete") {
                    command({{"cmd", "delete_channel"}, {"room_id", room->id}});
                } else if (sub == "private" && !rest.empty()) {
                    command({{"cmd", "set_channel_access"}, {"room_id", room->id}, {"role", "@everyone"},
                             {"deny", {"view_channel"}}});
                    command({{"cmd", "set_channel_access"}, {"room_id", room->id}, {"role", rest},
                             {"allow", {"view_channel"}}});
                } else if (sub == "readonly") {
                    command({{"cmd", "set_channel_access"}, {"room_id", room->id}, {"role", "@everyone"},
                             {"deny", {"send_messages"}}});
                } else if (sub == "open") {
                    command({{"cmd", "set_channel_access"}, {"room_id", room->id}, {"role", "@everyone"}});
                } else {
                    notice_ = "/channel new <name> | rename <name> | delete | private <role> | readonly | open";
                }
            } else if (cmd == "/role" && !arg.empty()) {
                // /role new <name> [permission ...] | delete <name> | give <user> <role> | take <user> <role>
                std::vector<std::string> words;
                for (size_t pos = 0; pos < arg.size();) {
                    size_t end = arg.find(' ', pos);
                    if (end == std::string::npos) end = arg.size();
                    if (end > pos) words.push_back(arg.substr(pos, end - pos));
                    pos = end + 1;
                }
                if (words.size() >= 2 && words[0] == "new") {
                    json perms = json::array();
                    for (size_t i = 2; i < words.size(); ++i) perms.push_back(words[i]);
                    command({{"cmd", "create_role"}, {"name", words[1]}, {"permissions", perms}});
                } else if (words.size() == 2 && words[0] == "delete") {
                    command({{"cmd", "delete_role"}, {"role", words[1]}});
                } else if (words.size() == 3 && (words[0] == "give" || words[0] == "take")) {
                    command({{"cmd", "grant_role"}, {"username", words[1]}, {"role", words[2]},
                             {"grant", words[0] == "give"}});
                } else {
                    notice_ = "/role new <name> [permission ...] | delete <name> | give <user> <role> | take <user> <role>";
                }
            } else if (cmd == "/add" && !arg.empty() && room) {
                command({{"cmd", "add_member"}, {"room_id", room->id}, {"username", arg}});
            } else if (cmd == "/leave" && room) {
                // Say goodbye first, while we can still send to the room.
                command({{"cmd", "send_event"}, {"room_id", room->id}, {"type", "m.room.member"},
                         {"content", {{"action", "left"}}}});
                command({{"cmd", "leave_room"}, {"room_id", room->id}});
            } else if (cmd == "/name" && !arg.empty() && room) {
                command({{"cmd", "set_room_name"}, {"room_id", room->id}, {"name", arg}});
            } else if (cmd == "/disappear" && !arg.empty() && room) {
                // Every new message in this chat disappears after the given time.
                double seconds = arg == "off" ? 0 : parse_duration(arg);
                if (arg != "off" && seconds <= 0) notice_ = "use a time like 30s, 5m, 2h or 1d, or \"off\"";
                else command({{"cmd", "set_disappearing"}, {"room_id", room->id}, {"seconds", seconds}});
            } else if (cmd == "/once" && !arg.empty() && room) {
                // One message that disappears: /once 30s the text
                auto sp = arg.find(' ');
                double seconds = parse_duration(arg.substr(0, sp));
                if (sp == std::string::npos || seconds <= 0) notice_ = "use: /once 30s your message";
                else
                    command({{"cmd", "send_text"}, {"room_id", room->id}, {"body", arg.substr(sp + 1)},
                             {"expires_in", seconds}});
            } else if (cmd == "/history" && room) {
                command({{"cmd", "request_history"}, {"room_id", room->id}});
            } else if (cmd == "/receipts" && (arg == "on" || arg == "off")) {
                command({{"cmd", "set_read_receipts"}, {"enabled", arg == "on"}});
                notice_ = arg == "on" ? "others will see what you have read" : "others will no longer see what you have read";
                if (settings_selected()) load_settings_page();
            } else if (cmd == "/share-history" && (arg == "on" || arg == "off")) {
                command({{"cmd", "set_history_sharing"}, {"enabled", arg == "on"}});
                notice_ = arg == "on" ? "you will share earlier messages with newcomers who ask"
                                      : "you will not share earlier messages with newcomers";
            } else if (cmd == "/verify" && room) {
                command({{"cmd", "safety_numbers"}, {"room_id", room->id}});
            } else if ((cmd == "/verified" || cmd == "/unverified") && !arg.empty() && room) {
                bool found = false;
                for (const auto& entry : verify_list_) {
                    if (entry.value("username", "") != arg) continue;
                    found = true;
                    command({{"cmd", "set_verified"}, {"user_id", entry.value("user_id", "")},
                             {"verified", cmd == "/verified"}});
                    command({{"cmd", "safety_numbers"}, {"room_id", room->id}});
                }
                if (!found) notice_ = "run /verify first, then /verified <username>";
            } else if (cmd == "/connect" && !arg.empty()) {
                server_ = arg;
                connect_to(arg);
            } else if (cmd == "/help") {
                show_help_ = !show_help_;
            } else {
                notice_ = "unknown command; try /help";
            }
            return;
        }
        if (!room) {
            notice_ = settings_selected() ? "this is the settings page; open a chat to type (/open <name>)"
                      : members_selected() ? "this is the members page; open a chat to type (/open <name>)"
                                         : "start a chat first: /chat <username>";
            return;
        }
        command({{"cmd", "send_text"}, {"room_id", room->id}, {"body", line}});
    }

    // ------------------------------------------------------------ views
    Element login_view() {
        Elements rows;
        rows.push_back(text("corded") | bold | center);
        rows.push_back(text("pre-alpha prototype, not audited") | dim | center);
        rows.push_back(separator());
        if (creating_) {
            rows.push_back(text(recovery_key_.empty() ? "No vault here yet. Create your identity."
                                                      : "Setting up this device with your recovery key."));
            rows.push_back(text(vault_dir_) | dim);
            rows.push_back(hbox({text("username:   "), name_input_->Render()}));
            rows.push_back(hbox({text("passphrase: "), pass_input_->Render()}));
            rows.push_back(hbox({text("again:      "), pass2_input_->Render()}));
            rows.push_back(text("The passphrase cannot be recovered if you forget it.") | dim);
        } else {
            rows.push_back(text("Unlock your vault."));
            rows.push_back(text(vault_dir_) | dim);
            rows.push_back(hbox({text("passphrase: "), pass_input_->Render()}));
        }
        if (busy_) rows.push_back(text("working...") | dim);
        if (!login_error_.empty()) rows.push_back(text(login_error_) | color(Color::Red));
        rows.push_back(separator());
        rows.push_back(text("Enter to continue, Tab to move, Ctrl+C to quit") | dim);
        return vbox(std::move(rows)) | border | size(WIDTH, LESS_THAN, 70) | center;
    }

    // Help takes over the message area, so it never pushes the rest off screen.
    Element help_view() {
        return vbox({
                              text("/chat <username>   start or open a chat"),
                              text("/group a b c : Name  start a group chat (the name is optional)"),
                              text("/name <text>       rename the open chat"),
                              text("/add <username>    add someone to the open group        /leave  leave it"),
                              text("/verify            show safety numbers for the people in this chat"),
                              text("Messages have numbers. /reply, /thread, /react, /edit and /delete take one:"),
                              text("   /reply 12 agreed     /react 12 +1     /delete 12      (no number = the latest)"),
                              text("/servers           list your servers    /server switch <name>   /server join <link>"),
                              text("/reply <text>      reply to the last message you received"),
                              text("/thread <text>     reply in a thread under the last message you received"),
                              text("/react <emoji>     react to the last message you received"),
                              text("/edit <text>       change your last message        /delete  remove it"),
                              text("/once 30s <text>   a message that disappears        /disappear 1h|off  for the whole chat"),
                              text("/connect host:port connect to a server"),
                              text("/exit (or /quit)   leave        Tab: switch between chats and typing"),
                              text("/nick <name>       set your display name on this server"),
                              text("/username <name>   pick another name if yours was taken (before you have joined)"),
                              text("/recovery-key      show the key for setting up another device as you"),
                              text("/receipts on|off   whether others see what you have read"),
                              text("/presence auto|dnd|invisible   how others see you (the members page shows everyone)"),
                              text("/history           ask members for earlier messages     /share-history on|off"),
                              text("/open <name>       open a channel or chat by name      /members  /roles"),
                              text("running the server (needs the permission): /channel new|rename|delete|private|readonly|open"),
                              text("   /role new|delete|give|take      /kick <user>   /ban <user>   /unban <user>"),
                              text("   /settings   /set <name> <value>   /status   /reboot (restarts the server program)"),
                              text("   /setnick <user> <name>   change someone's display name"),
                              text("   /remove-account <user>   delete an account for good and free its name"),
                              text("   /invite [uses]   make an invite link for someone to join"),
                              text("   /remove   delete the last message someone else posted in this channel"),
                          }) | yframe | flex;
    }

    // Everyone on this server, with what can be done about them.
    Element members_view() {
        Elements rows;
        for (const auto& m : member_list_) {
            std::string username = m.value("username", "?");
            std::string shown = m.value("display_name", username);
            std::string roles;
            for (const auto& r : m.value("roles", json::array())) roles += " [" + r.get<std::string>() + "]";
            std::string status = m.value("status", "offline");
            Color dot = status == "online" ? Color::Green : status == "away" ? Color::Yellow
                        : status == "dnd" ? Color::Red : Color::GrayDark;
            rows.push_back(hbox({
                text(status == "offline" ? "o " : "* ") | color(dot),
                text(shown) | bold | color(m.value("me", false) ? Color::Cyan : Color::Green),
                text(status == "dnd" ? "  do not disturb" : status == "online" ? "" : "  " + status) | color(dot),
                text(shown != username ? "  (" + username + ")" : "") | dim,
                text(m.value("is_owner", false) ? "  owner" : m.value("is_admin", false) ? "  admin" : "") |
                    color(Color::Magenta),
                text(roles) | color(Color::Cyan),
                text(m.value("verified", false) ? "  (checked)" : "") | color(Color::Green),
            }));
        }
        if (rows.empty()) rows.push_back(text("Loading...") | dim);
        rows.push_back(text(""));
        rows.push_back(text("Use the name in brackets (the username) in these commands:") | dim);
        rows.push_back(text("  /nick <name>               set your own display name    (/nick alone clears it)") | dim);
        rows.push_back(text("  /chat <user>               message someone") | dim);
        if (is_owner_ || !permissions_.empty()) {
            rows.push_back(text("If your role allows it:") | dim);
            rows.push_back(text("  /setnick <user> <name>     change someone's display name") | dim);
            rows.push_back(text("  /role give <user> <role>   /role take <user> <role>") | dim);
            rows.push_back(text("  /kick <user>               remove them; they can rejoin") | dim);
            rows.push_back(text("  /ban <user>   /unban <user>") | dim);
            rows.push_back(text("  /remove-account <user>     delete the account and free its name") | dim);
        }
        return vbox(std::move(rows)) | yframe | flex;
    }

    // What can be changed, for you and (if you may) for the server.
    Element settings_view() {
        auto onoff = [](bool v) { return std::string(v ? "on" : "off"); };
        auto line = [](const std::string& name, const std::string& value, const std::string& how) {
            return hbox({text("  " + name + ": ") | bold, text(value) | color(Color::Cyan), text("    " + how) | dim});
        };
        Elements rows;
        rows.push_back(text("You") | bold | color(Color::Green));
        rows.push_back(line("username", client_settings_.value("username", username_), "fixed once a server knows you"));
        rows.push_back(line("display name", "set per server", "/nick <name>"));
        rows.push_back(line("share earlier messages with newcomers", onoff(client_settings_.value("share_history", true)),
                            "/share-history on|off"));
        rows.push_back(line("tell others what you have read", onoff(client_settings_.value("send_read_receipts", true)),
                            "/receipts on|off"));
        rows.push_back(line("another device as you", "recovery key", "/recovery-key"));
        rows.push_back(text(""));
        rows.push_back(text("This server") | bold | color(Color::Green));
        rows.push_back(line("name", server_name_.empty() ? "?" : server_name_, ""));
        rows.push_back(line("address", server_shown_, "/servers to see all, /server switch <name>"));
        rows.push_back(line("server key", server_fp_.empty() ? "?" : server_fp_.substr(0, 16) + "...", "pinned on first connection"));
        rows.push_back(line("you are", is_owner_ ? "the owner" : permissions_.count("administrator") ? "an administrator" : "a member", ""));
        if (server_status_.contains("version")) {
            uint64_t up = server_status_.value("started_at", uint64_t{0});
            uint64_t now = now_ms_();
            rows.push_back(line("status", "version " + server_status_.value("version", "?") + ", up " +
                                              describe_duration(up && now > up ? (now - up) / 1000 : 0) + ", " +
                                              std::to_string(server_status_.value("members", 0)) + " members (" +
                                              std::to_string(server_status_.value("online", 0)) + " online), " +
                                              std::to_string(server_status_.value("stored_bytes", uint64_t{0}) / 1024) + " KB stored",
                                ""));
        }
        if (!server_settings_.empty()) {
            rows.push_back(text(""));
            rows.push_back(hbox({text("Server settings") | bold | color(Color::Green),
                                 text("    /set <name> <value>    /reboot    /invite") | dim}));
            for (const auto& st : server_settings_) {
                rows.push_back(hbox({text("  " + st.value("key", "") + " = ") | bold, text(st.value("value", "")) | color(Color::Cyan),
                                     text(st.value("owner_only", false) ? "  (owner only)" : "") | color(Color::Magenta),
                                     text(st.value("needs_restart", false) ? "  (needs /reboot)" : "") | dim,
                                     text("   " + st.value("description", "")) | dim | xflex_shrink}));
            }
        }
        return vbox(std::move(rows)) | yframe | flex;
    }

    Element messages_view() {
        if (show_help_) return help_view();
        if (members_selected()) return members_view();
        if (settings_selected()) return settings_view();
        Room* room = current();
        if (!room) {
            return vbox({text(""), text("No conversations yet.") | center,
                         text("Type  /chat <username>  to start one.") | center}) |
                   flex;
        }
        Elements lines;
        auto draw = [&](const Message& m, const std::string& indent) {
            if (!m.reply_to.empty()) {
                std::string quoted = "(earlier message)";
                for (const auto& other : room->messages)
                    if (other.event_id == m.reply_to) quoted = other.sender + ": " + snippet(other.body);
                lines.push_back(text(indent + "        > " + quoted) | dim);
            }
            std::string mark;
            if (m.mine) mark = m.status == "pending" ? " ..." : m.status == "failed" ? " (failed)" : "";
            if (m.edited) mark += " (edited)";
            if (m.disappearing) mark += " (disappears)";
            if (m.from_history) mark += " (earlier, shared)";
            bool mentioned = !m.mine && mentions_user(m.body, username_);
            Element name = text(m.sender + ": ") | bold | color(m.mine ? Color::Cyan : Color::Green);
            if (mentioned) name = hbox({text("@ ") | bold | color(Color::Yellow), name});
            Element body = linked_paragraph(m.body + mark);
            if (m.status == "undecryptable" || m.status == "failed") body = body | color(Color::Red);
            else if (mentioned) body = body | color(Color::Yellow);
            lines.push_back(hbox({text(indent) | dim, text(std::to_string(m.num) + " ") | color(Color::GrayDark),
                                  text(clock_time(m.ts) + " ") | dim, name, body | flex}));
            if (auto it = room->reactions.find(m.event_id); it != room->reactions.end()) {
                std::string r = indent + "        ";
                bool any = false;
                for (const auto& [key, ids] : it->second) {
                    if (ids.empty()) continue;
                    any = true;
                    r += "[" + key + (ids.size() > 1 ? " x" + std::to_string(ids.size()) : "") + "] ";
                }
                if (any) lines.push_back(text(r) | color(Color::Yellow));
            }
            std::string readers;
            for (const auto& [who, upto] : room->read_upto)
                if (upto == m.event_id) readers += (readers.empty() ? "" : ", ") + who;
            if (!readers.empty()) lines.push_back(text(indent + "        read by " + readers) | dim);
        };
        for (const auto& m : room->messages) {
            // Thread messages are drawn under the message that started the
            // thread, unless that message is not on screen.
            if (!m.thread_root.empty()) {
                bool root_known = false;
                for (const auto& other : room->messages)
                    if (other.event_id == m.thread_root) root_known = true;
                if (root_known) continue;
            }
            draw(m, "");
            // Replies hang under their message; a reply's own replies under it, and so on.
            std::function<void(const std::string&, const std::string&)> draw_replies =
                [&](const std::string& parent, const std::string& indent) {
                    if (indent.size() > 40) return;  // deep enough for any screen
                    for (const auto& child : room->messages)
                        if (child.thread_root == parent) {
                            draw(child, indent);
                            draw_replies(child.event_id, indent + "   | ");
                        }
                };
            draw_replies(m.event_id, "   | ");
        }
        if (lines.empty()) lines.push_back(text("No messages yet. Say hello.") | dim | center);
        std::string typers;
        uint64_t now = now_ms_();
        for (const auto& [who, until] : room->typing)
            if (until > now) typers += (typers.empty() ? "" : ", ") + who;
        if (!typers.empty())
            lines.push_back(text(typers + (typers.find(", ") == std::string::npos ? " is typing..." : " are typing...")) |
                            dim);
        return vbox(std::move(lines)) | focusPositionRelative(0, 1) | yframe | flex;
    }

    Element main_view() {
        Color conn_color = connection_ == "live" ? Color::Green
                           : connection_ == "disconnected" || connection_.empty() ? Color::Red
                                                                                   : Color::Yellow;
        Element header = hbox({
            text(" corded ") | bold | inverted,
            text(server_name_.empty() ? "" : " " + server_name_ + " ") | bold,
            text(" " + username_ + " "),
            text(is_owner_ ? "owner " : permissions_.count("administrator") ? "admin " : "") | color(Color::Magenta),
            text(connection_.empty() ? "offline" : connection_) | color(conn_color),
            text(server_shown_.empty() ? "" : "  " + server_shown_) | dim,
            text(server_fp_.empty() || connection_ != "live" ? "" : "  TLS, key " + server_fp_.substr(0, 8)) | dim,
            filler(),
            text(unread_elsewhere() ? std::to_string(unread_elsewhere()) + " unread on other servers  " : "") |
                color(Color::Yellow),
            text("end-to-end encrypted ") | dim,
        });
        Room* room = current();
        Element left = vbox({text(server_name_.empty() ? "Chats" : server_name_) | bold, separator(),
                             room_menu_->Render() | yframe | flex}) |
                       size(WIDTH, EQUAL, 24);
        Element right = vbox({
            hbox({text(show_help_ ? "Commands   (/help again to close)"
                                  : settings_selected() ? std::string("Settings")
                                  : members_selected() ? "Members of " + (server_name_.empty() ? std::string("this server") : server_name_)
                                  : room ? room->title : "") | bold,
                  text(room && room->disappear_after
                           ? "   messages disappear after " + describe_duration(room->disappear_after)
                           : "") | color(Color::Yellow)}),
            separator(),
            messages_view(),
            separator(),
            hbox({text("> "), input_->Render() | flex}),
        }) | flex;
        Elements all = {header, hbox({left, separator(), right}) | flex | border};
        if (show_info_) all.push_back(info_box_);
        if (show_verify_) {
            Elements rows = {text("Safety numbers") | bold,
                             text("Compare with each person over a channel you trust (in person, "
                                  "on a call). If the numbers match, nobody is in between.") | dim};
            for (const auto& entry : verify_list_)
                rows.push_back(hbox({
                    text(entry.value("username", "?") + "  ") | bold,
                    text(entry.value("safety_number", "")),
                    text(entry.value("verified", false) ? "  (checked)" : "") | color(Color::Green),
                }));
            rows.push_back(text("/verified <username> marks someone as checked; /unverified undoes it") | dim);
            all.push_back(vbox(std::move(rows)) | border);
        }
        std::string footer = !notice_.empty() ? notice_ : !pin_notice_.empty() ? pin_notice_ : "/help for commands";
        all.push_back(text(" " + footer) | (notice_.empty() ? dim : color(Color::Yellow)));
        return vbox(std::move(all));
    }

    void build_ui() {
        InputOption line;
        line.multiline = false;
        line.on_enter = [this] { submit_login(); };
        InputOption secret = line;
        secret.password = true;
        name_input_ = Input(&name_, "a-z, 0-9, _ and -", line);
        pass_input_ = Input(&pass_, "", secret);
        pass2_input_ = Input(&pass2_, "", secret);
        Component login_fields = creating_ ? Container::Vertical({name_input_, pass_input_, pass2_input_})
                                           : Container::Vertical({pass_input_});
        Component login = Renderer(login_fields, [this] { return login_view(); });

        InputOption chat;
        chat.multiline = false;
        chat.on_enter = [this] { submit_input(); };
        chat.on_change = [this] {
            Room* room = current();
            // The core sends at most one of these every few seconds.
            if (room && !input_text_.empty() && input_text_[0] != '/') command({{"cmd", "typing"}, {"room_id", room->id}});
        };
        input_ = Input(&input_text_, "type a message, or /help", chat);
        MenuOption menu = MenuOption::Vertical();
        menu.on_change = [this] { on_room_selected(); };
        room_menu_ = Menu(&titles_, &selected_, menu);
        Component main_fields = Container::Horizontal({room_menu_, input_});
        Component main = Renderer(main_fields, [this] { return main_view(); });
        main = CatchEvent(main, [this](Event e) {
            if (e == Event::Tab) {
                if (input_->Focused()) room_menu_->TakeFocus();
                else input_->TakeFocus();
                return true;
            }
            return false;
        });

        root_ = Container::Tab({login, main}, &phase_);
        if (creating_ && !name_.empty()) pass_input_->TakeFocus();
    }

    std::string join_link_, recovery_key_;
    bool rename_tried_ = false;
    std::string vault_dir_, server_, name_, fingerprint_, invite_;
    std::string server_fp_, pin_notice_;
    corded_engine* engine_ = nullptr;
    ScreenInteractive screen_ = ScreenInteractive::Fullscreen();
    std::atomic<bool> running_{true};

    // login
    bool creating_ = false, busy_ = false;
    std::string pass_, pass2_, login_error_;
    corded_request login_request_ = 0, chat_request_ = 0, join_request_ = 0;
    std::map<int64_t, ServerState> servers_;
    int64_t current_server_ = 0;
    std::map<int64_t, std::string> open_by_server_;
    std::vector<size_t> visible_;  // positions in rooms_ of the chats listed on screen
    json member_list_ = json::array();
    int page_ = 0;  // 0 = a chat is open, 1 = members page, 2 = settings page
    json client_settings_ = json::object(), server_settings_ = json::array(), server_status_ = json::object();
    corded_request settings_request_ = 0, status_request_ = 0;
    std::atomic<uint64_t> typing_until_{0};
    int idle_ticks_ = 0;

    // main
    int phase_ = 0;
    std::string username_, connection_, server_shown_, notice_, input_text_;
    std::vector<Room> rooms_;
    std::vector<std::string> titles_;
    int selected_ = 0;
    bool show_help_ = false, show_verify_ = false, show_info_ = false, is_owner_ = false;
    std::string server_name_;
    std::set<std::string> permissions_;
    json roles_ = json::array();
    Element info_box_ = text("");
    json verify_list_ = json::array();

    Component root_, name_input_, pass_input_, pass2_input_, input_, room_menu_;
};

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    // Before any thread exists, so every thread inherits it: a termination
    // signal is handled by one dedicated thread instead of killing the program
    // mid-write.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
    std::string vault, server, name, fingerprint, invite, join, recovery;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--vault") vault = next();
        else if (a == "--server") server = next();
        else if (a == "--name") name = next();
        else if (a == "--fingerprint") fingerprint = next();
        else if (a == "--invite") invite = next();
        else if (a == "--join") join = next();
        else if (a == "--recovery-key") recovery = next();
        else {
            std::printf("usage: corded-tui [--vault DIR] [--server HOST:PORT] [--name USERNAME]\n"
                        "                  [--fingerprint KEY] [--invite CODE] [--join LINK]\n"
                        "                  [--recovery-key KEY]\n\n"
                        "  --recovery-key set this device up as someone who already has an account on\n"
                        "                 another device; get the key there with /recovery-key\n"
                        "  --join         an invite link (corded://...) from a member of the server;\n"
                        "                 replaces --server, --fingerprint and --invite\n"
                        "  --vault        where this identity is stored (default: ~/.corded/default)\n"
                        "  --server       server to connect to, for example localhost:7443\n"
                        "  --name         username to register when creating a new vault\n"
                        "  --fingerprint  the server's key, as printed when cordedd starts; without\n"
                        "                 it the key seen on first connection is trusted\n"
                        "  --invite       invite code, if the server needs one to create an account\n");
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (vault.empty()) {
        const char* home = std::getenv("HOME");
        vault = std::string(home ? home : ".") + "/.corded/default";
    }
    return TuiApp(vault, server, name, fingerprint, invite, join, recovery).run();
}

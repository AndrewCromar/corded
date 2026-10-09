// corded-tui: a terminal client. It talks to the core only through the public
// C header: commands go in as JSON, events come out as JSON.
#include "corded/corded.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
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
};

struct Room {
    std::string id, title;
    std::string kind = "direct";  // channel, direct or group
    std::vector<Message> messages;
    // target event id -> reaction key -> ids of the reaction events
    std::map<std::string, std::map<std::string, std::set<std::string>>> reactions;
    int unread = 0;
    bool loaded = false;
};

std::string clock_time(uint64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[8];
    std::strftime(buf, sizeof buf, "%H:%M", &tm);
    return buf;
}

std::string snippet(const std::string& s, size_t n = 40) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

class TuiApp {
public:
    TuiApp(std::string vault_dir, std::string server, std::string name, std::string fingerprint,
           std::string invite, std::string join_link)
        : join_link_(std::move(join_link)),
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
                if (corded_next_event(engine_, 100, &text, nullptr) != CORDED_OK) continue;
                json ev = json::parse(text, nullptr, false);
                corded_event_free(text);
                if (ev.is_discarded()) continue;
                screen_.Post([this, ev = std::move(ev)] { on_event(ev); });
                screen_.PostEvent(Event::Custom);
            }
        });

        screen_.Loop(root_);
        running_ = false;
        pump.join();
        corded_engine_destroy(engine_);
        return 0;
    }

private:
    // ------------------------------------------------------------ engine I/O
    corded_request command(const json& cmd) {
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
                phase_ = 1;
                input_->TakeFocus();
                if (!join_link_.empty()) command({{"cmd", "connect"}, {"link", join_link_}});
                else if (!server_.empty()) connect_to(server_);
            }
        } else if (kind == "connection_state") {
            connection_ = ev.value("state", "");
            if (ev.contains("detail")) notice_ = ev.value("detail", "");
            if (ev.contains("server")) server_shown_ = ev.value("server", "");
            if (ev.contains("fingerprint")) server_fp_ = ev.value("fingerprint", "");
            if (connection_ == "live") notice_.clear();
        } else if (kind == "room_updated") {
            const json& r = ev.at("room");
            std::string id = r.value("room_id", "");
            {
                Room& room = room_for(id);
                room.title = r.value("title", "?");
                room.kind = r.value("kind", "direct");
            }
            refresh_titles();  // re-sorts, so look the room up again
            Room& room = room_for(id);
            if (!room.loaded) {
                room.loaded = true;
                command({{"cmd", "fetch_timeline"}, {"room_id", room.id}, {"limit", 500}});
            }
        } else if (kind == "server_info") {
            server_name_ = ev.value("name", "");
            is_owner_ = ev.value("is_owner", false);
            permissions_.clear();
            for (const auto& p : ev.value("my_permissions", json::array())) permissions_.insert(p.get<std::string>());
            roles_ = ev.value("roles", json::array());
        } else if (kind == "room_removed") {
            if (ev.value("reason", "") == "removed") notice_ = "you were removed from a chat";
            std::string id = ev.value("room_id", "");
            rooms_.erase(std::remove_if(rooms_.begin(), rooms_.end(), [&](const Room& r) { return r.id == id; }),
                         rooms_.end());
            selected_ = 0;
            refresh_titles();
        } else if (kind == "event_received") {
            const json& d = ev.at("data");
            Room& room = room_for(d.value("room_id", ""));
            bool is_new = add_event(room, d);
            if (is_new && !d.value("mine", false) && current() != &room) {
                ++room.unread;
                refresh_titles();
            }
        } else if (kind == "event_updated") {
            // An edit or a deletion changed an existing message.
            const json& d = ev.at("data");
            Room& room = room_for(d.value("room_id", ""));
            std::string id = d.value("event_id", "");
            for (auto& m : room.messages) {
                if (m.event_id != id) continue;
                m.status = d.value("status", m.status);
                m.edited = d.value("edited", false);
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
            Elements rows = {text("Members of " + (server_name_.empty() ? std::string("this server") : server_name_)) | bold};
            for (const auto& m : ev["data"]["members"]) {
                std::string roles;
                for (const auto& r : m.value("roles", json::array())) roles += " [" + r.get<std::string>() + "]";
                rows.push_back(hbox({text(m.value("username", "?")) | bold,
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
            return room.reactions[rel_target][key].insert(id).second;
        }
        // Edits and deletions are not lines of their own; they arrive again as
        // event_updated for the message they change.
        if (type == "m.edit" || type == "m.redaction") return false;
        for (auto& m : room.messages) {
            if (m.event_id != id) continue;
            m.status = d.value("status", m.status);
            return false;
        }
        Message m;
        m.event_id = id;
        m.type = type;
        m.mine = d.value("mine", false);
        m.sender = d.value("sender_name", m.mine ? username_ : std::string("?"));
        m.status = d.value("status", "");
        m.ts = d.value("origin_ts", uint64_t{0});
        m.edited = d.value("edited", false);
        if (rel_kind == "reply") m.reply_to = rel_target;
        if (rel_kind == "thread") m.thread_root = rel_target;
        if (m.status == "redacted") m.body = "[deleted]";
        else if (type == "m.text") m.body = d["content"].value("body", "");
        else if (type == "m.room.member")
            m.body = d["content"].value("action", "") == "left"
                         ? "left the chat"
                         : d["content"].value("action", "") == "removed"
                               ? "removed " + d["content"].value("username", "someone") + " from the chat"
                               : "added " + d["content"].value("username", "someone") + " to the chat";
        else if (type == "m.room.name") m.body = "named this chat \"" + d["content"].value("name", "") + "\"";
        else if (m.status == "undecryptable") m.body = "[could not decrypt this message]";
        else if (d.contains("fallback_text")) m.body = d.value("fallback_text", "");
        else m.body = "[unsupported message type: " + type + "]";
        room.messages.push_back(std::move(m));
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
    Room* current() {
        if (rooms_.empty()) return nullptr;
        selected_ = std::clamp(selected_, 0, static_cast<int>(rooms_.size()) - 1);
        return &rooms_[static_cast<size_t>(selected_)];
    }
    // Channels first, then everything else, each alphabetical. The open chat
    // stays open even though its position may change.
    void refresh_titles() {
        std::string open = (selected_ >= 0 && selected_ < static_cast<int>(rooms_.size()))
                               ? rooms_[static_cast<size_t>(selected_)].id
                               : "";
        std::stable_sort(rooms_.begin(), rooms_.end(), [](const Room& a, const Room& b) {
            bool ac = a.kind == "channel", bc = b.kind == "channel";
            return ac != bc ? ac : a.title < b.title;
        });
        titles_.clear();
        for (size_t i = 0; i < rooms_.size(); ++i) {
            const Room& r = rooms_[i];
            if (r.id == open) selected_ = static_cast<int>(i);
            titles_.push_back(r.title + (r.unread > 0 ? " (" + std::to_string(r.unread) + ")" : ""));
        }
    }
    void select_room(const std::string& id) {
        for (size_t i = 0; i < rooms_.size(); ++i)
            if (rooms_[i].id == id) selected_ = static_cast<int>(i);
        on_room_selected();
    }
    void on_room_selected() {
        if (Room* r = current(); r && r->unread > 0) {
            r->unread = 0;
            refresh_titles();
        }
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
        corded_status st = creating_
                               ? corded_vault_create(engine_, p, pass_.size(), name_.c_str(), &login_request_)
                               : corded_vault_unlock(engine_, p, pass_.size(), &login_request_);
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
            if (cmd == "/quit" || cmd == "/q") {
                screen_.Exit();
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
                for (const auto& r : rooms_)
                    if (!found && lower(r.title).find(lower(arg)) != std::string::npos) {
                        select_room(r.id);
                        found = true;
                    }
                if (!found) notice_ = "no chat matches \"" + arg + "\"";
            } else if (cmd == "/kick" && !arg.empty()) {
                command({{"cmd", "kick"}, {"username", arg}});
            } else if ((cmd == "/ban" || cmd == "/unban") && !arg.empty()) {
                command({{"cmd", "ban_user"}, {"username", arg}, {"banned", cmd == "/ban"}});
            } else if (cmd == "/invite") {
                // "/invite" for an unlimited link, "/invite 3" for three uses.
                json c = {{"cmd", "create_invite"}};
                if (!arg.empty()) c["max_uses"] = std::atoi(arg.c_str());
                command(c);
            } else if (cmd == "/members") {
                command({{"cmd", "member_list"}});
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
            } else if ((cmd == "/edit" && !arg.empty() && room) || (cmd == "/delete" && room)) {
                const Message* mine = nullptr;
                for (auto it = room->messages.rbegin(); it != room->messages.rend() && !mine; ++it)
                    if (it->mine && it->type == "m.text" && it->status == "ok") mine = &*it;
                if (!mine) notice_ = "you have no sent message here to change";
                else if (cmd == "/edit")
                    command({{"cmd", "edit_event"}, {"room_id", room->id}, {"event_id", mine->event_id}, {"body", arg}});
                else
                    command({{"cmd", "delete_event"}, {"room_id", room->id}, {"event_id", mine->event_id}});
            } else if (cmd == "/remove" && room) {
                // Moderators: delete the last message someone else posted here.
                if (const Message* m = last_from_other(*room))
                    command({{"cmd", "delete_event"}, {"room_id", room->id}, {"event_id", m->event_id}});
                else
                    notice_ = "nothing here to remove";
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
            } else if (cmd == "/reply" && !arg.empty() && room) {
                if (const Message* m = last_from_other(*room))
                    command({{"cmd", "send_text"}, {"room_id", room->id}, {"body", arg}, {"reply_to", m->event_id}});
                else
                    notice_ = "nothing to reply to yet";
            } else if (cmd == "/thread" && !arg.empty() && room) {
                // Continue the thread the last received message is in, or start
                // one on that message.
                if (const Message* m = last_from_other(*room))
                    command({{"cmd", "send_text"}, {"room_id", room->id}, {"body", arg},
                             {"thread", m->thread_root.empty() ? m->event_id : m->thread_root}});
                else
                    notice_ = "nothing to start a thread on yet";
            } else if (cmd == "/react" && !arg.empty() && room) {
                if (const Message* m = last_from_other(*room))
                    command({{"cmd", "send_event"}, {"room_id", room->id}, {"type", "m.reaction"},
                             {"content", {{"key", arg}}},
                             {"relation", {{"kind", "annotation"}, {"target", m->event_id}, {"key", arg}}}});
                else
                    notice_ = "nothing to react to yet";
            } else if (cmd == "/help") {
                show_help_ = !show_help_;
            } else {
                notice_ = "unknown command; try /help";
            }
            return;
        }
        if (!room) {
            notice_ = "start a chat first: /chat <username>";
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
            rows.push_back(text("No vault here yet. Create your identity."));
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

    Element messages_view() {
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
            Element name = text(m.sender + ": ") | bold | color(m.mine ? Color::Cyan : Color::Green);
            Element body = paragraph(m.body + mark);
            if (m.status == "undecryptable" || m.status == "failed") body = body | color(Color::Red);
            lines.push_back(hbox({text(indent) | dim, text(clock_time(m.ts) + " ") | dim, name, body | flex}));
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
            for (const auto& child : room->messages)
                if (child.thread_root == m.event_id) draw(child, "   | ");
        }
        if (lines.empty()) lines.push_back(text("No messages yet. Say hello.") | dim | center);
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
            text("end-to-end encrypted ") | dim,
        });
        Room* room = current();
        Element left = vbox({text(server_name_.empty() ? "Chats" : server_name_) | bold, separator(),
                             rooms_.empty() ? text("(none)") | dim : room_menu_->Render() | yframe | flex}) |
                       size(WIDTH, EQUAL, 24);
        Element right = vbox({
            text(room ? room->title : "") | bold,
            separator(),
            messages_view(),
            separator(),
            hbox({text("> "), input_->Render() | flex}),
        }) | flex;
        Elements all = {header, hbox({left, separator(), right}) | flex | border};
        if (show_help_)
            all.push_back(vbox({
                              text("/chat <username>   start or open a chat"),
                              text("/group a b c : Name  start a group chat (the name is optional)"),
                              text("/name <text>       rename the open chat"),
                              text("/add <username>    add someone to the open group        /leave  leave it"),
                              text("/verify            show safety numbers for the people in this chat"),
                              text("/reply <text>      reply to the last message you received"),
                              text("/thread <text>     reply in a thread under the last message you received"),
                              text("/react <emoji>     react to the last message you received"),
                              text("/edit <text>       change your last message        /delete  remove it"),
                              text("/connect host:port connect to a server"),
                              text("/quit              leave        Tab: switch between chats and typing"),
                              text("/open <name>       open a channel or chat by name      /members  /roles"),
                              text("running the server (needs the permission): /channel new|rename|delete|private|readonly|open"),
                              text("   /role new|delete|give|take      /kick <user>   /ban <user>   /unban <user>"),
                              text("   /invite [uses]   make an invite link for someone to join"),
                              text("   /remove   delete the last message someone else posted in this channel"),
                          }) |
                          border);
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

    std::string join_link_;
    std::string vault_dir_, server_, name_, fingerprint_, invite_;
    std::string server_fp_, pin_notice_;
    corded_engine* engine_ = nullptr;
    ScreenInteractive screen_ = ScreenInteractive::Fullscreen();
    std::atomic<bool> running_{true};

    // login
    bool creating_ = false, busy_ = false;
    std::string pass_, pass2_, login_error_;
    corded_request login_request_ = 0, chat_request_ = 0;

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
    std::string vault, server, name, fingerprint, invite, join;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--vault") vault = next();
        else if (a == "--server") server = next();
        else if (a == "--name") name = next();
        else if (a == "--fingerprint") fingerprint = next();
        else if (a == "--invite") invite = next();
        else if (a == "--join") join = next();
        else {
            std::printf("usage: corded-tui [--vault DIR] [--server HOST:PORT] [--name USERNAME]\n"
                        "                  [--fingerprint KEY] [--invite CODE] [--join LINK]\n\n"
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
    return TuiApp(vault, server, name, fingerprint, invite, join).run();
}

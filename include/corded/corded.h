/*
 * corded.h: the public C interface of libcorded.
 *
 * PROTOTYPE. This interface is not frozen and will change.
 *
 * Model: a frontend creates one engine, sends it commands, and reads a stream
 * of JSON events. Nothing here blocks on the network. A call that starts work
 * returns a request id at once; the outcome arrives later as a
 * "command_result" event carrying that id.
 *
 * Threading: every function may be called from any thread.
 *
 * One vault can belong to several servers at once. Every event that concerns a
 * server carries "server_id", and so does every room. A command about a room
 * goes to that room's server by itself. A command that names no room may carry
 * "server_id"; without it, the first server is meant.
 *
 * Commands (JSON objects passed to corded_command, "cmd" names the command):
 *   {"cmd":"status"}
 *   {"cmd":"list_servers"}
 *   {"cmd":"set_username","username":"newname"}   only before any server has accepted you
 *   {"cmd":"connect","host":"example.org","port":7443,"fingerprint":"<optional server key>",
 *    "invite":"<code, if the server needs one to register>"}
 *   {"cmd":"connect","link":"corded://example.org:7443/?fp=...&invite=..."}
 *   {"cmd":"disconnect"}
 *   {"cmd":"lock"}
 *   {"cmd":"list_rooms"}
 *   {"cmd":"start_chat","username":"bob"}
 *   {"cmd":"create_room","usernames":["bob","carol"],"name":"optional room name"}
 *   {"cmd":"set_room_name","room_id":"...","name":"new name"}
 *   {"cmd":"add_member","room_id":"...","username":"dave"}     (groups only)
 *   {"cmd":"leave_room","room_id":"..."}                        (groups only)
 *  Running the community (the server refuses anyone without the permission):
 *   {"cmd":"server_info"}
 *   {"cmd":"member_list"}
 *   {"cmd":"create_channel","name":"announcements"}
 *   {"cmd":"rename_channel","room_id":"...","name":"news"}
 *   {"cmd":"delete_channel","room_id":"..."}
 *   {"cmd":"set_channel_access","room_id":"...","role":"@everyone",
 *    "allow":[],"deny":["view_channel"]}
 *   {"cmd":"create_role","name":"moderator","permissions":["kick_members","manage_messages"]}
 *   {"cmd":"edit_role","role":"moderator","permissions":["kick_members"]}
 *   {"cmd":"delete_role","role":"moderator"}
 *   {"cmd":"grant_role","username":"dave","role":"moderator","grant":true}
 *   {"cmd":"create_invite","max_uses":5,"expires_in_hours":24}   (both optional)
 *   {"cmd":"revoke_invite","code":"..."}
 *   {"cmd":"get_settings"}                                        (needs manage_server)
 *   {"cmd":"set_setting","key":"registration","value":"invite"}
 *   {"cmd":"restart_server"}          restarts the server program; clients reconnect
 *   {"cmd":"server_status"}
 *   {"cmd":"kick","username":"dave"}
 *   {"cmd":"ban_user","username":"dave","banned":true}
 *   {"cmd":"remove_account","username":"dave"}    deletes the account and frees its name
 *  Permission names: view_channel, send_messages, add_reactions, attach_files,
 *  mention_everyone, manage_messages, manage_channels, manage_roles,
 *  manage_nicknames, kick_members, ban_members, create_invite, manage_server,
 *  administrator.
 *   {"cmd":"safety_numbers","room_id":"..."}
 *   {"cmd":"set_verified","user_id":"...","verified":true}
 *   {"cmd":"send_text","room_id":"...","body":"hi","reply_to":"<event id, optional>",
 *    "thread":"<id of the message that started the thread, optional>",
 *    "expires_in":<seconds until this one message disappears, optional>}
 *   {"cmd":"set_disappearing","room_id":"...","seconds":3600}   (0 turns it off)
 *   {"cmd":"get_recovery_key"}        the secret for adding another device; handle with care
 *   {"cmd":"request_history","room_id":"..."}       ask members for earlier messages
 *   {"cmd":"set_history_sharing","enabled":false}   stop answering such requests
 *   {"cmd":"fetch_thread","room_id":"...","event_id":"<thread's first message>"}
 *   {"cmd":"edit_event","room_id":"...","event_id":"...","body":"new text"}
 *   {"cmd":"delete_event","room_id":"...","event_id":"..."}
 *   {"cmd":"send_event","room_id":"...","type":"m.reaction","content":{...},
 *    "relation":{"kind":"annotation","target":"<event id>","key":"+1"}}
 *   {"cmd":"fetch_timeline","room_id":"...","limit":200}
 *
 * Events (JSON objects, "event" names the event):
 *   vault_state, connection_state, account, server_info, server_pinned, room_updated,
 *   room_removed,
 *   event_received, event_expired, history_received, server_notice,
 *   event_updated, event_send_status, command_result, warning
 */
#ifndef CORDED_H
#define CORDED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(CORDED_BUILDING)
#    define CORDED_API __declspec(dllexport)
#  else
#    define CORDED_API __declspec(dllimport)
#  endif
#else
#  define CORDED_API __attribute__((visibility("default")))
#endif

#define CORDED_ABI_VERSION_MAJOR 0
#define CORDED_ABI_VERSION_MINOR 1

typedef struct corded_engine corded_engine;
typedef int32_t corded_status;
typedef uint64_t corded_request;

#define CORDED_OK 0
#define CORDED_ERR_INVALID_ARGUMENT (-1)
#define CORDED_ERR_NO_EVENT (-2) /* corded_next_event timed out */
#define CORDED_ERR_INTERNAL (-3)

typedef struct corded_config {
    uint32_t struct_size;   /* set to sizeof(corded_config) */
    const char* vault_dir;  /* directory that holds this identity's vault */
    int32_t fast_kdf;       /* tests only: weak passphrase hashing; leave 0 */
} corded_config;

CORDED_API uint32_t corded_abi_version(void); /* (major << 16) | minor */
CORDED_API const char* corded_version_string(void);
CORDED_API const char* corded_status_message(corded_status status);

CORDED_API corded_status corded_engine_create(const corded_config* config, corded_engine** out);
/* After this returns no further events are produced. */
CORDED_API void corded_engine_destroy(corded_engine* engine);

CORDED_API corded_status corded_vault_exists(corded_engine* engine, int32_t* out_exists);
/* The passphrase is copied and the copy is wiped after use. It need not be
 * NUL-terminated. username: a-z, 0-9, '_' and '-', at most 32 characters. */
CORDED_API corded_status corded_vault_create(corded_engine* engine, const uint8_t* passphrase,
                                             size_t passphrase_len, const char* username,
                                             corded_request* out_request);
/* Sets up this device for a person who already exists on another device.
 * recovery_key is what {"cmd":"get_recovery_key"} returned there. The new
 * device has the same identity, its own keys and its own passphrase. */
CORDED_API corded_status corded_vault_restore(corded_engine* engine, const uint8_t* passphrase,
                                              size_t passphrase_len, const char* username,
                                              const char* recovery_key, corded_request* out_request);
CORDED_API corded_status corded_vault_unlock(corded_engine* engine, const uint8_t* passphrase,
                                             size_t passphrase_len, corded_request* out_request);

CORDED_API corded_status corded_command(corded_engine* engine, const char* command_json,
                                        size_t len, corded_request* out_request);

/* Waits up to timeout_ms for the next event. On CORDED_OK, *out_json is a
 * NUL-terminated string owned by the caller until corded_event_free. */
CORDED_API corded_status corded_next_event(corded_engine* engine, int32_t timeout_ms,
                                           const char** out_json, size_t* out_len);
CORDED_API void corded_event_free(const char* json);

#ifdef __cplusplus
}
#endif

#endif /* CORDED_H */

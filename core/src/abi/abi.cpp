// The C ABI shim. No engine logic lives here: each function checks its
// arguments, forwards to the engine, and keeps C++ exceptions from escaping.
#include "corded/corded.h"

#include "engine/engine.hpp"

#include <cstring>

struct corded_engine {
    corded::Engine engine;
    explicit corded_engine(corded::EngineConfig c) : engine(std::move(c)) {}
};

namespace {

template <typename F>
corded_status guarded(F&& f) noexcept {
    try {
        return f();
    } catch (const std::invalid_argument&) {
        return CORDED_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return CORDED_ERR_INTERNAL;
    }
}

}  // namespace

extern "C" {

uint32_t corded_abi_version(void) {
    return (static_cast<uint32_t>(CORDED_ABI_VERSION_MAJOR) << 16) | CORDED_ABI_VERSION_MINOR;
}

const char* corded_version_string(void) { return "corded 0.1.0-prototype"; }

const char* corded_status_message(corded_status status) {
    switch (status) {
        case CORDED_OK: return "ok";
        case CORDED_ERR_INVALID_ARGUMENT: return "invalid argument";
        case CORDED_ERR_NO_EVENT: return "no event";
        case CORDED_ERR_INTERNAL: return "internal error";
        default: return "unknown status";
    }
}

corded_status corded_engine_create(const corded_config* config, corded_engine** out) {
    return guarded([&]() -> corded_status {
        if (!config || !out || config->struct_size < sizeof(corded_config) || !config->vault_dir ||
            !*config->vault_dir)
            return CORDED_ERR_INVALID_ARGUMENT;
        corded::EngineConfig c;
        c.vault_dir = config->vault_dir;
        c.fast_kdf = config->fast_kdf != 0;
        *out = new corded_engine(std::move(c));
        return CORDED_OK;
    });
}

void corded_engine_destroy(corded_engine* engine) {
    try {
        delete engine;
    } catch (...) {
    }
}

corded_status corded_vault_exists(corded_engine* engine, int32_t* out_exists) {
    return guarded([&]() -> corded_status {
        if (!engine || !out_exists) return CORDED_ERR_INVALID_ARGUMENT;
        *out_exists = engine->engine.vault_exists() ? 1 : 0;
        return CORDED_OK;
    });
}

corded_status corded_vault_create(corded_engine* engine, const uint8_t* passphrase,
                                  size_t passphrase_len, const char* username,
                                  corded_request* out_request) {
    return guarded([&]() -> corded_status {
        if (!engine || !passphrase || passphrase_len == 0 || !username)
            return CORDED_ERR_INVALID_ARGUMENT;
        auto req = engine->engine.vault_create(corded::Bytes(passphrase, passphrase + passphrase_len),
                                               username);
        if (out_request) *out_request = req;
        return CORDED_OK;
    });
}

corded_status corded_vault_unlock(corded_engine* engine, const uint8_t* passphrase,
                                  size_t passphrase_len, corded_request* out_request) {
    return guarded([&]() -> corded_status {
        if (!engine || !passphrase || passphrase_len == 0) return CORDED_ERR_INVALID_ARGUMENT;
        auto req = engine->engine.vault_unlock(corded::Bytes(passphrase, passphrase + passphrase_len));
        if (out_request) *out_request = req;
        return CORDED_OK;
    });
}

corded_status corded_command(corded_engine* engine, const char* command_json, size_t len,
                             corded_request* out_request) {
    return guarded([&]() -> corded_status {
        if (!engine || !command_json || len == 0 || len > 1024 * 1024)
            return CORDED_ERR_INVALID_ARGUMENT;
        auto req = engine->engine.command(std::string(command_json, len));
        if (out_request) *out_request = req;
        return CORDED_OK;
    });
}

corded_status corded_next_event(corded_engine* engine, int32_t timeout_ms, const char** out_json,
                                size_t* out_len) {
    return guarded([&]() -> corded_status {
        if (!engine || !out_json) return CORDED_ERR_INVALID_ARGUMENT;
        auto ev = engine->engine.next_event(timeout_ms);
        if (!ev) return CORDED_ERR_NO_EVENT;
        char* copy = new char[ev->size() + 1];
        std::memcpy(copy, ev->c_str(), ev->size() + 1);
        *out_json = copy;
        if (out_len) *out_len = ev->size();
        return CORDED_OK;
    });
}

void corded_event_free(const char* json) { delete[] json; }

}  // extern "C"

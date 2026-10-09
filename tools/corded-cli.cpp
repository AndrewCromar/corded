// corded-cli: a headless client for development and scripting. It prints every
// engine event as one JSON line on stdout and reads commands, one JSON object
// per line, from stdin. It uses only the public C header.
//
//   corded-cli --vault DIR --pass PASSPHRASE [--name USERNAME] [--fast-kdf]
#include "corded/corded.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string vault, pass, name;
    int fast = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--vault") vault = next();
        else if (a == "--pass") pass = next();
        else if (a == "--name") name = next();
        else if (a == "--fast-kdf") fast = 1;
    }
    if (vault.empty() || pass.empty()) {
        std::fprintf(stderr,
                     "usage: corded-cli --vault DIR --pass PASSPHRASE [--name USERNAME] [--fast-kdf]\n");
        return 2;
    }

    corded_config cfg{};
    cfg.struct_size = sizeof cfg;
    cfg.vault_dir = vault.c_str();
    cfg.fast_kdf = fast;
    corded_engine* engine = nullptr;
    if (corded_engine_create(&cfg, &engine) != CORDED_OK) {
        std::fprintf(stderr, "cannot create engine\n");
        return 1;
    }

    std::atomic<bool> running{true};
    std::thread printer([&] {
        while (running) {
            const char* json = nullptr;
            if (corded_next_event(engine, 200, &json, nullptr) == CORDED_OK) {
                std::printf("%s\n", json);
                std::fflush(stdout);
                corded_event_free(json);
            }
        }
    });

    int32_t exists = 0;
    corded_vault_exists(engine, &exists);
    auto* p = reinterpret_cast<const uint8_t*>(pass.data());
    corded_status st = exists ? corded_vault_unlock(engine, p, pass.size(), nullptr)
                              : corded_vault_create(engine, p, pass.size(), name.c_str(), nullptr);
    if (st != CORDED_OK) std::fprintf(stderr, "vault call failed: %s\n", corded_status_message(st));

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "quit") break;
        st = corded_command(engine, line.data(), line.size(), nullptr);
        if (st != CORDED_OK) std::fprintf(stderr, "command rejected: %s\n", corded_status_message(st));
    }

    // Give in-flight sends a moment, then stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    running = false;
    printer.join();
    corded_engine_destroy(engine);
    return 0;
}

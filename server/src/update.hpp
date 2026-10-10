// Updating the server from the owner's client.
//
// The newest release is found on GitHub and downloaded with the machine's own
// `curl`; its signature is checked here against the public key built into
// this program; only then is it unpacked with `tar` and put in place of the
// programs that are running. Nothing is installed that the release key did
// not sign, whatever the network or the download site say.
#pragma once

#include "corded/common/release.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace corded::update {

inline constexpr const char* kRepository = "AndrewCromar/corded";

// What kind of machine this program was built for, as releases name it.
inline const char* platform() {
#if defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#else
    return "";
#endif
}

struct Outcome {
    bool installed = false;   // new programs are in place; a restart will run them
    std::string message;      // what to tell the person who asked
};

// Runs a program and gives back what it printed, or nothing if it failed.
inline std::optional<std::string> run(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return std::nullopt;
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) out.append(buf, n);
    return pclose(pipe) == 0 ? std::optional<std::string>(out) : std::nullopt;
}

inline std::optional<Bytes> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Single quotes around a word for the shell. Paths and addresses here come
// from this program and from GitHub's answer; they are quoted all the same.
inline std::string shell_word(const std::string& word) {
    std::string out = "'";
    for (char c : word) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// Blocks while it downloads; call it off the network thread.
inline Outcome install_newest(const std::string& running_version, const std::filesystem::path& install_dir,
                              const std::filesystem::path& work_dir, const std::string& public_key_pem) {
    namespace fs = std::filesystem;
    using nlohmann::json;
    std::string kind = platform();
    if (kind.empty()) return {false, "this kind of machine cannot update itself yet; run the installer again instead"};
    auto key = release::public_key_from_pem(public_key_pem);
    if (!key) return {false, "this server was built without a release key, so it cannot check an update"};

    auto listing = run("curl -fsSL --max-time 30 -H 'Accept: application/vnd.github+json' "
                       "https://api.github.com/repos/" + std::string(kRepository) + "/releases?per_page=30");
    if (!listing) return {false, "could not reach GitHub to look for a release (is curl installed?)"};
    json releases = json::parse(*listing, nullptr, false);
    if (!releases.is_array()) return {false, "GitHub's answer could not be read"};

    // The newest release that has a build for this machine.
    for (const auto& r : releases) {
        std::string tag = r.value("tag_name", "");
        if (tag.size() < 2 || tag[0] != 'v' || r.value("draft", false)) continue;
        std::string archive_name = "corded-" + tag + "-" + kind + ".tar.gz", archive_url, signature_url;
        for (const auto& a : r.value("assets", json::array())) {
            if (a.value("name", "") == archive_name) archive_url = a.value("browser_download_url", "");
            if (a.value("name", "") == archive_name + ".sig") signature_url = a.value("browser_download_url", "");
        }
        if (archive_url.empty()) continue;
        if (release::release_of(running_version) == tag)
            return {false, "already running the newest release (" + tag + ")"};
        if (signature_url.empty())
            return {false, "the newest release (" + tag + ") is not signed, so it was not installed"};

        std::error_code ec;
        fs::remove_all(work_dir, ec);
        fs::create_directories(work_dir, ec);
        fs::path archive = work_dir / archive_name, signature = work_dir / (archive_name + ".sig");
        if (!run("curl -fsSL --max-time 600 -o " + shell_word(archive.string()) + " " + shell_word(archive_url)) ||
            !run("curl -fsSL --max-time 60 -o " + shell_word(signature.string()) + " " + shell_word(signature_url)))
            return {false, "the download of " + tag + " failed"};
        auto bytes = read_file(archive), sig = read_file(signature);
        if (!bytes || !sig || !release::signed_by(*bytes, *sig, *key)) {
            fs::remove_all(work_dir, ec);
            return {false, "the download of " + tag + " did not carry the release key's signature; nothing was installed"};
        }
        if (!run("tar -xzf " + shell_word(archive.string()) + " -C " + shell_word(work_dir.string())))
            return {false, "the release could not be unpacked (is tar installed?)"};
        fs::path unpacked = work_dir / ("corded-" + tag + "-" + kind);
        if (!fs::exists(unpacked / "cordedd")) return {false, "the release does not hold a server program"};

        // Each file is written beside its place and moved over it, so a
        // program is never half-written; the running one keeps its old file.
        int replaced = 0;
        for (const auto& entry : fs::directory_iterator(unpacked, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            fs::path target = install_dir / entry.path().filename(), fresh = target;
            fresh += ".new";
            fs::copy_file(entry.path(), fresh, fs::copy_options::overwrite_existing, ec);
            if (ec) return {false, "could not write to " + install_dir.string() + ": " + ec.message()};
            fs::permissions(fresh, fs::status(entry.path()).permissions(), ec);
            fs::rename(fresh, target, ec);
            if (ec) return {false, "could not replace " + target.string() + ": " + ec.message()};
            ++replaced;
        }
        fs::remove_all(work_dir, ec);
        return {true, "installed " + tag + " (" + std::to_string(replaced) + " files, signature checked)"};
    }
    return {false, "no release has a build for this kind of machine (" + kind + ")"};
}

}  // namespace corded::update

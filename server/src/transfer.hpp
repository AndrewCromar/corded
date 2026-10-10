// Moving a server: everything it keeps, in one file.
//
// `cordedd --export FILE` writes the accounts, rooms, stored messages and
// files, the settings and the server's own identity into FILE; `cordedd
// --import FILE --data DIR` unpacks it into a fresh data folder. A server
// started on that folder is the same server to every client: same identity,
// same accounts, same history. The export can be made while the server runs.
//
// The file holds the server's private key. With a passphrase it is sealed
// (Argon2id, then XChaCha20-Poly1305 in pieces); without one it is plain and
// only protected against damage, so it must be kept as safe as the server's
// own disk.
//
// Layout: "CORDEDX1", one byte (1: sealed), then for a sealed file a 16-byte
// salt and the 24-byte stream header; after that pieces, each a little-endian
// 32-bit length and that many bytes. A sealed file's last piece carries the
// stream's final tag. A plain file ends with an empty piece and the BLAKE2b
// hash of everything before it. Inside the pieces: for each kept file a
// 16-bit name length, the name, a 64-bit size and the bytes; a zero name
// length ends the list.
#pragma once

#include "corded/common/db.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace corded::transfer {

namespace fs = std::filesystem;

inline constexpr char kMagic[8] = {'C', 'O', 'R', 'D', 'E', 'D', 'X', '1'};
inline constexpr size_t kPiece = 64 * 1024;

class Writer {
public:
    Writer(const std::string& path, const std::string& passphrase) : out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("cannot write " + path);
#ifndef _WIN32
        chmod(path.c_str(), 0600);
#endif
        sealed_ = !passphrase.empty();
        out_.write(kMagic, sizeof kMagic);
        out_.put(sealed_ ? 1 : 0);
        if (sealed_) {
            unsigned char salt[crypto_pwhash_SALTBYTES], key[crypto_secretstream_xchacha20poly1305_KEYBYTES];
            unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
            randombytes_buf(salt, sizeof salt);
            if (crypto_pwhash(key, sizeof key, passphrase.data(), passphrase.size(), salt,
                              crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE,
                              crypto_pwhash_ALG_ARGON2ID13) != 0)
                throw std::runtime_error("not enough memory to derive the key");
            crypto_secretstream_xchacha20poly1305_init_push(&stream_, header, key);
            sodium_memzero(key, sizeof key);
            out_.write(reinterpret_cast<const char*>(salt), sizeof salt);
            out_.write(reinterpret_cast<const char*>(header), sizeof header);
        } else {
            crypto_generichash_init(&hash_, nullptr, 0, crypto_generichash_BYTES);
        }
        piece_.reserve(kPiece);
    }

    void entry(const std::string& name, const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + file.string());
        uint64_t size = fs::file_size(file);
        head(name, size);
        std::vector<char> buf(kPiece);
        uint64_t left = size;
        while (left > 0) {
            in.read(buf.data(), static_cast<std::streamsize>(std::min<uint64_t>(left, buf.size())));
            auto got = static_cast<size_t>(in.gcount());
            if (got == 0) throw std::runtime_error(file.string() + " changed while it was being read");
            put(buf.data(), got);
            left -= got;
        }
    }

    void entry(const std::string& name, const std::string& bytes) {
        head(name, bytes.size());
        put(bytes.data(), bytes.size());
    }

    void finish() {
        const char end[2] = {0, 0};
        put(end, 2);
        flush(true);
        if (!sealed_) {
            write_len(0);
            unsigned char sum[crypto_generichash_BYTES];
            crypto_generichash_final(&hash_, sum, sizeof sum);
            out_.write(reinterpret_cast<const char*>(sum), sizeof sum);
        }
        out_.flush();
        if (!out_) throw std::runtime_error("writing the export failed (is the disk full?)");
    }

private:
    void head(const std::string& name, uint64_t size) {
        char h[2] = {static_cast<char>(name.size() & 0xff), static_cast<char>(name.size() >> 8)};
        put(h, 2);
        put(name.data(), name.size());
        char s[8];
        for (int i = 0; i < 8; ++i) s[i] = static_cast<char>((size >> (8 * i)) & 0xff);
        put(s, 8);
    }
    void put(const char* p, size_t n) {
        while (n > 0) {
            size_t take = std::min(n, kPiece - piece_.size());
            piece_.insert(piece_.end(), p, p + take);
            p += take;
            n -= take;
            if (piece_.size() == kPiece) flush(false);
        }
    }
    void write_len(uint32_t n) {
        char l[4] = {static_cast<char>(n & 0xff), static_cast<char>((n >> 8) & 0xff),
                     static_cast<char>((n >> 16) & 0xff), static_cast<char>((n >> 24) & 0xff)};
        out_.write(l, 4);
    }
    void flush(bool last) {
        if (sealed_) {
            std::vector<unsigned char> c(piece_.size() + crypto_secretstream_xchacha20poly1305_ABYTES);
            unsigned long long clen = 0;
            crypto_secretstream_xchacha20poly1305_push(
                &stream_, c.data(), &clen, reinterpret_cast<const unsigned char*>(piece_.data()), piece_.size(),
                nullptr, 0, last ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0);
            write_len(static_cast<uint32_t>(clen));
            out_.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(clen));
        } else if (!piece_.empty()) {
            crypto_generichash_update(&hash_, reinterpret_cast<const unsigned char*>(piece_.data()), piece_.size());
            write_len(static_cast<uint32_t>(piece_.size()));
            out_.write(piece_.data(), static_cast<std::streamsize>(piece_.size()));
        }
        piece_.clear();
    }

    std::ofstream out_;
    bool sealed_ = false;
    crypto_secretstream_xchacha20poly1305_state stream_{};
    crypto_generichash_state hash_{};
    std::vector<char> piece_;
};

// Whether an export is sealed with a passphrase.
inline bool is_sealed(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char head[9] = {};
    in.read(head, 9);
    if (in.gcount() != 9 || std::string(head, 8) != std::string(kMagic, 8))
        throw std::runtime_error(path + " is not a corded server export");
    return head[8] == 1;
}

class Reader {
public:
    Reader(const std::string& path, const std::string& passphrase) : in_(path, std::ios::binary) {
        sealed_ = is_sealed(path);
        in_.seekg(9);
        if (sealed_) {
            unsigned char salt[crypto_pwhash_SALTBYTES], key[crypto_secretstream_xchacha20poly1305_KEYBYTES];
            unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
            in_.read(reinterpret_cast<char*>(salt), sizeof salt);
            in_.read(reinterpret_cast<char*>(header), sizeof header);
            if (!in_) throw std::runtime_error("the export is cut short");
            if (crypto_pwhash(key, sizeof key, passphrase.data(), passphrase.size(), salt,
                              crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE,
                              crypto_pwhash_ALG_ARGON2ID13) != 0)
                throw std::runtime_error("not enough memory to derive the key");
            int bad = crypto_secretstream_xchacha20poly1305_init_pull(&stream_, header, key);
            sodium_memzero(key, sizeof key);
            if (bad != 0) throw std::runtime_error("the export is damaged");
        } else {
            crypto_generichash_init(&hash_, nullptr, 0, crypto_generichash_BYTES);
        }
    }

    // The next kept file's name and size; false at the end of the list, once
    // the whole file has been checked.
    bool next(std::string& name, uint64_t& size) {
        unsigned char h[2];
        get(h, 2);
        size_t len = h[0] | (h[1] << 8);
        if (len == 0) {
            finish();
            return false;
        }
        name.resize(len);
        get(reinterpret_cast<unsigned char*>(name.data()), len);
        unsigned char s[8];
        get(s, 8);
        size = 0;
        for (int i = 0; i < 8; ++i) size |= static_cast<uint64_t>(s[i]) << (8 * i);
        return true;
    }

    void copy_to(const fs::path& file, uint64_t size) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + file.string());
        std::vector<unsigned char> buf(kPiece);
        while (size > 0) {
            size_t take = static_cast<size_t>(std::min<uint64_t>(size, buf.size()));
            get(buf.data(), take);
            out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(take));
            size -= take;
        }
        out.flush();
        if (!out) throw std::runtime_error("writing " + file.string() + " failed (is the disk full?)");
    }

    std::string read_all(uint64_t size) {
        if (size > 1024 * 1024) throw std::runtime_error("the export is damaged");
        std::string out(static_cast<size_t>(size), '\0');
        get(reinterpret_cast<unsigned char*>(out.data()), out.size());
        return out;
    }

private:
    void get(unsigned char* p, size_t n) {
        while (n > 0) {
            if (at_ == piece_.size()) fill();
            size_t take = std::min(n, piece_.size() - at_);
            std::copy(piece_.begin() + static_cast<long>(at_), piece_.begin() + static_cast<long>(at_ + take), p);
            at_ += take;
            p += take;
            n -= take;
        }
    }
    uint32_t read_len() {
        unsigned char l[4];
        in_.read(reinterpret_cast<char*>(l), 4);
        if (!in_) throw std::runtime_error("the export is cut short");
        return l[0] | (l[1] << 8) | (l[2] << 16) | (static_cast<uint32_t>(l[3]) << 24);
    }
    void fill() {
        if (final_) throw std::runtime_error("the export is damaged");
        uint32_t len = read_len();
        if (len > kPiece + crypto_secretstream_xchacha20poly1305_ABYTES || (!sealed_ && len == 0))
            throw std::runtime_error("the export is damaged");
        std::vector<unsigned char> raw(len);
        in_.read(reinterpret_cast<char*>(raw.data()), len);
        if (!in_) throw std::runtime_error("the export is cut short");
        at_ = 0;
        if (sealed_) {
            if (len < crypto_secretstream_xchacha20poly1305_ABYTES) throw std::runtime_error("the export is damaged");
            piece_.resize(len - crypto_secretstream_xchacha20poly1305_ABYTES);
            unsigned long long mlen = 0;
            unsigned char tag = 0;
            if (crypto_secretstream_xchacha20poly1305_pull(&stream_, piece_.data(), &mlen, &tag, raw.data(), len,
                                                           nullptr, 0) != 0)
                throw std::runtime_error("wrong passphrase, or the export is damaged");
            piece_.resize(static_cast<size_t>(mlen));
            final_ = tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL;
        } else {
            crypto_generichash_update(&hash_, raw.data(), raw.size());
            piece_ = std::move(raw);
        }
    }
    void finish() {
        if (at_ != piece_.size()) throw std::runtime_error("the export is damaged");
        if (sealed_) {
            if (!final_) throw std::runtime_error("the export is cut short");
            return;
        }
        if (read_len() != 0) throw std::runtime_error("the export is damaged");
        unsigned char want[crypto_generichash_BYTES], got[crypto_generichash_BYTES];
        in_.read(reinterpret_cast<char*>(want), sizeof want);
        if (!in_) throw std::runtime_error("the export is cut short");
        crypto_generichash_final(&hash_, got, sizeof got);
        if (sodium_memcmp(want, got, sizeof got) != 0) throw std::runtime_error("the export is damaged");
    }

    std::ifstream in_;
    bool sealed_ = false, final_ = false;
    crypto_secretstream_xchacha20poly1305_state stream_{};
    crypto_generichash_state hash_{};
    std::vector<unsigned char> piece_;
    size_t at_ = 0;
};

// A stored file's name as the server makes them; nothing that could step
// outside the data folder.
inline bool safe_blob_name(const std::string& n) {
    if (n.empty() || n.size() > 128 || n[0] == '.') return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

struct Summary {
    uint64_t files = 0, bytes = 0;
};

// Writes everything in `data_dir` into `file`. The database is copied as one
// consistent picture, so the server may keep running meanwhile.
inline Summary export_server(const std::string& data_dir, const std::string& file, const std::string& passphrase) {
    const fs::path data(data_dir);
    if (!fs::exists(data / "cordedd.db")) throw std::runtime_error("no server found in " + data_dir);
    const fs::path snapshot = fs::path(file + ".db-copy");
    std::error_code ec;
    fs::remove(snapshot, ec);
    {
        db::Database live((data / "cordedd.db").string());
        std::string target = snapshot.string(), quoted;
        for (char c : target) quoted += c == '\'' ? std::string("''") : std::string(1, c);
        live.exec(("VACUUM INTO '" + quoted + "'").c_str());
    }
    Summary done;
    try {
        Writer out(file, passphrase);
        nlohmann::json manifest = {
            {"format", 1},
#ifdef CORDED_BUILD_VERSION
            {"made_by", CORDED_BUILD_VERSION},
#endif
            {"exported_at", std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()},
        };
        out.entry("manifest.json", manifest.dump());
        out.entry("cordedd.db", snapshot);
        for (const char* name : {"tls-cert.pem", "tls-key.pem"})
            if (fs::exists(data / name)) out.entry(name, data / name);
        if (fs::is_directory(data / "blobs"))
            for (const auto& e : fs::directory_iterator(data / "blobs")) {
                std::string name = e.path().filename().string();
                if (!e.is_regular_file() || !safe_blob_name(name)) continue;
                out.entry("blobs/" + name, e.path());
                ++done.files;
            }
        out.finish();
    } catch (...) {
        fs::remove(snapshot, ec);
        fs::remove(file, ec);
        throw;
    }
    fs::remove(snapshot, ec);
    done.bytes = fs::file_size(file);
    return done;
}

// Unpacks an export into `data_dir`, which must not hold a server yet.
// Nothing is put in place until the whole file has been read and checked.
inline Summary import_server(const std::string& file, const std::string& data_dir, const std::string& passphrase) {
    const fs::path data(data_dir);
    if (fs::exists(data / "cordedd.db"))
        throw std::runtime_error(data_dir + " already holds a server; import into an empty folder");
    const fs::path staging = data / "import.partial";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "blobs");
    Summary done;
    try {
        Reader in(file, passphrase);
        std::string name;
        uint64_t size = 0;
        bool has_db = false;
        while (in.next(name, size)) {
            if (name == "manifest.json") {
                auto manifest = nlohmann::json::parse(in.read_all(size), nullptr, false);
                if (!manifest.is_object() || manifest.value("format", 0) != 1)
                    throw std::runtime_error("this export was made by a newer cordedd; update this one first");
            } else if (name == "cordedd.db" || name == "tls-cert.pem" || name == "tls-key.pem") {
                in.copy_to(staging / name, size);
                has_db = has_db || name == "cordedd.db";
            } else if (name.rfind("blobs/", 0) == 0 && safe_blob_name(name.substr(6))) {
                in.copy_to(staging / "blobs" / name.substr(6), size);
                ++done.files;
            } else {
                throw std::runtime_error("the export holds something unexpected: " + name);
            }
            done.bytes += size;
        }
        if (!has_db) throw std::runtime_error("the export holds no server");
    } catch (...) {
        fs::remove_all(staging, ec);
        throw;
    }
    fs::create_directories(data / "blobs");
    for (const auto& e : fs::directory_iterator(staging / "blobs")) fs::rename(e.path(), data / "blobs" / e.path().filename());
    for (const char* name : {"tls-cert.pem", "tls-key.pem", "cordedd.db"})
        if (fs::exists(staging / name)) fs::rename(staging / name, data / name);
#ifndef _WIN32
    chmod((data / "tls-key.pem").string().c_str(), 0600);
#endif
    fs::remove_all(staging, ec);
    return done;
}

// Asks for a passphrase on the terminal without showing it.
inline std::string ask_passphrase(const char* prompt) {
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string line;
#ifndef _WIN32
    termios before{};
    bool hidden = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &before) == 0;
    if (hidden) {
        termios quiet = before;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    std::getline(std::cin, line);
    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSANOW, &before);
        std::fprintf(stderr, "\n");
    }
#else
    std::getline(std::cin, line);
#endif
    return line;
}

}  // namespace corded::transfer

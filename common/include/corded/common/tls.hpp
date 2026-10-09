// TLS helpers shared by the core and the server.
//
// The transport is TLS 1.3. Servers use a self-signed certificate that clients
// pin by fingerprint, so no certificate authority is involved. The device then
// signs a challenge that includes a value exported from the TLS session, which
// ties the sign-in to this exact connection.
#pragma once

#include "corded/common/bytes.hpp"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace corded::tls {

using Stream = asio::ssl::stream<asio::ip::tcp::socket>;

inline constexpr std::string_view kExporterLabel = "EXPORTER-corded-auth-v1";

inline void require_tls13(asio::ssl::context& ctx) {
    SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_3_VERSION);
}

// A secret both ends of this TLS session can compute and nobody else can.
inline Bytes exporter(Stream& stream) {
    Bytes out(32);
    if (SSL_export_keying_material(stream.native_handle(), out.data(), out.size(),
                                   kExporterLabel.data(), kExporterLabel.size(), nullptr, 0, 0) != 1)
        throw std::runtime_error("TLS exporter failed");
    return out;
}

// SHA-256 of the DER-encoded certificate.
inline Bytes fingerprint(X509* cert) {
    unsigned char* der = nullptr;
    int len = i2d_X509(cert, &der);
    if (len <= 0) throw std::runtime_error("cannot encode certificate");
    Bytes out(crypto_hash_sha256_BYTES);
    crypto_hash_sha256(out.data(), der, static_cast<unsigned long long>(len));
    OPENSSL_free(der);
    return out;
}

inline Bytes peer_fingerprint(Stream& stream) {
    X509* cert = SSL_get1_peer_certificate(stream.native_handle());
    if (!cert) throw std::runtime_error("the server sent no certificate");
    std::unique_ptr<X509, decltype(&X509_free)> guard(cert, X509_free);
    return fingerprint(cert);
}

inline Bytes file_fingerprint(const std::string& cert_path) {
    FILE* f = std::fopen(cert_path.c_str(), "r");
    if (!f) throw std::runtime_error("cannot read " + cert_path);
    X509* cert = PEM_read_X509(f, nullptr, nullptr, nullptr);
    std::fclose(f);
    if (!cert) throw std::runtime_error("cannot parse " + cert_path);
    std::unique_ptr<X509, decltype(&X509_free)> guard(cert, X509_free);
    return fingerprint(cert);
}

// Writes a new P-256 key and a ten-year self-signed certificate.
inline void generate_self_signed(const std::string& cert_path, const std::string& key_path,
                                 const std::string& name) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen("P-256"), EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!key || !cert) throw std::runtime_error("cannot generate a TLS key");

    X509_set_version(cert.get(), 2);
    Bytes serial = random_bytes(8);
    serial[0] &= 0x7F;
    std::unique_ptr<BIGNUM, decltype(&BN_free)> bn(BN_bin2bn(serial.data(), 8, nullptr), BN_free);
    BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(cert.get()));
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 3650);
    X509_set_pubkey(cert.get(), key.get());
    X509_NAME* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0)
        throw std::runtime_error("cannot sign the TLS certificate");

    FILE* kf = std::fopen(key_path.c_str(), "w");
    if (!kf) throw std::runtime_error("cannot write " + key_path);
    std::filesystem::permissions(key_path,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    int ok = PEM_write_PrivateKey(kf, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(kf);
    FILE* cf = std::fopen(cert_path.c_str(), "w");
    if (!cf) throw std::runtime_error("cannot write " + cert_path);
    ok = ok && PEM_write_X509(cf, cert.get());
    std::fclose(cf);
    if (!ok) throw std::runtime_error("cannot write the TLS key and certificate");
}

}  // namespace corded::tls

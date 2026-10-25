#include "kp/kalshi/signer.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cctype>
#include <cstdio>
#include <mutex>
#include <stdexcept>

namespace kp::kalshi {

namespace {
constexpr int SALT_LEN = 32;          // hashes.SHA256.digest_size

std::string ssl_err() {
    char b[256];
    const unsigned long e = ERR_get_error();
    if (!e) return "unknown OpenSSL error";
    ERR_error_string_n(e, b, sizeof b);
    return b;
}
}  // namespace

std::string base64(const std::string& bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock((unsigned char*)out.data(), (const unsigned char*)bytes.data(), (int)bytes.size());
    out.resize((std::size_t)n);
    return out;
}

std::string unbase64(const std::string& b64) {
    std::string out(3 * (b64.size() / 4) + 3, '\0');
    const int n = EVP_DecodeBlock((unsigned char*)out.data(), (const unsigned char*)b64.data(), (int)b64.size());
    if (n < 0) throw std::runtime_error("bad base64");
    std::size_t pad = 0;
    for (auto it = b64.rbegin(); it != b64.rend() && *it == '='; ++it) ++pad;
    out.resize((std::size_t)n - pad);
    return out;
}

struct Signer::Impl {
    std::string path;
    EVP_PKEY* key = nullptr;
    std::mutex mu;
    EVP_PKEY* get() {
        std::lock_guard<std::mutex> lk(mu);
        if (!key) {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) throw std::runtime_error("cannot open private key " + path);
            key = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
            std::fclose(f);
            if (!key) throw std::runtime_error("cannot parse private key " + path + ": " + ssl_err());
        }
        return key;
    }
    ~Impl() { if (key) EVP_PKEY_free(key); }
};

Signer::Signer(std::string pem_path) : impl_(new Impl) { impl_->path = std::move(pem_path); }
Signer::~Signer() = default;

std::string Signer::sign(const std::string& ts, const std::string& method, const std::string& path) {
    std::string m = method;
    for (auto& c : m) c = (char)std::toupper((unsigned char)c);
    const std::string msg = ts + m + path;
    EVP_PKEY* key = impl_->get();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pctx = nullptr;
    std::string sig;
    bool ok = EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), nullptr, key) == 1 &&
              EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
              EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) == 1 &&
              EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, SALT_LEN) == 1;
    size_t n = 0;
    if (ok) ok = EVP_DigestSign(ctx, nullptr, &n, (const unsigned char*)msg.data(), msg.size()) == 1;
    if (ok) {
        sig.resize(n);
        ok = EVP_DigestSign(ctx, (unsigned char*)sig.data(), &n, (const unsigned char*)msg.data(), msg.size()) == 1;
        sig.resize(n);
    }
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("RSA-PSS sign failed: " + ssl_err());
    return base64(sig);
}

bool Signer::verify(const std::string& message, const std::string& b64_signature) {
    const std::string sig = unbase64(b64_signature);
    EVP_PKEY* key = impl_->get();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pctx = nullptr;
    bool ok = EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), nullptr, key) == 1 &&
              EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
              EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) == 1 &&
              EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, SALT_LEN) == 1;
    if (ok) ok = EVP_DigestVerify(ctx, (const unsigned char*)sig.data(), sig.size(),
                                  (const unsigned char*)message.data(), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    ERR_clear_error();
    return ok;
}

}  // namespace kp::kalshi

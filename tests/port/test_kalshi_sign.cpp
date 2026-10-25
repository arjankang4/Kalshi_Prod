// kalshi::Signer against the reference's _sign (see gen_kalshi_sign.py). PSS is randomized, so the
// test is cross-VERIFICATION with every parameter pinned: the reference's signatures verify here at
// salt 32 and at nothing else, and ours verify the same way.
//
// KP_EMIT_SIGS=<file> writes this binary's own signatures as `ts method path sig` lines, for a
// one-off check that the reference's `cryptography` verifies them too.
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cstdlib>
#include <fstream>

#include "kp/kalshi/signer.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;

namespace {

// Verify with an explicit padding / salt length, independent of Signer's own settings.
bool verify_with(EVP_PKEY* key, const std::string& msg, const std::string& b64, int padding, int salt) {
    const std::string sig = kp::kalshi::unbase64(b64);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX* p = nullptr;
    bool ok = EVP_DigestVerifyInit(ctx, &p, EVP_sha256(), nullptr, key) == 1 &&
              EVP_PKEY_CTX_set_rsa_padding(p, padding) == 1;
    if (ok && padding == RSA_PKCS1_PSS_PADDING)
        ok = EVP_PKEY_CTX_set_rsa_mgf1_md(p, EVP_sha256()) == 1 && EVP_PKEY_CTX_set_rsa_pss_saltlen(p, salt) == 1;
    if (ok) ok = EVP_DigestVerify(ctx, (const unsigned char*)sig.data(), sig.size(),
                                  (const unsigned char*)msg.data(), msg.size()) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("kalshi_sign");
    T.expect({"key", "py_sig_verifies", "py_sig_salt20_refused", "py_sig_pkcs1_refused",
              "py_sig_tampered_refused", "cpp_sig_verifies", "cpp_sig_salt20_refused", "cpp_sig_randomized"});
    const std::string pem = dir + "/kalshi_sign_key.pem";
    kp::kalshi::Signer signer(pem);
    std::FILE* f = std::fopen(pem.c_str(), "rb");
    EVP_PKEY* key = f ? PEM_read_PrivateKey(f, nullptr, nullptr, nullptr) : nullptr;
    if (f) std::fclose(f);
    const char* emit = std::getenv("KP_EMIT_SIGS");
    std::ofstream em;
    if (emit) em.open(emit);

    for (const auto& line : read(dir, "kalshi_sign")) {
        const auto w = words(line);
        if (w[0] == "key") { T.hit("key", key != nullptr, line); continue; }
        if (w[0] != "sig") { T.hit("unknown_tag:" + w[0], false, line); continue; }
        const std::string &ts = w[1], &method = w[2], &path = w[3], &sig = w[4];
        const std::string msg = ts + upper(method) + path;
        T.hit("py_sig_verifies", signer.verify(msg, sig) && verify_with(key, msg, sig, RSA_PKCS1_PSS_PADDING, 32), line);
        T.hit("py_sig_salt20_refused", !verify_with(key, msg, sig, RSA_PKCS1_PSS_PADDING, 20), line);
        T.hit("py_sig_pkcs1_refused", !verify_with(key, msg, sig, RSA_PKCS1_PADDING, 0), line);
        T.hit("py_sig_tampered_refused", !signer.verify(msg + "x", sig), line);

        const std::string mine = signer.sign(ts, method, path);
        T.hit("cpp_sig_verifies", verify_with(key, msg, mine, RSA_PKCS1_PSS_PADDING, 32), line + " -> " + mine);
        T.hit("cpp_sig_salt20_refused", !verify_with(key, msg, mine, RSA_PKCS1_PSS_PADDING, 20), line);
        T.hit("cpp_sig_randomized", mine != sig && mine != signer.sign(ts, method, path), line);
        if (em) em << ts << " " << method << " " << path << " " << mine << "\n";
    }
    EVP_PKEY_free(key);
    return T.report();
}

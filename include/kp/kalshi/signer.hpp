// kalshi/client._sign: RSA-PSS over `{ts}{METHOD}{path}`, SHA-256, MGF1(SHA-256), salt length =
// SHA-256's digest size (32) -- `padding.PSS(mgf=MGF1(SHA256()), salt_length=SHA256.digest_size)`.
// Base64 (standard alphabet, padded). `path` includes /trade-api/v2 and EXCLUDES the query.
//
// PSS is randomized, so a signature is not reproducible byte for byte; the test is that each side's
// signature VERIFIES under the other's parameters (see test_kalshi_sign).
#pragma once
#include <memory>
#include <string>

namespace kp::kalshi {

class Signer {
public:
    // The key is loaded lazily on first sign(), as _private_key() does. Unencrypted PEM.
    explicit Signer(std::string pem_path);
    ~Signer();
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    std::string sign(const std::string& timestamp_ms, const std::string& method, const std::string& path);

    // Verify a base64 signature with this key's public half, salt length pinned to 32. For tests.
    bool verify(const std::string& message, const std::string& b64_signature);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string base64(const std::string& bytes);
std::string unbase64(const std::string& b64);

}  // namespace kp::kalshi

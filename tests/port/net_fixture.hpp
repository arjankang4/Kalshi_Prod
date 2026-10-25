// A scripted local server for the transport tests: plain TCP or TLS with a self-signed
// certificate minted at runtime. No internet, no Python: the tests that use it must be runnable in
// the minutes before a session, like every other suite here.
//
// Each accepted connection is handed to the test's handler on its own thread; the handler drives
// the conversation byte by byte with the blocking helpers below, so a test can craft any framing
// it likes -- including the malformed and the adversarial.
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kp::test::net {

// A throwaway RSA key and a self-signed certificate for CN=localhost, SAN DNS:localhost only (so
// connecting to 127.0.0.1 is a hostname MISMATCH -- which one test relies on).
struct SelfSigned {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    std::string ca_path;

    explicit SelfSigned(const std::string& dir) {
        key = EVP_RSA_gen(2048);
        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600L * 24);
        X509_set_pubkey(cert, key);
        X509_NAME* nm = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0);
        X509_set_issuer_name(cert, nm);
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, "DNS:localhost");
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_basic_constraints, "critical,CA:TRUE");
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        X509_sign(cert, key, EVP_sha256());
        ca_path = dir + "/kp-test-ca.pem";
        FILE* f = std::fopen(ca_path.c_str(), "w");
        PEM_write_X509(f, cert);
        std::fclose(f);
    }
    ~SelfSigned() {
        X509_free(cert);
        EVP_PKEY_free(key);
        std::remove(ca_path.c_str());
    }
};

class Conn {
public:
    Conn(int fd, SSL* ssl) : fd_(fd), ssl_(ssl) {}
    ~Conn() { close(); }

    // Blocking read of exactly n bytes; false on EOF/timeout.
    bool read_n(std::string& out, std::size_t n, int timeout_ms = 5000) {
        out.clear();
        while (out.size() < n) {
            if (buf_.empty() && !fill(timeout_ms)) return false;
            const std::size_t k = std::min(n - out.size(), buf_.size());
            out.append(buf_, 0, k);
            buf_.erase(0, k);
        }
        return true;
    }
    // Read through "\r\n\r\n" (an HTTP head); false on EOF/timeout.
    bool read_head(std::string& head, int timeout_ms = 5000) {
        for (;;) {
            const auto p = buf_.find("\r\n\r\n");
            if (p != std::string::npos) {
                head = buf_.substr(0, p + 4);
                buf_.erase(0, p + 4);
                return true;
            }
            if (!fill(timeout_ms)) return false;
        }
    }
    bool write(const std::string& s) {
        std::size_t off = 0;
        while (off < s.size()) {
            const int r = ssl_ ? SSL_write(ssl_, s.data() + off, (int)(s.size() - off))
                               : (int)::send(fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (r <= 0) return false;
            off += (std::size_t)r;
        }
        return true;
    }
    // Is the peer gone (EOF) within timeout? Used to prove a request was NOT re-sent.
    bool eof_within(int timeout_ms) { return !fill(timeout_ms) ; }
    void close() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    }
    // Abortive close: no TLS close_notify, no FIN handshake niceties.
    void drop() {
        if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
    std::string& pending() { return buf_; }

private:
    bool fill(int timeout_ms) {
        if (fd_ < 0) return false;
        if (ssl_ && SSL_pending(ssl_) == 0) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, timeout_ms) <= 0) return false;
        } else if (!ssl_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, timeout_ms) <= 0) return false;
        }
        char b[65536];
        const int r = ssl_ ? SSL_read(ssl_, b, sizeof b) : (int)::recv(fd_, b, sizeof b, 0);
        if (r <= 0) return false;
        buf_.append(b, (std::size_t)r);
        return true;
    }
    int fd_;
    SSL* ssl_;
    std::string buf_;
};

class Server {
public:
    using Handler = std::function<void(Conn&, int index)>;

    Server(Handler h, const SelfSigned* tls = nullptr) : h_(std::move(h)) {
        lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(lfd_, (sockaddr*)&a, sizeof a) != 0 || ::listen(lfd_, 16) != 0)
            throw std::runtime_error("test server: bind/listen failed");
        socklen_t sl = sizeof a;
        getsockname(lfd_, (sockaddr*)&a, &sl);
        port_ = ntohs(a.sin_port);
        if (tls) {
            ctx_ = SSL_CTX_new(TLS_server_method());
            SSL_CTX_use_certificate(ctx_, tls->cert);
            SSL_CTX_use_PrivateKey(ctx_, tls->key);
        }
        th_ = std::thread([this] { loop(); });
    }
    ~Server() { stop(); }

    void stop() {
        if (stopping_.exchange(true)) return;
        ::shutdown(lfd_, SHUT_RDWR);
        ::close(lfd_);
        if (th_.joinable()) th_.join();
        for (auto& t : workers_) if (t.joinable()) t.join();
        if (ctx_) SSL_CTX_free(ctx_);
    }
    int port() const { return port_; }
    int accepted() const { return accepted_; }

private:
    void loop() {
        for (;;) {
            pollfd p{lfd_, POLLIN, 0};
            const int r = ::poll(&p, 1, 100);
            if (stopping_) return;
            if (r <= 0) continue;
            const int fd = ::accept(lfd_, nullptr, nullptr);
            if (fd < 0) { if (stopping_) return; continue; }
            const int idx = accepted_++;
            workers_.emplace_back([this, fd, idx] {
                SSL* ssl = nullptr;
                if (ctx_) {
                    ssl = SSL_new(ctx_);
                    SSL_set_fd(ssl, fd);
                    if (SSL_accept(ssl) != 1) {            // e.g. the client refused our cert
                        SSL_free(ssl);
                        ::close(fd);
                        return;
                    }
                }
                Conn c(fd, ssl);
                try { h_(c, idx); } catch (...) {}
            });
        }
    }
    Handler h_;
    int lfd_ = -1;
    int port_ = 0;
    SSL_CTX* ctx_ = nullptr;
    std::thread th_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0};
};

// --- WebSocket frames, server side ---------------------------------------------------------------
inline std::string ws_frame(int opcode, const std::string& payload, bool fin = true, bool mask = false) {
    std::string f;
    f.push_back((char)((fin ? 0x80 : 0) | opcode));
    const std::size_t n = payload.size();
    const char m = mask ? (char)0x80 : 0;
    if (n < 126) f.push_back((char)(m | (char)n));
    else if (n <= 0xFFFF) { f.push_back((char)(m | 126)); f.push_back((char)(n >> 8)); f.push_back((char)(n & 0xFF)); }
    else { f.push_back((char)(m | 127)); for (int i = 7; i >= 0; --i) f.push_back((char)((n >> (8 * i)) & 0xFF)); }
    std::string p = payload;
    if (mask) {
        const char k[4] = {0x11, 0x22, 0x33, 0x44};
        f.append(k, 4);
        for (std::size_t i = 0; i < p.size(); ++i) p[i] = (char)(p[i] ^ k[i & 3]);
    }
    return f + p;
}

struct ClientFrame {
    bool fin = false, masked = false;
    int opcode = -1;
    int len_form = 0;          // 7, 16 or 64: the length encoding the client chose
    std::string mask_key;
    std::string payload;       // unmasked
};

inline bool read_client_frame(Conn& c, ClientFrame& f, int timeout_ms = 5000) {
    std::string h;
    if (!c.read_n(h, 2, timeout_ms)) return false;
    f.fin = (unsigned char)h[0] & 0x80;
    f.opcode = (unsigned char)h[0] & 0x0F;
    f.masked = (unsigned char)h[1] & 0x80;
    std::uint64_t n = (unsigned char)h[1] & 0x7F;
    f.len_form = 7;
    if (n == 126) {
        if (!c.read_n(h, 2, timeout_ms)) return false;
        n = ((std::uint64_t)(unsigned char)h[0] << 8) | (unsigned char)h[1];
        f.len_form = 16;
    } else if (n == 127) {
        if (!c.read_n(h, 8, timeout_ms)) return false;
        n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | (unsigned char)h[(std::size_t)i];
        f.len_form = 64;
    }
    f.mask_key.clear();
    if (f.masked && !c.read_n(f.mask_key, 4, timeout_ms)) return false;
    if (!c.read_n(f.payload, (std::size_t)n, timeout_ms)) return false;
    if (f.masked)
        for (std::size_t i = 0; i < f.payload.size(); ++i) f.payload[i] = (char)(f.payload[i] ^ f.mask_key[i & 3]);
    return true;
}

// Header value from an HTTP head (case-insensitive name), "" if absent; `count` of that header.
inline std::string header(const std::string& head, const std::string& name, int* count = nullptr) {
    std::string v;
    int n = 0;
    std::size_t pos = 0;
    while ((pos = head.find("\r\n", pos)) != std::string::npos) {
        pos += 2;
        const auto e = head.find("\r\n", pos);
        const std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        const auto c = line.find(':');
        if (c != std::string::npos && c == name.size() && strncasecmp(line.c_str(), name.c_str(), c) == 0) {
            std::size_t s = c + 1;
            while (s < line.size() && line[s] == ' ') ++s;
            v = line.substr(s);
            ++n;
        }
    }
    if (count) *count = n;
    return v;
}

}  // namespace kp::test::net

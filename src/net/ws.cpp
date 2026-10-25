// RFC 6455 client over a non-blocking socket, optionally wrapped in OpenSSL TLS.
//
// THREADING, and why every SSL_* call takes one lock. OpenSSL does not allow one SSL object to be
// driven from two threads at once, and this class is used exactly that way: a pump thread in recv()
// and the quote loop in send_text(). So the socket is non-blocking, the reader POLLS WITHOUT the
// lock and takes it only around each non-blocking SSL_read, and a writer holds it for the whole
// frame (so frames never interleave on the wire). A reader stuck in poll therefore never blocks a
// command from going out, and a command never corrupts the reader's TLS state.
//
// NO ORIGIN HEADER, EVER: Kalshi's edge answers an upgrade that carries one with 403 before it
// looks at the signature. connect() refuses a caller-supplied Origin rather than trusting that no
// caller will ever pass one.
#include "kp/net/ws.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

namespace kp::net {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t MAX_MESSAGE = 64u << 20;   // a sanity bound, far above any Kalshi frame

std::string ssl_errors() {
    std::string out;
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof buf);
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out;
}

int ms_left(Clock::time_point deadline) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return d < 0 ? 0 : (d > 0x7fffffff ? 0x7fffffff : (int)d);
}

std::string b64(const unsigned char* p, std::size_t n) {
    std::string out(4 * ((n + 2) / 3) + 1, '\0');
    const int k = EVP_EncodeBlock((unsigned char*)out.data(), p, (int)n);
    out.resize((std::size_t)k);
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

struct Url {
    bool tls = true;
    std::string host;
    int port = 443;
    std::string target = "/";
};

Url parse_url(const std::string& url) {
    Url u;
    std::string rest;
    if (url.rfind("wss://", 0) == 0) { u.tls = true; u.port = 443; rest = url.substr(6); }
    else if (url.rfind("ws://", 0) == 0) { u.tls = false; u.port = 80; rest = url.substr(5); }
    else throw WsError("unsupported websocket url: " + url);
    const auto slash = rest.find_first_of("/?");
    std::string hostport = rest.substr(0, slash);
    if (slash != std::string::npos) {
        u.target = rest.substr(slash);
        if (u.target[0] == '?') u.target = "/" + u.target;
    }
    if (!hostport.empty() && hostport[0] == '[') {                 // [v6]:port
        const auto rb = hostport.find(']');
        if (rb == std::string::npos) throw WsError("bad host in url: " + url);
        u.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') u.port = std::stoi(hostport.substr(rb + 2));
    } else {
        const auto colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            u.host = hostport.substr(0, colon);
            u.port = std::stoi(hostport.substr(colon + 1));
        } else {
            u.host = hostport;
        }
    }
    if (u.host.empty()) throw WsError("no host in url: " + url);
    return u;
}

}  // namespace

struct WebSocket::Impl {
    int fd = -1;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    std::mutex io;                 // guards every SSL_* call and every write
    std::atomic<bool> open{false};
    std::atomic<bool> closed{false};
    bool close_sent = false;       // under io
    std::string ca_file;

    std::string rbuf;              // unparsed bytes (reader thread only)
    std::string msg;               // a fragmented message being reassembled
    bool in_msg = false;

    ~Impl() { release(); }

    void release() {
        if (ssl) { SSL_free(ssl); ssl = nullptr; }
        if (ctx) { SSL_CTX_free(ctx); ctx = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
        open = false;
    }

    bool wait(short ev, Clock::time_point deadline) {
        pollfd p{fd, ev, 0};
        for (;;) {
            const int r = ::poll(&p, 1, ms_left(deadline));
            if (r > 0) return true;
            if (r == 0) return false;
            if (errno != EINTR) throw WsError(std::string("poll: ") + std::strerror(errno));
        }
    }

    // One non-blocking read attempt. >0 bytes; 0 means "would block"; throws WsClosed on EOF.
    long read_once(char* buf, std::size_t n, short& want) {
        std::lock_guard<std::mutex> lk(io);
        if (closed) throw WsClosed("websocket closed", 0);
        if (ssl) {
            ERR_clear_error();
            const int r = SSL_read(ssl, buf, (int)n);
            if (r > 0) return r;
            const int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_READ) { want = POLLIN; return 0; }
            if (e == SSL_ERROR_WANT_WRITE) { want = POLLOUT; return 0; }
            if (e == SSL_ERROR_ZERO_RETURN) throw WsClosed("connection closed by peer (TLS close_notify)", 0);
            if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) {
                if (r == 0 || errno == 0) throw WsClosed("connection closed by peer", 0);
                throw WsClosed(std::string("socket error: ") + std::strerror(errno), 0);
            }
            throw WsClosed("TLS read error: " + ssl_errors(), 0);
        }
        const ssize_t r = ::recv(fd, buf, n, 0);
        if (r > 0) return (long)r;
        if (r == 0) throw WsClosed("connection closed by peer", 0);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { want = POLLIN; return 0; }
        throw WsClosed(std::string("socket error: ") + std::strerror(errno), 0);
    }

    // Append at least one byte to rbuf, or return false at the deadline.
    bool fill(Clock::time_point deadline) {
        char buf[16384];
        for (;;) {
            short want = POLLIN;
            const long r = read_once(buf, sizeof buf, want);
            if (r > 0) { rbuf.append(buf, (std::size_t)r); return true; }
            if (!wait(want, deadline)) return false;
        }
    }

    // Caller holds `io`.
    void write_all_locked(const char* p, std::size_t n, Clock::time_point deadline) {
        while (n > 0) {
            if (ssl) {
                ERR_clear_error();
                const int r = SSL_write(ssl, p, (int)n);
                if (r > 0) { p += r; n -= (std::size_t)r; continue; }
                const int e = SSL_get_error(ssl, r);
                short ev = 0;
                if (e == SSL_ERROR_WANT_WRITE) ev = POLLOUT;
                else if (e == SSL_ERROR_WANT_READ) ev = POLLIN;
                else throw WsClosed("TLS write error: " + ssl_errors(), 0);
                if (!wait(ev, deadline)) throw WsError("websocket write timed out");
            } else {
                const ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
                if (r > 0) { p += r; n -= (std::size_t)r; continue; }
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    if (!wait(POLLOUT, deadline)) throw WsError("websocket write timed out");
                    continue;
                }
                throw WsClosed(std::string("socket write error: ") + std::strerror(errno), 0);
            }
        }
    }

    // Client frames are always masked (RFC 6455 5.3) with a fresh random key.
    static std::string frame(int opcode, const std::string& payload) {
        std::string f;
        const std::size_t n = payload.size();
        f.push_back((char)(0x80 | (opcode & 0x0F)));
        if (n < 126) {
            f.push_back((char)(0x80 | n));
        } else if (n <= 0xFFFF) {
            f.push_back((char)(0x80 | 126));
            f.push_back((char)((n >> 8) & 0xFF));
            f.push_back((char)(n & 0xFF));
        } else {
            f.push_back((char)(0x80 | 127));
            for (int i = 7; i >= 0; --i) f.push_back((char)((n >> (8 * i)) & 0xFF));
        }
        unsigned char key[4];
        if (RAND_bytes(key, 4) != 1) throw WsError("RAND_bytes failed");
        f.append((const char*)key, 4);
        const std::size_t off = f.size();
        f += payload;
        for (std::size_t i = 0; i < n; ++i) f[off + i] = (char)(f[off + i] ^ (char)key[i & 3]);
        return f;
    }

    void send_frame(int opcode, const std::string& payload) {
        const std::string f = frame(opcode, payload);
        std::lock_guard<std::mutex> lk(io);
        if (closed || !open) throw WsClosed("websocket is not connected", 0);
        if (opcode == 0x8) {
            if (close_sent) return;
            close_sent = true;
        }
        write_all_locked(f.data(), f.size(), Clock::now() + std::chrono::seconds(10));
    }

    void shutdown_socket() {
        std::lock_guard<std::mutex> lk(io);
        closed = true;
        if (ssl) { ERR_clear_error(); SSL_shutdown(ssl); }
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    }
};

WebSocket::WebSocket() : impl_(new Impl) {}
WebSocket::~WebSocket() { delete impl_; }

void WebSocket::set_ca_file(const std::string& path) { impl_->ca_file = path; }

bool WebSocket::connected() const { return impl_->open && !impl_->closed; }

void WebSocket::connect(const std::string& url,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        double timeout_s) {
    Impl& I = *impl_;
    for (const auto& [k, v] : headers)
        if (lower(k) == "origin")
            throw WsError("refusing to send an Origin header (Kalshi answers 403 to one)");
    I.release();
    I.closed = false;
    I.close_sent = false;
    I.rbuf.clear();
    I.msg.clear();
    I.in_msg = false;
    const Url u = parse_url(url);
    const auto deadline = Clock::now() + std::chrono::milliseconds((long)(timeout_s * 1000.0));

    // --- TCP ---------------------------------------------------------------------------------
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(u.port);
    if (const int g = getaddrinfo(u.host.c_str(), port.c_str(), &hints, &res); g != 0)
        throw WsError("resolve " + u.host + ": " + gai_strerror(g));
    std::string last_err = "no address";
    for (addrinfo* a = res; a; a = a->ai_next) {
        const int fd = ::socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) { last_err = std::strerror(errno); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int r;
            do { r = ::poll(&p, 1, ms_left(deadline)); } while (r < 0 && errno == EINTR);
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (r > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                I.fd = fd;
                break;
            }
            last_err = r == 0 ? "connect timed out" : std::strerror(soerr ? soerr : errno);
        } else {
            last_err = std::strerror(errno);
        }
        ::close(fd);
    }
    freeaddrinfo(res);
    if (I.fd < 0) throw WsError("connect " + u.host + ":" + port + ": " + last_err);

    // --- TLS: SNI + chain verification + hostname check ---------------------------------------
    if (u.tls) {
        I.ctx = SSL_CTX_new(TLS_client_method());
        if (!I.ctx) { I.release(); throw WsError("SSL_CTX_new: " + ssl_errors()); }
        SSL_CTX_set_min_proto_version(I.ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(I.ctx, SSL_VERIFY_PEER, nullptr);
        const int ok = I.ca_file.empty() ? SSL_CTX_set_default_verify_paths(I.ctx)
                                         : SSL_CTX_load_verify_locations(I.ctx, I.ca_file.c_str(), nullptr);
        if (ok != 1) { const auto e = ssl_errors(); I.release(); throw WsError("CA store: " + e); }
        SSL_CTX_set_mode(I.ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        I.ssl = SSL_new(I.ctx);
        SSL_set_fd(I.ssl, I.fd);
        in6_addr a6;
        in_addr a4;
        const bool is_ip = inet_pton(AF_INET, u.host.c_str(), &a4) == 1 ||
                           inet_pton(AF_INET6, u.host.c_str(), &a6) == 1;
        if (!is_ip) SSL_set_tlsext_host_name(I.ssl, u.host.c_str());   // SNI (never for an IP)
        X509_VERIFY_PARAM* vp = SSL_get0_param(I.ssl);
        if (is_ip) X509_VERIFY_PARAM_set1_ip_asc(vp, u.host.c_str());
        else { X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
               SSL_set1_host(I.ssl, u.host.c_str()); }
        for (;;) {
            ERR_clear_error();
            const int r = SSL_connect(I.ssl);
            if (r == 1) break;
            const int e = SSL_get_error(I.ssl, r);
            short ev = 0;
            if (e == SSL_ERROR_WANT_READ) ev = POLLIN;
            else if (e == SSL_ERROR_WANT_WRITE) ev = POLLOUT;
            else {
                std::string why = ssl_errors();
                const long vr = SSL_get_verify_result(I.ssl);
                if (vr != X509_V_OK) why = std::string("certificate verify failed: ") +
                                           X509_verify_cert_error_string(vr);
                I.release();
                throw WsError("TLS handshake with " + u.host + ": " + why);
            }
            if (!I.wait(ev, deadline)) { I.release(); throw WsError("TLS handshake timed out"); }
        }
    }
    I.open = true;

    // --- the upgrade ---------------------------------------------------------------------------
    unsigned char nonce[16];
    RAND_bytes(nonce, sizeof nonce);
    const std::string key = b64(nonce, sizeof nonce);
    const bool default_port = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
    std::string host_hdr = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    if (!default_port) host_hdr += ":" + std::to_string(u.port);
    std::string req = "GET " + u.target + " HTTP/1.1\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Host: " + host_hdr + "\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "Connection: Upgrade\r\n";
    for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    req += "\r\n";
    try {
        std::lock_guard<std::mutex> lk(I.io);
        I.write_all_locked(req.data(), req.size(), deadline);
    } catch (const WsError& e) {
        I.release();
        throw WsError(std::string("handshake send: ") + e.what());
    }
    std::size_t end;
    try {
        while ((end = I.rbuf.find("\r\n\r\n")) == std::string::npos) {
            if (I.rbuf.size() > 65536) throw WsError("handshake response too large");
            if (!I.fill(deadline)) throw WsError("handshake timed out");
        }
    } catch (const WsError& e) {
        I.release();
        throw WsError(std::string("handshake: ") + e.what());
    }
    const std::string head = I.rbuf.substr(0, end);
    I.rbuf.erase(0, end + 4);
    int status = 0;
    if (std::sscanf(head.c_str(), "HTTP/%*d.%*d %d", &status) != 1) {
        I.release();
        throw WsError("handshake: malformed status line");
    }
    if (status != 101) {
        I.release();
        throw WsError("handshake " + std::to_string(status));
    }
    std::string upgrade, connection, accept;
    std::size_t pos = head.find("\r\n");
    while (pos != std::string::npos && pos + 2 < head.size()) {
        const std::size_t nl = head.find("\r\n", pos + 2);
        const std::string line = head.substr(pos + 2, nl == std::string::npos ? std::string::npos : nl - pos - 2);
        const auto c = line.find(':');
        if (c != std::string::npos) {
            const std::string name = lower(line.substr(0, c));
            std::size_t v = c + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
            std::string val = line.substr(v);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
            if (name == "upgrade") upgrade = lower(val);
            else if (name == "connection") connection = lower(val);
            else if (name == "sec-websocket-accept") accept = val;
        }
        pos = nl;
    }
    const std::string guid = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)guid.data(), guid.size(), sha);
    if (upgrade != "websocket" || connection.find("upgrade") == std::string::npos ||
        accept != b64(sha, sizeof sha)) {
        I.release();
        throw WsError("handshake: invalid upgrade response");
    }
}

void WebSocket::send_text(const std::string& payload) { impl_->send_frame(0x1, payload); }

std::optional<std::string> WebSocket::recv(double timeout_s) {
    Impl& I = *impl_;
    if (!I.open) throw WsClosed("websocket is not connected", 0);
    const auto deadline = Clock::now() + std::chrono::microseconds((long long)(timeout_s * 1e6));
    for (;;) {
        // --- parse one frame out of rbuf, if a whole one is there ---
        const std::string& b = I.rbuf;
        if (b.size() >= 2) {
            const auto b0 = (unsigned char)b[0], b1 = (unsigned char)b[1];
            const bool fin = b0 & 0x80;
            const int rsv = b0 & 0x70;
            const int op = b0 & 0x0F;
            const bool masked = b1 & 0x80;
            std::uint64_t len = b1 & 0x7F;
            std::size_t hdr = 2;
            bool have = true;
            if (len == 126) {
                if (b.size() < 4) have = false;
                else { len = ((std::uint64_t)(unsigned char)b[2] << 8) | (unsigned char)b[3]; hdr = 4; }
            } else if (len == 127) {
                if (b.size() < 10) have = false;
                else {
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | (unsigned char)b[2 + (std::size_t)i];
                    hdr = 10;
                }
            }
            if (have) {
                if (rsv) throw WsError("protocol error: reserved bits set without an extension");
                if (len > MAX_MESSAGE) throw WsError("protocol error: frame too large");
                if (op >= 0x8 && (!fin || len > 125))
                    throw WsError("protocol error: fragmented or oversized control frame");
                const std::size_t mlen = masked ? 4 : 0;
                if (b.size() >= hdr + mlen + len) {
                    std::string payload = b.substr(hdr + mlen, (std::size_t)len);
                    if (masked) {                  // servers must not mask; tolerated, unmasked
                        for (std::size_t i = 0; i < payload.size(); ++i)
                            payload[i] = (char)(payload[i] ^ b[hdr + (i & 3)]);
                    }
                    I.rbuf.erase(0, hdr + mlen + (std::size_t)len);
                    switch (op) {
                        case 0x0:
                            if (!I.in_msg) throw WsError("protocol error: continuation without a start");
                            I.msg += payload;
                            if (I.msg.size() > MAX_MESSAGE) throw WsError("message too large");
                            if (fin) { I.in_msg = false; std::string m; m.swap(I.msg); return m; }
                            continue;
                        case 0x1:
                        case 0x2:
                            if (I.in_msg) throw WsError("protocol error: new message inside a fragmented one");
                            if (fin) return payload;
                            I.in_msg = true;
                            I.msg = std::move(payload);
                            continue;
                        case 0x8: {
                            int code = 0;
                            if (payload.size() >= 2)
                                code = ((unsigned char)payload[0] << 8) | (unsigned char)payload[1];
                            std::string echo;
                            if (code) { echo.push_back((char)(code >> 8)); echo.push_back((char)(code & 0xFF)); }
                            try { I.send_frame(0x8, echo); } catch (const WsError&) {}
                            I.shutdown_socket();
                            throw WsClosed("closed by peer" + (payload.size() > 2 ? ": " + payload.substr(2) : std::string()), code);
                        }
                        case 0x9:
                            I.send_frame(0xA, payload);        // pong with the ping's payload
                            continue;
                        case 0xA:
                            continue;
                        default:
                            throw WsError("protocol error: unknown opcode " + std::to_string(op));
                    }
                }
            }
        }
        if (!I.fill(deadline)) return std::nullopt;
    }
}

void WebSocket::close(int code) {
    Impl& I = *impl_;
    if (!I.open || I.closed) return;
    std::string p;
    if (code) { p.push_back((char)(code >> 8)); p.push_back((char)(code & 0xFF)); }
    try { I.send_frame(0x8, p); } catch (const WsError&) {}
    I.shutdown_socket();
}

}  // namespace kp::net

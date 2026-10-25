// HTTPS with keep-alive -- the transport under the Kalshi and Massive REST clients.
//
// One HttpClient holds ONE connection (libcurl easy handle) and is NOT thread-safe: the reference
// keeps a thread-local http.client connection per thread (kalshi/client.py `_local`), so give each
// thread its own client. Keep-alive matters -- the per-request TCP+TLS handshake measured 40ms of a
// 139ms poll -- and `connects()` exposes the count so a caller can tell a reused socket from a new
// one (CURLINFO_NUM_CONNECTS), which the Kalshi client needs: a failure on a REUSED socket is a
// dropped idle keep-alive and a GET may be retried; on a fresh one it may not.
#pragma once
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kp::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
    long status = 0;
    std::string body;
    Headers headers;          // lower-cased names, in arrival order
    bool reused = false;      // true if this request went out on an already-open connection
    double elapsed_ms = 0.0;
};

// Transport failure: DNS, connect, TLS, timeout, reset. NOT an HTTP error status -- those come
// back as an HttpResponse with that status.
class HttpError : public std::runtime_error {
public:
    HttpError(const std::string& what, bool was_reused) : std::runtime_error(what), reused(was_reused) {}
    bool reused;
};

class HttpClient {
public:
    explicit HttpClient(std::string base_url);        // scheme://host[:port], no trailing slash
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // `target` is the path plus any "?query", exactly as it goes on the request line.
    HttpResponse request(const std::string& method, const std::string& target,
                         const Headers& headers = {}, const std::string& body = "",
                         double timeout_s = 20.0);
    void drop();                                       // close the connection; next call reconnects
    long connects() const;                             // TCP connections opened so far

private:
    struct Impl;
    Impl* impl_;
};

// urllib.parse.quote_plus / urlencode, byte for byte (safe set: letters, digits, "_.-~").
std::string quote_plus(const std::string& s);
std::string urlencode(const std::vector<std::pair<std::string, std::string>>& params);

}  // namespace kp::net

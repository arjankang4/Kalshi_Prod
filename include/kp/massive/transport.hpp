// The seams between the Massive clients and the wire: a clock, a WebSocket, an HTTP GET.
//
// Everything under kp/massive that holds STATE -- the option quote collector, the futures and index
// feeds, the paged REST helpers -- is written against these three, never against kp::net directly.
// That is what lets the state be golden-tested against the Python by replaying identical frame
// sequences and REST payloads (tools/golden/gen_massive_*.py monkeypatches `websocket`, `urllib`
// and `time` in the reference with the same canned data), with no socket and no sleeping.
//
// The real implementations -- kp::net::WebSocket, kp::net::HttpClient and the system clock -- are
// bound in src/massive/net_adapters.cpp and nowhere else, so a test binary that never names
// `net_ws_factory()` / `net_http_get()` never links the socket code at all.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace kp::massive {

// time.monotonic / time.time_ns / time.sleep, injectable. `system()` is the real thing:
// CLOCK_MONOTONIC (what CPython's time.monotonic reads on Linux), CLOCK_REALTIME in ns, and a
// real sleep.
struct Clock {
    std::function<double()> monotonic;
    std::function<long long()> time_ns;
    std::function<void(double)> sleep;
    static Clock system();
};

// A WebSocket as the Massive clients use it. Mirrors the subset of `websocket-client` the
// reference calls: create_connection(url, timeout) / send / recv / close.
//
// recv(timeout) returns nullopt on a timeout (websocket-client's WebSocketTimeoutException, which
// every pump treats as "nothing yet") and THROWS on anything else -- a closed or broken socket.
class WsTransport {
public:
    virtual ~WsTransport() = default;
    virtual void connect(const std::string& url, double timeout_s) = 0;
    virtual void send_text(const std::string& payload) = 0;
    virtual std::optional<std::string> recv(double timeout_s) = 0;
    virtual void close() = 0;
};
using WsFactory = std::function<std::unique_ptr<WsTransport>()>;

// A recv() inside a handshake that timed out. Distinct from a MassiveError on purpose: the
// collector retries ONLY a MassiveError naming max_connections, and a timeout is not one.
class WsTimeout : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One HTTP GET of a fully-formed URL (scheme://host/path?query). Returns the status and body for
// ANY status; throws (std::exception) only on a transport failure -- DNS, connect, TLS, timeout --
// which is what urllib reports as URLError/OSError. Must be callable from several threads at once.
struct HttpResult {
    long status = 0;
    std::string body;
};
using HttpGet = std::function<HttpResult(const std::string& url, double timeout_s)>;

// The real transports, over kp::net (src/massive/net_adapters.cpp). The HTTP getter keeps one
// keep-alive kp::net::HttpClient per (thread, scheme://host) -- HttpClient is not thread-safe and
// the REST helpers are called from the quote loop, the futures poller and the rvol backfill.
WsFactory net_ws_factory();
HttpGet net_http_get();

}  // namespace kp::massive

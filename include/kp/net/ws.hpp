// A WebSocket client, RFC 6455 over OpenSSL, hand-rolled.
//
// Hand-rolled because this box's libcurl has NO WebSocket support (`wss://` fails with
// CURLE_UNSUPPORTED_PROTOCOL), and the reference's `websocket-client` is what it replaces.
//
// DO NOT SEND AN `Origin` HEADER. Kalshi answers a handshake carrying one with 403 -- measured,
// see the kalshi-write-budget note. `websocket-client` sends none unless asked, and neither do we.
//
// Threading: ONE reader thread calls recv(); send_text() may be called from any thread (it takes
// an internal lock), because the reference sends subscribe/update commands from the loop thread
// while a pump thread reads. Control frames are handled inside recv(): a ping is answered with a
// pong carrying the same payload, a close is answered and then surfaces as WsClosed.
#pragma once
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kp::net {

class WsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
// The peer closed (or the socket died). `code` is the close code when one was received, else 0.
class WsClosed : public WsError {
public:
    WsClosed(const std::string& what, int close_code) : WsError(what), code(close_code) {}
    int code;
};

class WebSocket {
public:
    WebSocket();
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // wss://host[:port]/path?query. Throws WsError (including the HTTP status on a refused
    // upgrade, e.g. "handshake 403").
    void connect(const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers = {},
                 double timeout_s = 10.0);
    void send_text(const std::string& payload);

    // The next complete TEXT (or binary) message, reassembled across continuation frames.
    // nullopt if none arrived within timeout_s. Throws WsClosed when the connection ends.
    std::optional<std::string> recv(double timeout_s);

    void close(int code = 1000);
    bool connected() const;

    // Verify the server against this PEM bundle instead of the system store. For tests against a
    // local server with a self-signed certificate; verification itself is never switched off.
    // A `ws://` URL is plain TCP (local tests only); `wss://` always verifies chain and hostname.
    void set_ca_file(const std::string& pem_path);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace kp::net

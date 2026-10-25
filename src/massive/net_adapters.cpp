// The ONE translation unit in kp/massive that names kp::net. Everything else is written against
// the seams in kp/massive/transport.hpp, so a test that never calls net_ws_factory() /
// net_http_get() / Rest::live() never links the socket layer.
#include <map>
#include <memory>

#include "kp/massive/rest.hpp"
#include "kp/massive/transport.hpp"
#include "kp/net/http.hpp"
#include "kp/net/ws.hpp"

namespace kp::massive {

namespace {

class NetWs final : public WsTransport {
public:
    void connect(const std::string& url, double timeout_s) override { ws_.connect(url, {}, timeout_s); }
    void send_text(const std::string& payload) override { ws_.send_text(payload); }
    std::optional<std::string> recv(double timeout_s) override { return ws_.recv(timeout_s); }
    void close() override {
        if (ws_.connected()) ws_.close(1000);
    }
    ~NetWs() override {
        try { close(); } catch (...) {}
    }

private:
    kp::net::WebSocket ws_;
};

// "https://api.massive.com/v3/x?a=b" -> {"https://api.massive.com", "/v3/x?a=b"}
std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto scheme = url.find("://");
    const auto path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (path == std::string::npos) return {url, "/"};
    return {url.substr(0, path), url.substr(path)};
}

}  // namespace

WsFactory net_ws_factory() {
    return [] { return std::unique_ptr<WsTransport>(new NetWs()); };
}

HttpGet net_http_get() {
    return [](const std::string& url, double timeout_s) {
        // One keep-alive client per (thread, origin): HttpClient is not thread-safe, and the
        // futures poller, the quote loop and the rvol backfill all call in from their own threads.
        thread_local std::map<std::string, std::unique_ptr<kp::net::HttpClient>> clients;
        const auto [base, target] = split_url(url);
        auto& c = clients[base];
        if (!c) c = std::make_unique<kp::net::HttpClient>(base);
        const kp::net::HttpResponse r = c->request("GET", target, {}, "", timeout_s);
        return HttpResult{r.status, r.body};
    };
}

std::unique_ptr<Rest> Rest::live(const std::string& kalshi_root) {
    return std::make_unique<Rest>(load_api_key(kalshi_root), net_http_get(), Clock::system());
}

}  // namespace kp::massive

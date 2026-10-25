// net::HttpClient against a scripted local HTTP server.
//
// THE PROPERTY THAT MATTERS MOST is the last section: a request that dies on a REUSED keep-alive
// connection is surfaced as HttpError(reused=true) and NOT re-sent. libcurl's default is to re-send
// it on a fresh connection -- for POST and DELETE too -- which for an order create is a double-send.
// The server counts the copies it received; exactly one is the pass.
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "harness.hpp"
#include "kp/net/http.hpp"
#include "net_fixture.hpp"

using namespace kp::test;
using namespace kp::test::net;
using kp::net::HttpClient;
using kp::net::HttpError;

namespace {

struct Req {
    std::string head, body;
    int conn = -1;
};

// Read one request (head + Content-Length body). false on EOF.
bool read_req(Conn& c, Req& r, int idx, int timeout_ms = 5000) {
    if (!c.read_head(r.head, timeout_ms)) return false;
    const std::string cl = header(r.head, "Content-Length");
    r.body.clear();
    if (!cl.empty() && std::stoul(cl) > 0 && !c.read_n(r.body, std::stoul(cl), timeout_ms)) return false;
    r.conn = idx;
    return true;
}

void reply(Conn& c, int status, const std::string& body, const std::string& extra = "") {
    c.write("HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\n"
            "X-Mixed-Case: Yes\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body);
}

}  // namespace

int main() {
    // --- keep-alive, headers, bodies ------------------------------------------------------------
    section("keep-alive: one connection, reuse flagged, headers shaped like http.client's");
    {
        std::vector<Req> got;
        std::mutex mu;
        Server s([&](Conn& c, int idx) {
            for (;;) {
                Req r;
                if (!read_req(c, r, idx)) return;
                { std::lock_guard<std::mutex> lk(mu); got.push_back(r); }
                reply(c, r.head.rfind("GET /429", 0) == 0 ? 429 : 200, "{\"ok\":" + std::to_string(got.size()) + "}");
            }
        });
        HttpClient h("http://127.0.0.1:" + std::to_string(s.port()));
        auto a = h.request("GET", "/trade-api/v2/markets?limit=1000&cursor=abc",
                           {{"Content-Type", "application/json"}});
        auto b = h.request("POST", "/trade-api/v2/portfolio/events/orders/batched",
                           {{"KALSHI-ACCESS-KEY", "K"}, {"Content-Type", "application/json"}},
                           "{\"orders\": [1, 2]}");
        auto c = h.request("DELETE", "/trade-api/v2/portfolio/events/orders/batched",
                           {{"Content-Type", "application/json"}}, "{\"orders\": [{\"order_id\": \"x\"}]}");
        auto d = h.request("DELETE", "/trade-api/v2/portfolio/events/orders/abc",
                           {{"Content-Type", "application/json"}});
        auto e = h.request("GET", "/429");
        check("statuses", a.status == 200 && b.status == 200 && c.status == 200 && d.status == 200 && e.status == 429);
        check("a non-2xx comes back as a response, not a throw", e.status == 429 && e.body == "{\"ok\":5}");
        check("first request on a new connection: reused=false", !a.reused);
        check("  the next four: reused=true", b.reused && c.reused && d.reused && e.reused);
        check("  one TCP connection in all", h.connects() == 1 && s.accepted() == 1, std::to_string(h.connects()));
        bool lower = false;
        for (const auto& [k, v] : e.headers) if (k == "x-mixed-case" && v == "Yes") lower = true;
        check("response header names are lower-cased, values kept", lower);
        s.stop();
        std::lock_guard<std::mutex> lk(mu);
        check("five requests reached the server", got.size() == 5);
        if (got.size() == 5) {
            check("GET target verbatim", got[0].head.rfind("GET /trade-api/v2/markets?limit=1000&cursor=abc HTTP/1.1\r\n", 0) == 0);
            int n = 0;
            header(got[0].head, "Accept", &n);
            check("no curl 'Accept: */*'", n == 0);
            header(got[1].head, "Expect", &n);
            check("no 'Expect: 100-continue'", n == 0);
            check("Accept-Encoding: identity (as http.client)", header(got[0].head, "Accept-Encoding") == "identity");
            header(got[0].head, "Content-Length", &n);
            check("a GET carries no Content-Length", n == 0);
            check("POST body byte-exact", got[1].body == "{\"orders\": [1, 2]}");
            check("  with its Content-Length", header(got[1].head, "Content-Length") == "18");
            check("  caller headers kept", header(got[1].head, "KALSHI-ACCESS-KEY") == "K");
            header(got[1].head, "Content-Type", &n);
            check("  exactly one Content-Type, the caller's", n == 1 && header(got[1].head, "Content-Type") == "application/json");
            check("DELETE with a body sends it", got[2].head.rfind("DELETE ", 0) == 0 &&
                                                got[2].body == "{\"orders\": [{\"order_id\": \"x\"}]}");
            header(got[3].head, "Content-Length", &n);
            check("DELETE without a body sends no Content-Length", got[3].head.rfind("DELETE /trade-api/v2/portfolio/events/orders/abc ", 0) == 0 && n == 0);
        }
    }

    section("drop() closes the connection; the next request opens one");
    {
        Server s([&](Conn& c, int idx) {
            for (;;) { Req r; if (!read_req(c, r, idx)) return; reply(c, 200, "{}"); }
        });
        HttpClient h("http://127.0.0.1:" + std::to_string(s.port()));
        h.request("GET", "/a");
        h.drop();
        auto r = h.request("GET", "/b");
        check("new connection after drop", !r.reused && h.connects() == 2 && s.accepted() == 2);
    }

    section("server closes an IDLE keep-alive: the next request just reconnects");
    {
        Server s([&](Conn& c, int idx) {
            Req r;
            if (!read_req(c, r, idx)) return;
            reply(c, 200, "{}");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            c.close();                                    // idle close, after answering
        });
        HttpClient h("http://127.0.0.1:" + std::to_string(s.port()));
        h.request("GET", "/a");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto r = h.request("POST", "/b", {{"Content-Type", "application/json"}}, "{}");
        check("the write succeeds on a fresh connection", r.status == 200 && !r.reused && s.accepted() == 2);
    }

    // --- the double-send guard ------------------------------------------------------------------
    for (const std::string method : {"POST", "DELETE", "GET"}) {
        section(method + " that dies on a REUSED connection: HttpError(reused), sent ONCE");
        std::atomic<int> copies{0};
        Server s([&](Conn& c, int idx) {
            for (int n = 0;; ++n) {
                Req r;
                if (!read_req(c, r, idx, 3000)) return;
                if (r.head.rfind(method + " /order", 0) == 0) {
                    ++copies;
                    c.drop();                             // read it, answer nothing, vanish
                    return;
                }
                reply(c, 200, "{}");
            }
        });
        HttpClient h("http://127.0.0.1:" + std::to_string(s.port()));
        h.request("GET", "/warm");                        // open the keep-alive connection
        bool threw = false, reused = false;
        try {
            h.request(method, "/order", {{"Content-Type", "application/json"}},
                      method == "GET" ? "" : "{\"orders\": []}", 3.0);
        } catch (const HttpError& e) { threw = true; reused = e.reused; }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check("HttpError raised", threw);
        check("  flagged reused=true (a dropped keep-alive)", reused);
        check("  the server saw exactly ONE copy", copies.load() == 1, std::to_string(copies.load()));
        // libcurl may OPEN the fresh connection before the veto fires; what must never happen is a
        // request on it, which `copies` counts across every connection the server accepted.
        auto r = h.request("GET", "/after");
        check("  the client recovers on a fresh connection", r.status == 200 && !r.reused);
    }

    section("a request that dies on a FRESH connection is reused=false");
    {
        Server s([&](Conn& c, int idx) { Req r; read_req(c, r, idx); c.drop(); });
        HttpClient h("http://127.0.0.1:" + std::to_string(s.port()));
        bool threw = false, reused = true;
        try { h.request("POST", "/order", {{"Content-Type", "application/json"}}, "{}"); }
        catch (const HttpError& e) { threw = true; reused = e.reused; }
        check("HttpError, reused=false", threw && !reused);
        check("  sent once", s.accepted() == 1);
    }

    section("connection refused is reused=false; a silent server times out");
    {
        int port;
        { Server s([](Conn&, int) {}); port = s.port(); }
        HttpClient h("http://127.0.0.1:" + std::to_string(port));
        bool threw = false, reused = true;
        try { h.request("GET", "/x", {}, "", 2.0); } catch (const HttpError& e) { threw = true; reused = e.reused; }
        check("refused -> HttpError(reused=false)", threw && !reused);

        Server s2([&](Conn& c, int idx) { Req r; read_req(c, r, idx); std::this_thread::sleep_for(std::chrono::milliseconds(1500)); });
        HttpClient h2("http://127.0.0.1:" + std::to_string(s2.port()));
        const auto t0 = std::chrono::steady_clock::now();
        threw = false;
        try { h2.request("GET", "/x", {}, "", 0.5); } catch (const HttpError&) { threw = true; }
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check("a silent server -> HttpError at the timeout", threw && el < 1.4, std::to_string(el));
    }
    return report("test_http");
}

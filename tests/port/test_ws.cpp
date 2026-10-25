// net::WebSocket against a local TLS server with a certificate minted at runtime: the handshake
// bytes (and the Origin header that must never be there), every payload-length form in both
// directions, masking, fragmentation with a ping interleaved mid-message, pong echo, the close
// handshake in both directions, a frame split across a recv timeout, concurrent senders, and
// certificate/hostname verification failing closed.
#include <chrono>
#include <cstdlib>
#include <set>
#include <thread>

#include <openssl/sha.h>

#include "harness.hpp"
#include "kp/net/ws.hpp"
#include "net_fixture.hpp"

using namespace kp::test;
using namespace kp::test::net;
using kp::net::WebSocket;
using kp::net::WsClosed;
using kp::net::WsError;

namespace {

std::string accept_for(const std::string& key) {
    const std::string g = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)g.data(), g.size(), sha);
    std::string out(64, '\0');
    out.resize((std::size_t)EVP_EncodeBlock((unsigned char*)out.data(), sha, sizeof sha));
    return out;
}

// Complete the server half of the upgrade. Returns the request head.
std::string upgrade(Conn& c, const std::string& status = "101 Switching Protocols",
                    bool good_accept = true) {
    std::string head;
    if (!c.read_head(head)) return "";
    const std::string key = header(head, "Sec-WebSocket-Key");
    std::string resp = "HTTP/1.1 " + status + "\r\n";
    if (status.rfind("101", 0) == 0) {
        resp += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
        resp += "Sec-WebSocket-Accept: " + (good_accept ? accept_for(key) : std::string("bm9wZQ==")) + "\r\n";
    } else {
        resp += "Content-Length: 0\r\n";
    }
    resp += "\r\n";
    c.write(resp);
    return head;
}

std::string pattern(std::size_t n, int seed) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = (char)('a' + (int)((i * 7 + (std::size_t)seed) % 26));
    return s;
}

}  // namespace

int main() {
    const char* td = std::getenv("TMPDIR");
    const std::string tmp = td ? td : "/tmp";
    SelfSigned tls(tmp);

    // --- 1. handshake ---------------------------------------------------------------------------
    section("handshake: the request line and headers, and NO Origin");
    {
        std::string seen;
        std::mutex mu;
        Server s([&](Conn& c, int) {
            const std::string h = upgrade(c);
            { std::lock_guard<std::mutex> lk(mu); seen = h; }
            c.write(ws_frame(0x1, "hello"));
            ClientFrame f;
            read_client_frame(c, f, 2000);                 // the close
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/trade-api/ws/v2?x=1",
                   {{"KALSHI-ACCESS-KEY", "k"}, {"KALSHI-ACCESS-TIMESTAMP", "123"}});
        check("connected", ws.connected());
        auto m = ws.recv(2.0);
        check("first message", m && *m == "hello");
        ws.close();
        s.stop();
        std::lock_guard<std::mutex> lk(mu);
        check("request line carries path AND query", seen.rfind("GET /trade-api/ws/v2?x=1 HTTP/1.1\r\n", 0) == 0,
              seen.substr(0, 60));
        check("Host carries the non-default port", header(seen, "Host") == "localhost:" + std::to_string(s.port()));
        check("Upgrade: websocket", header(seen, "Upgrade") == "websocket");
        check("Connection: Upgrade", header(seen, "Connection") == "Upgrade");
        check("Sec-WebSocket-Version: 13", header(seen, "Sec-WebSocket-Version") == "13");
        check("a 16-byte base64 key", header(seen, "Sec-WebSocket-Key").size() == 24);
        check("caller headers are sent", header(seen, "KALSHI-ACCESS-KEY") == "k" &&
                                         header(seen, "KALSHI-ACCESS-TIMESTAMP") == "123");
        int n_origin = 0;
        header(seen, "Origin", &n_origin);
        check("NO Origin header (Kalshi 403s one)", n_origin == 0);
    }

    section("handshake failures fail closed");
    {
        Server s([&](Conn& c, int) { upgrade(c, "403 Forbidden"); }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        std::string why;
        try { ws.connect("wss://localhost:" + std::to_string(s.port()) + "/"); } catch (const WsError& e) { why = e.what(); }
        check("a refused upgrade throws 'handshake 403'", why == "handshake 403", why);
        check("  and is not connected", !ws.connected());
    }
    {
        Server s([&](Conn& c, int) { upgrade(c, "101 Switching Protocols", false); }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        std::string why;
        try { ws.connect("wss://localhost:" + std::to_string(s.port()) + "/"); } catch (const WsError& e) { why = e.what(); }
        check("a wrong Sec-WebSocket-Accept is refused", why.find("invalid upgrade") != std::string::npos, why);
    }
    {
        WebSocket ws;
        std::string why;
        try { ws.connect("wss://localhost:1/", {{"Origin", "https://x"}}); } catch (const WsError& e) { why = e.what(); }
        check("a caller-supplied Origin is refused before any socket", why.find("Origin") != std::string::npos, why);
    }

    section("TLS verification fails closed");
    {
        Server s([&](Conn& c, int) { upgrade(c); }, &tls);
        {
            WebSocket ws;                       // system store: our self-signed cert is unknown
            std::string why;
            try { ws.connect("wss://localhost:" + std::to_string(s.port()) + "/"); } catch (const WsError& e) { why = e.what(); }
            check("an untrusted certificate is refused", why.find("certificate verify failed") != std::string::npos, why);
        }
        {
            WebSocket ws;                       // trusted cert, but it names localhost, not the IP
            ws.set_ca_file(tls.ca_path);
            std::string why;
            try { ws.connect("wss://127.0.0.1:" + std::to_string(s.port()) + "/"); } catch (const WsError& e) { why = e.what(); }
            check("a hostname mismatch is refused", why.find("verify failed") != std::string::npos, why);
        }
    }

    // --- 2. server -> client framing ------------------------------------------------------------
    section("every length form, server -> client");
    {
        const std::vector<std::size_t> sizes = {0, 1, 125, 126, 127, 65535, 65536, 70000, 1u << 20};
        Server s([&](Conn& c, int) {
            upgrade(c);
            for (std::size_t i = 0; i < sizes.size(); ++i) c.write(ws_frame(0x1, pattern(sizes[i], (int)i)));
            c.write(ws_frame(0x2, std::string("\x00\xff\x01", 3)));
            ClientFrame f;
            read_client_frame(c, f, 3000);
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            auto m = ws.recv(5.0);
            check("len " + std::to_string(sizes[i]), m && *m == pattern(sizes[i], (int)i),
                  m ? std::to_string(m->size()) : "none");
        }
        auto b = ws.recv(2.0);
        check("a binary message is returned too", b && *b == std::string("\x00\xff\x01", 3));
        ws.close();
    }

    section("fragmentation, with a ping INSIDE the message; the pong echoes its payload");
    {
        ClientFrame pong;
        std::atomic<bool> got{false};
        Server s([&](Conn& c, int) {
            upgrade(c);
            c.write(ws_frame(0x1, "Hel", false));
            c.write(ws_frame(0x9, "p1"));
            c.write(ws_frame(0x0, "lo", false));
            c.write(ws_frame(0xA, "unsolicited"));        // a pong nobody asked for is ignored
            c.write(ws_frame(0x0, " world", true));
            c.write(ws_frame(0x1, "next"));
            if (read_client_frame(c, pong, 3000)) got = true;
            ClientFrame f;
            read_client_frame(c, f, 3000);
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        auto m = ws.recv(3.0);
        check("reassembled across continuations", m && *m == "Hello world", m ? *m : "none");
        auto m2 = ws.recv(3.0);
        check("  and the next message is intact", m2 && *m2 == "next");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ws.close();
        s.stop();
        check("a pong was sent", got.load());
        check("  opcode 0xA", pong.opcode == 0xA);
        check("  carrying the ping's payload", pong.payload == "p1", pong.payload);
        check("  masked, as every client frame must be", pong.masked && pong.mask_key.size() == 4);
    }

    // --- 3. client -> server framing ------------------------------------------------------------
    section("every length form, client -> server: minimal encoding, masked, fresh key");
    {
        const std::vector<std::size_t> sizes = {0, 1, 125, 126, 65535, 65536, 100000};
        std::vector<ClientFrame> got;
        std::mutex mu;
        Server s([&](Conn& c, int) {
            upgrade(c);
            for (std::size_t i = 0; i < sizes.size(); ++i) {
                ClientFrame f;
                if (!read_client_frame(c, f)) return;
                std::lock_guard<std::mutex> lk(mu);
                got.push_back(f);
            }
            ClientFrame f;
            read_client_frame(c, f, 3000);
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        for (std::size_t i = 0; i < sizes.size(); ++i) ws.send_text(pattern(sizes[i], (int)i + 3));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ws.close();
        s.stop();
        std::lock_guard<std::mutex> lk(mu);
        check("all frames arrived", got.size() == sizes.size());
        std::set<std::string> keys;
        for (std::size_t i = 0; i < got.size(); ++i) {
            const auto& f = got[i];
            const int want_form = sizes[i] < 126 ? 7 : (sizes[i] <= 0xFFFF ? 16 : 64);
            check("send len " + std::to_string(sizes[i]),
                  f.fin && f.opcode == 0x1 && f.masked && f.len_form == want_form &&
                  f.payload == pattern(sizes[i], (int)i + 3));
            keys.insert(f.mask_key);
        }
        check("mask keys are not reused", keys.size() == got.size());
    }

    section("concurrent send_text from four threads while a reader is in recv()");
    {
        std::vector<std::string> got;
        std::mutex mu;
        Server s([&](Conn& c, int) {
            upgrade(c);
            for (int i = 0; i < 800; ++i) {
                ClientFrame f;
                if (!read_client_frame(c, f)) return;
                std::lock_guard<std::mutex> lk(mu);
                got.push_back(f.payload);
            }
            c.write(ws_frame(0x1, "done"));
            ClientFrame f;
            read_client_frame(c, f, 3000);
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        std::optional<std::string> done;
        std::thread reader([&] {
            for (int i = 0; i < 100 && !done; ++i) done = ws.recv(0.1);
        });
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&, t] {
                for (int i = 0; i < 200; ++i)
                    ws.send_text("t" + std::to_string(t) + ":" + std::to_string(i) + ":" + pattern(300 + (std::size_t)i, t));
            });
        for (auto& t : ts) t.join();
        reader.join();
        ws.close();
        s.stop();
        std::set<std::string> uniq(got.begin(), got.end());
        bool intact = got.size() == 800 && uniq.size() == 800;
        for (const auto& g : got) {
            const auto a = g.find(':'), b = g.find(':', a + 1);
            if (a == std::string::npos || b == std::string::npos) { intact = false; break; }
            const int t = std::stoi(g.substr(1, a - 1)), i = std::stoi(g.substr(a + 1, b - a - 1));
            if (g.substr(b + 1) != pattern(300 + (std::size_t)i, t)) { intact = false; break; }
        }
        check("800 frames, none interleaved or corrupted", intact, std::to_string(got.size()));
        check("  and the reader got the reply meanwhile", done && *done == "done");
    }

    // --- 4. timeouts ----------------------------------------------------------------------------
    section("a frame split across a recv timeout is kept, not lost");
    {
        Server s([&](Conn& c, int) {
            upgrade(c);
            const std::string f = ws_frame(0x1, pattern(1000, 9));
            c.write(f.substr(0, 3));
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            c.write(f.substr(3));
            ClientFrame cf;
            read_client_frame(c, cf, 3000);
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        const auto t0 = std::chrono::steady_clock::now();
        auto a = ws.recv(0.15);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check("recv times out with nullopt", !a.has_value());
        check("  after roughly the timeout", el >= 0.14 && el < 0.4, std::to_string(el));
        auto b = ws.recv(2.0);
        check("  and the completed frame arrives whole", b && *b == pattern(1000, 9));
        ws.close();
    }

    // --- 5. close -------------------------------------------------------------------------------
    section("server-initiated close: WsClosed with its code, and the close is echoed");
    {
        ClientFrame echo;
        std::atomic<bool> got{false};
        Server s([&](Conn& c, int) {
            upgrade(c);
            c.write(ws_frame(0x1, "last"));
            c.write(ws_frame(0x8, std::string("\x03\xe9", 2) + "going away"));
            if (read_client_frame(c, echo, 3000)) got = true;
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        auto m = ws.recv(2.0);
        check("data before the close still arrives", m && *m == "last");
        int code = -1;
        try { ws.recv(2.0); } catch (const WsClosed& e) { code = e.code; }
        check("WsClosed carries 1001", code == 1001, std::to_string(code));
        check("  not connected afterwards", !ws.connected());
        s.stop();
        check("  the close was echoed", got && echo.opcode == 0x8 && echo.masked &&
                                       echo.payload == std::string("\x03\xe9", 2));
        bool threw = false;
        try { ws.recv(0.1); } catch (const WsClosed&) { threw = true; }
        check("  a further recv throws WsClosed", threw);
    }

    section("client-initiated close sends 1000");
    {
        ClientFrame f;
        std::atomic<bool> got{false};
        Server s([&](Conn& c, int) {
            upgrade(c);
            if (read_client_frame(c, f, 3000)) got = true;
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        ws.close();
        s.stop();
        check("close frame 0x8 with code 1000", got && f.opcode == 0x8 && f.payload == std::string("\x03\xe8", 2));
        bool threw = false;
        try { ws.send_text("x"); } catch (const WsClosed&) { threw = true; }
        check("  send after close throws WsClosed", threw);
    }

    section("a dropped TCP connection surfaces as WsClosed(code 0)");
    {
        Server s([&](Conn& c, int) { upgrade(c); c.drop(); }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        int code = -1;
        try { for (int i = 0; i < 20; ++i) ws.recv(0.2); } catch (const WsClosed& e) { code = e.code; }
        check("WsClosed, code 0", code == 0, std::to_string(code));
    }

    section("protocol violations are errors, not silently accepted");
    {
        Server s([&](Conn& c, int) {
            upgrade(c);
            std::string bad = ws_frame(0x1, "x");
            bad[0] = (char)(bad[0] | 0x40);            // RSV1 with no extension negotiated
            c.write(bad);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }, &tls);
        WebSocket ws;
        ws.set_ca_file(tls.ca_path);
        ws.connect("wss://localhost:" + std::to_string(s.port()) + "/");
        bool threw = false;
        try { ws.recv(2.0); } catch (const WsError&) { threw = true; }
        check("RSV bits without an extension throw", threw);
    }

    section("plain ws:// works for local tools");
    {
        Server s([&](Conn& c, int) {
            upgrade(c);
            c.write(ws_frame(0x1, "plain"));
            ClientFrame f;
            read_client_frame(c, f, 2000);
        });
        WebSocket ws;
        ws.connect("ws://127.0.0.1:" + std::to_string(s.port()) + "/");
        auto m = ws.recv(2.0);
        check("message over plain TCP", m && *m == "plain");
        ws.close();
    }
    return report("test_ws");
}

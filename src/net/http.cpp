// HttpClient over one libcurl easy handle (keep-alive), plus urllib.parse.quote_plus/urlencode.
//
// THE ONE THING libcurl DOES THAT THE REFERENCE MUST NOT. When a request goes out on a REUSED
// connection and nothing at all comes back, libcurl silently opens a fresh connection and sends the
// request AGAIN -- for every method, POST and DELETE included (lib/transfer.c Curl_retry_request
// rewinds the upload and retries). That is precisely the double-send kalshi/client.py spends a
// paragraph refusing: "a POST is ambiguous -- it may have been received -- and repeating one could
// double-send an order." So the second attempt is vetoed in CURLOPT_PREREQFUNCTION, which libcurl
// calls once per attempt, right before the request bytes are written. The failure surfaces as an
// HttpError with reused=true, exactly the case http.client raises in the reference, and the RETRY
// DECISION stays where the reference makes it: kalshi::Client retries a GET, never a write.
//
// Headers are shaped like http.client's rather than curl's defaults: no `Accept: */*`, no
// `Expect: 100-continue`, `Accept-Encoding: identity`, and no form Content-Type on a POST (callers
// always pass Content-Type, as _auth_headers does).
#include "kp/net/http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace kp::net {

namespace {

std::once_flag g_curl_once;
void curl_global() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t on_body(char* p, size_t sz, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
}

struct HeaderSink {
    Headers* h;
};

size_t on_header(char* p, size_t sz, size_t n, void* ud) {
    auto* hs = static_cast<HeaderSink*>(ud);
    std::string line(p, sz * n);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("HTTP/", 0) == 0) {           // a new status line: an interim 1xx or a new try
        hs->h->clear();
        return sz * n;
    }
    const auto c = line.find(':');
    if (c == std::string::npos) return sz * n;
    std::string name = line.substr(0, c);
    for (auto& ch : name) ch = (char)std::tolower((unsigned char)ch);
    std::size_t v = c + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
    hs->h->emplace_back(std::move(name), line.substr(v));
    return sz * n;
}

}  // namespace

struct HttpClient::Impl {
    std::string base;
    CURL* h = nullptr;
    long connects = 0;
    int attempts = 0;           // PREREQ calls within the current perform

    static int on_prereq(void* ud, char*, char*, int, int) {
        auto* self = static_cast<Impl*>(ud);
        // The first attempt goes; any second one inside the same perform is libcurl's own
        // transparent retry of a request that died on a reused socket. Veto it.
        return ++self->attempts > 1 ? CURL_PREREQFUNC_ABORT : CURL_PREREQFUNC_OK;
    }

    void open() {
        curl_global();
        h = curl_easy_init();
        if (!h) throw HttpError("curl_easy_init failed", false);
    }
    void close() {
        if (h) curl_easy_cleanup(h);
        h = nullptr;
    }
};

HttpClient::HttpClient(std::string base_url) : impl_(new Impl) {
    impl_->base = std::move(base_url);
    while (!impl_->base.empty() && impl_->base.back() == '/') impl_->base.pop_back();
    impl_->open();
}

HttpClient::~HttpClient() {
    impl_->close();
    delete impl_;
}

void HttpClient::drop() {
    impl_->close();
    impl_->open();
}

long HttpClient::connects() const { return impl_->connects; }

HttpResponse HttpClient::request(const std::string& method, const std::string& target,
                                 const Headers& headers, const std::string& body,
                                 double timeout_s) {
    Impl& I = *impl_;
    if (!I.h) I.open();
    CURL* h = I.h;
    // Reset the per-request options but keep the connection cache (curl_easy_reset keeps it).
    curl_easy_reset(h);

    HttpResponse out;
    HeaderSink hs{&out.headers};
    const std::string url = I.base + target;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &hs);
    curl_easy_setopt(h, CURLOPT_PREREQFUNCTION, &Impl::on_prereq);
    curl_easy_setopt(h, CURLOPT_PREREQDATA, &I);
    // http.client's timeout is per socket operation; curl's is for the whole transfer. The whole
    // transfer is the stricter of the two, which is the safe direction for a quote loop.
    const long ms = timeout_s > 0 ? (long)(timeout_s * 1000.0) : 0L;
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, ms);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, ms);

    std::string m = method;
    for (auto& c : m) c = (char)std::toupper((unsigned char)c);
    const bool has_body = !body.empty() || m == "POST" || m == "PUT" || m == "PATCH";
    if (m == "GET") {
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    } else if (m == "HEAD") {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else {
        if (m != "POST") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, m.c_str());
        if (has_body) {
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
        }
    }

    curl_slist* hl = nullptr;
    hl = curl_slist_append(hl, "Accept:");                 // delete curl's default
    hl = curl_slist_append(hl, "Expect:");                 // never 100-continue
    hl = curl_slist_append(hl, "Accept-Encoding: identity");
    bool have_ct = false;
    for (const auto& [k, v] : headers) {
        std::string lk = k;
        for (auto& c : lk) c = (char)std::tolower((unsigned char)c);
        if (lk == "content-type") have_ct = true;
        const std::string line = k + ": " + v;
        hl = curl_slist_append(hl, line.c_str());
    }
    if (has_body && !have_ct) hl = curl_slist_append(hl, "Content-Type:");   // no form type
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);

    I.attempts = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(h);
    out.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    curl_slist_free_all(hl);

    long nconn = 0;
    curl_easy_getinfo(h, CURLINFO_NUM_CONNECTS, &nconn);
    I.connects += nconn;
    // A retry is only ever attempted on a reused connection, so a vetoed second attempt means the
    // FIRST went out on a reused socket even though curl then counted a new connect.
    out.reused = (nconn == 0) || I.attempts > 1;
    if (rc != CURLE_OK) {
        const bool reused = out.reused && I.attempts >= 1;
        std::string what = std::string(curl_easy_strerror(rc));
        if (I.attempts > 1) what += " (request died on a reused connection; not re-sent)";
        // A failed transfer can leave the handle's connection in any state; start clean.
        I.close();
        throw HttpError(what, reused);
    }
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    out.status = status;
    return out;
}

// --- urllib.parse -------------------------------------------------------------------------------

std::string quote_plus(const std::string& s) {
    // quote(s, safe=' ') then ' ' -> '+'; always-safe is letters, digits and "_.-~". The input is
    // the UTF-8 encoding of the Python str, which is what quote() encodes before escaping.
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum || c == '_' || c == '.' || c == '-' || c == '~') {
            out.push_back((char)c);
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string urlencode(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string out;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) out.push_back('&');
        out += quote_plus(params[i].first);
        out.push_back('=');
        out += quote_plus(params[i].second);
    }
    return out;
}

}  // namespace kp::net

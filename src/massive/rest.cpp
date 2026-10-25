#include "kp/massive/rest.hpp"

#include <chrono>
#include <fstream>
#include <thread>
#include <time.h>

#include "detail.hpp"

namespace kp::massive {

// --- Clock ------------------------------------------------------------------------------------

Clock Clock::system() {
    Clock c;
    c.monotonic = [] {
        // CPython's time.monotonic() on Linux: clock_gettime(CLOCK_MONOTONIC) as a double.
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    };
    c.time_ns = [] {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    };
    c.sleep = [](double s) {
        if (s > 0) std::this_thread::sleep_for(std::chrono::duration<double>(s));
    };
    return c;
}

// --- urlencode / env --------------------------------------------------------------------------

namespace {

std::string quote_plus(const std::string& s) {
    static const char* HEX = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
            || c == '.' || c == '-' || c == '~') {
            out += (char)c;
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        }
    }
    return out;
}

std::string strip_chars(const std::string& s, const char* chars) {
    const auto b = s.find_first_not_of(chars);
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(chars);
    return s.substr(b, e - b + 1);
}

const char* WS = " \t\n\r\v\f";

bool ok2xx(long status) { return status >= 200 && status < 300; }

std::string http_msg(const HttpResult& r, std::size_t n) {
    return "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, n);
}

std::string with_filtered(const std::string& base, const std::string& path, const Params& params,
                          const std::string* api_key) {
    std::vector<std::pair<std::string, std::string>> kv;
    for (const auto& [k, v] : params)
        if (v) kv.emplace_back(k, *v);
    if (api_key) kv.emplace_back("apiKey", *api_key);
    return base + path + "?" + urlencode(kv);
}

}  // namespace

std::string urlencode(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string out;
    for (const auto& [k, v] : params) {
        if (!out.empty()) out += '&';
        out += quote_plus(k);
        out += '=';
        out += quote_plus(v);
    }
    return out;
}

std::string load_api_key(const std::string& kalshi_root) {
    const std::string path = kalshi_root + "/.env.massive";
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line, key;
    bool found = false;
    while (std::getline(in, line)) {
        line = strip_chars(line, WS);
        if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) continue;
        const auto eq = line.find('=');
        const std::string k = strip_chars(line.substr(0, eq), WS);
        std::string v = strip_chars(line.substr(eq + 1), WS);
        v = strip_chars(v, "\"");
        v = strip_chars(v, "'");
        if (k == "MASSIVE_API_KEY") { key = v; found = true; }   // dict: the LAST one wins
    }
    if (!found) throw std::runtime_error("MASSIVE_API_KEY missing from " + path);
    return key;
}

// --- RateLimiter ------------------------------------------------------------------------------

RateLimiter::RateLimiter(double per_sec, const Clock* clock)
    : clock_(clock), interval_(1.0 / per_sec) {}

void RateLimiter::acquire() {
    double wait;
    {
        std::lock_guard<std::mutex> g(mu_);
        const double now = clock_->monotonic();
        const double d = next_ - now;
        wait = d > 0.0 ? d : 0.0;                    // max(0.0, d)
        next_ = (now >= next_ ? now : next_) + interval_;  // max(now, _next) + _interval
    }
    if (wait != 0.0) clock_->sleep(wait);
}

// --- Rest -------------------------------------------------------------------------------------

Rest::Rest(std::string api_key, HttpGet get, Clock clock, std::string base)
    : key_(std::move(api_key)), http_(std::move(get)), clock_(std::move(clock)),
      base_(std::move(base)), hist_lim_(RATE_PER_SEC, &clock_), agg_lim_(AGG_RATE_PER_SEC, &clock_),
      fut_lim_(FUTURES_RATE_PER_SEC, &clock_) {}

kp::json::Value Rest::rest(const std::string& path, const Params& params) const {
    const HttpResult r = http_(with_filtered(base_, path, params, &key_), 30.0);
    // urllib raises HTTPError on a non-2xx; client.rest maps it, body[:300].
    if (!ok2xx(r.status)) throw MassiveError(http_msg(r, 300));
    return kp::json::parse(r.body);
}

kp::json::Value Rest::rest_url(const std::string& url) const {
    const char* sep = url.find('?') != std::string::npos ? "&" : "?";
    const HttpResult r = http_(url + sep + "apiKey=" + key_, 30.0);
    // The reference lets urllib's HTTPError escape here UNMAPPED -- it is not a MassiveError, and
    // a caller catching MassiveError would not catch it. Neither is this.
    if (!ok2xx(r.status)) throw std::runtime_error(http_msg(r, 300));
    return kp::json::parse(r.body);
}

kp::json::Value Rest::get_url(const std::string& url, double timeout_s, RateLimiter* limiter) const {
    (limiter ? *limiter : hist_lim_).acquire();
    const char* sep = url.find('?') != std::string::npos ? "&" : "?";
    const std::string keyed = url + sep + "apiKey=" + key_;
    std::string last;
    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        const double backoff = 0.5 * (double)(1 << attempt);
        HttpResult r;
        try {
            r = http_(keyed, timeout_s);
        } catch (const std::exception& e) {
            // (URLError, TimeoutError, OSError): retried. repr(e)[:200] in the reference.
            last = detail::head(e.what(), 200);
            clock_.sleep(backoff);
            continue;
        }
        if (ok2xx(r.status)) return kp::json::parse(r.body);   // a bad body raises, unretried
        const std::string msg = http_msg(r, 200);
        if (r.status == 429 || r.status == 500 || r.status == 502 || r.status == 503
            || r.status == 504) {
            last = msg;
            clock_.sleep(backoff);
            continue;
        }
        throw MassiveHistoryError(msg);
    }
    throw MassiveHistoryError(last);
}

kp::json::Value Rest::get(const std::string& path, const Params& params) const {
    return get_url(with_filtered(base_, path, params, nullptr));
}

}  // namespace kp::massive

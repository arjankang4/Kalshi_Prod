#include "kp/kalshi/client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>

#include "kp/kalshi/env.hpp"
#include "kp/kalshi/signer.hpp"
#include "kp/storage/pyerr.hpp"

namespace kp::kalshi {

using kp::json::Value;
using kp::storage::PyError;
using kp::storage::py_float;
using kp::storage::py_slice;
using kp::storage::py_str;
using kp::storage::py_type_name;

// --- clock ------------------------------------------------------------------------------------

double ClientClock::monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ClientClock::sleep(double s) {
    if (s > 0) std::this_thread::sleep_for(std::chrono::duration<double>(s));
}
double ClientClock::time() {
    const std::int64_t t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
    if (t % 1000000000 == 0) return (double)(t / 1000000000);
    return (double)t / 1e9;
}
ClientClock& ClientClock::real() {
    static ClientClock c;
    return c;
}

// --- transport --------------------------------------------------------------------------------

namespace {

class CurlTransport : public Transport {
public:
    explicit CurlTransport(std::string base) : base_(std::move(base)), id_(next_id()) {}
    kp::net::HttpResponse request(const std::string& method, const std::string& target,
                                  const kp::net::Headers& headers, const std::string& body,
                                  double timeout_s) override {
        // _local: ONE connection per thread, never shared -- an HTTP/1.1 connection carries one
        // request at a time. Keyed by transport so two clients on one thread do not collide.
        thread_local std::unordered_map<std::uint64_t, std::unique_ptr<kp::net::HttpClient>> conns;
        auto& c = conns[id_];
        if (!c) c = std::make_unique<kp::net::HttpClient>(base_);
        return c->request(method, target, headers, body, timeout_s);
    }

private:
    static std::uint64_t next_id() {
        static std::atomic<std::uint64_t> n{1};
        return n++;
    }
    std::string base_;
    std::uint64_t id_;
};

bool truthy(const Value& v) {
    using T = Value::Type;
    switch (v.type()) {
        case T::Null: return false;
        case T::Bool: return v.boolean();
        case T::Number: return v.is_int() ? v.integer() != 0 : v.num() != 0.0;
        case T::String: return !v.str().empty();
        case T::Array: return !v.arr().empty();
        case T::Object: return !v.obj().empty();
    }
    return false;
}

// d.get(k) on a value that must be a dict (AttributeError otherwise, as CPython).
Value pyget(const Value& d, const std::string& k) {
    if (!d.is_object()) throw PyError("AttributeError", "'" + py_type_name(d) + "' object has no attribute 'get'");
    const Value* v = d.find(k);
    return v ? *v : Value();
}
// `x or {}`
Value or_dict(const Value& v) { return truthy(v) ? v : Value::make_object({}); }

std::string utf8_ignore(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        std::size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        bool ok = n > 0 && i + n <= s.size();
        for (std::size_t k = 1; ok && k < n; ++k) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (ok && n == 3) {
            const unsigned char c1 = (unsigned char)s[i + 1];
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;   // overlong / surrogate
        }
        if (ok && n == 4) {
            const unsigned char c1 = (unsigned char)s[i + 1];
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false;
        }
        if (ok) { out.append(s, i, n); i += n; }
        else ++i;
    }
    return out;
}

// urllib.parse.urlparse(api_base).path
std::string url_path(const std::string& url) {
    const auto s = url.find("://");
    const std::size_t from = s == std::string::npos ? 0 : s + 3;
    const auto p = url.find('/', from);
    if (p == std::string::npos) return "";
    std::string path = url.substr(p);
    const auto q = path.find_first_of("?#");
    if (q != std::string::npos) path.resize(q);
    return path;
}
std::string url_origin(const std::string& url) {
    const auto s = url.find("://");
    const std::size_t from = s == std::string::npos ? 0 : s + 3;
    const auto p = url.find('/', from);
    return p == std::string::npos ? url : url.substr(0, p);
}

bool is_write(const std::string& m) { return m == "POST" || m == "DELETE" || m == "PUT" || m == "PATCH"; }

// `{**d, k: v}` -- replace in place, else append.
void merge(Params& p, const std::string& k, Value v) {
    for (auto& kv : p) if (kv.first == k) { kv.second = std::move(v); return; }
    p.emplace_back(k, std::move(v));
}

}  // namespace

std::shared_ptr<Transport> curl_transport(const std::string& base) { return std::make_shared<CurlTransport>(base); }

std::string py_exc_text(const std::exception& e, std::size_t n) {
    std::string type = "RuntimeError";
    if (dynamic_cast<const KalshiError*>(&e)) type = "KalshiError";
    else if (dynamic_cast<const kp::net::HttpError*>(&e)) type = "HttpError";
    else if (const auto* pe = dynamic_cast<const PyError*>(&e)) type = pe->type;
    return type + ": " + py_slice(e.what(), n);
}

// --- Client -----------------------------------------------------------------------------------

Client::Config Client::from_env(const std::string& root) {
    const KalshiEnv e = kalshi_env(root);
    return Config{e.api_base, e.key_id, e.private_key_path};
}

Client::Client(Config cfg, std::shared_ptr<Transport> transport, ClientClock* clock, SignFn sign)
    : cfg_(std::move(cfg)) {
    while (!cfg_.api_base.empty() && cfg_.api_base.back() == '/') cfg_.api_base.pop_back();
    base_path_ = url_path(cfg_.api_base);
    transport_ = transport ? std::move(transport) : curl_transport(url_origin(cfg_.api_base));
    clock_ = clock ? clock : &ClientClock::real();
    if (sign) {
        sign_ = std::move(sign);
    } else {
        auto signer = std::make_shared<Signer>(cfg_.private_key_path);
        sign_ = [signer](const std::string& ts, const std::string& m, const std::string& p) {
            return signer->sign(ts, m, p);
        };
    }
}

double Client::read_rate() const {
    std::lock_guard<std::mutex> lk(rate_mu_);
    return rate_per_sec_;
}
void Client::set_read_rate(double r) {
    std::lock_guard<std::mutex> lk(rate_mu_);
    rate_per_sec_ = r;
}

void Client::throttle(const std::string& method) {
    if (is_write(method)) return;            // writes are charged, never delayed
    double wait;
    {
        std::lock_guard<std::mutex> lk(rate_mu_);
        const double now = clock_->monotonic();
        wait = std::max(0.0, next_slot_ - now);
        next_slot_ = std::max(now, next_slot_) + 1.0 / rate_per_sec_;
    }
    if (wait != 0.0) clock_->sleep(wait);
}

kp::net::Headers Client::auth_headers(const std::string& method, const std::string& full_path) {
    const std::string ts = std::to_string((long long)(clock_->time() * 1000));
    std::string m = method;
    for (auto& c : m) c = (char)std::toupper((unsigned char)c);
    return {{"KALSHI-ACCESS-KEY", cfg_.key_id},
            {"KALSHI-ACCESS-SIGNATURE", sign_(ts, m, full_path)},
            {"KALSHI-ACCESS-TIMESTAMP", ts},
            {"Content-Type", "application/json"}};
}

Value Client::request(const std::string& method_in, const std::string& path, const Params& params,
                      const Value* body, bool is_signed, double timeout, std::optional<double> tokens) {
    const std::string full_path = base_path_ + path;
    std::string target = full_path;
    if (!params.empty()) {                    // `if params:` -- then None values are dropped
        std::vector<std::pair<std::string, std::string>> kv;
        for (const auto& [k, v] : params) if (!v.is_null()) kv.emplace_back(k, py_str(v));
        target += "?" + kp::net::urlencode(kv);
    }
    const std::string data = body ? kp::json::dumps(*body) : std::string();
    std::string method = method_in;
    for (auto& c : method) c = (char)std::toupper((unsigned char)c);
    const bool write = is_write(method);
    const double cost = tokens ? *tokens : DEFAULT_TOKEN_COST;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        throttle(method);
        // Charged BEFORE the send: a request that reaches Kalshi has spent its tokens whether or
        // not we get to read the answer. Writes never retry, so this runs once for them.
        if (write && bucket_.debit) bucket_.debit(cost);
        const kp::net::Headers headers = is_signed ? auth_headers(method, full_path)
                                                   : kp::net::Headers{{"Content-Type", "application/json"}};
        kp::net::HttpResponse resp;
        try {
            resp = transport_->request(method, target, headers, data, timeout);
        } catch (const kp::net::HttpError& e) {
            // A failure on a socket we did not just open is a dropped idle keep-alive: the request
            // was never answered. Safe to repeat for a GET; a write may have been received.
            if (e.reused && method == "GET" && attempt < MAX_RETRIES - 1) continue;
            throw;
        }
        if (resp.status >= 200 && resp.status < 300) return kp::json::parse(resp.body);

        const std::string detail = utf8_ignore(resp.body);
        const bool retryable = !write && (resp.status == 429 || resp.status >= 500);
        if (write && resp.status == 429 && bucket_.penalise) bucket_.penalise(cost);
        if (!retryable || attempt == MAX_RETRIES - 1) throw KalshiError((int)resp.status, detail);
        clock_->sleep(0.4 * std::pow(2.0, attempt));
    }
    throw KalshiError(0, "unreachable");
}

Value Client::get(const std::string& path, const Params& params) { return request("GET", path, params); }

void Client::paged_iter(const std::string& path, const std::string& key, bool is_signed, const Params& params,
                        const std::function<void(const Value&)>& each) {
    Value cursor;
    for (;;) {
        Params p = params;
        merge(p, "cursor", cursor);
        const Value page = request("GET", path, p, nullptr, is_signed);
        const Value items = pyget(page, key);
        if (truthy(items)) {
            if (items.is_array()) for (const auto& it : items.arr()) each(it);
            else if (items.is_object()) for (const auto& m : items.obj()) each(Value::make_string(m.first));
            else throw PyError("TypeError", "'" + py_type_name(items) + "' object is not iterable");
        }
        cursor = pyget(page, "cursor");
        if (!truthy(cursor)) return;
    }
}

Value Client::paged(const std::string& path, const std::string& key, bool is_signed, const Params& params) {
    Value out = Value::make_array({});
    paged_iter(path, key, is_signed, params, [&](const Value& v) { out.push(v); });
    return out;
}

Value Client::markets(const Params& params) {
    Params p{{"limit", P(1000)}};
    for (const auto& [k, v] : params) merge(p, k, v);
    return paged("/markets", "markets", false, p);
}

Value Client::orderbooks(const std::vector<std::string>& tickers, std::optional<long> depth) {
    Value out = Value::make_object({});
    for (std::size_t i = 0; i < tickers.size(); i += MAX_ORDERBOOK_TICKERS) {
        Params p;
        for (std::size_t k = i; k < std::min(tickers.size(), i + MAX_ORDERBOOK_TICKERS); ++k)
            p.emplace_back("tickers", P(tickers[k]));
        if (depth) p.emplace_back("depth", P((long long)*depth));
        const Value resp = request("GET", "/markets/orderbooks", p);
        const Value books = pyget(resp, "orderbooks");
        if (!truthy(books)) continue;
        // {b["ticker"]: b for b in ...} then dict.update: a repeated ticker keeps its first slot.
        Value chunk = Value::make_object({});
        for (const auto& b : books.arr()) {
            if (!b.is_object()) throw PyError("TypeError", "'" + py_type_name(b) + "' object is not subscriptable");
            const Value* t = b.find("ticker");
            if (!t) throw PyError("KeyError", "'ticker'");
            chunk.set(py_str(*t), b);
        }
        for (const auto& [k, v] : chunk.obj()) out.set(k, v);
    }
    return out;
}

Value Client::balance() { return request("GET", "/portfolio/balance", {}, nullptr, true); }

Value Client::resting_orders(const Params& params) {
    Params p{{"status", P("resting")}, {"limit", P(1000)}};
    for (const auto& [k, v] : params) merge(p, k, v);
    Value out = Value::make_object({});
    out.set("orders", paged("/portfolio/orders", "orders", true, p));
    out.set("cursor", Value());
    return out;
}

Value Client::open_positions(bool strict) {
    Value out = Value::make_object({});
    try {
        Value cursor;
        for (;;) {
            Params p{{"limit", P(200)}, {"count_filter", P("position")}};
            if (truthy(cursor)) p.emplace_back("cursor", cursor);
            const Value resp = request("GET", "/portfolio/positions", p, nullptr, true);
            const Value rows = pyget(resp, "market_positions");
            if (truthy(rows)) {
                for (const auto& row : rows.arr()) {
                    // row.get("position_fp", row.get("position")) -- a PRESENT null is None.
                    const Value* fp = row.is_object() ? row.find("position_fp") : nullptr;
                    const Value n = fp ? *fp : pyget(row, "position");
                    if (n.is_null()) continue;
                    const double x = py_float(n);
                    if (x != 0.0) {
                        const Value* t = row.find("ticker");
                        if (!t) throw PyError("KeyError", "'ticker'");
                        out.set(py_str(*t), Value::make_number(x));
                    }
                }
            }
            cursor = pyget(resp, "cursor");
            if (!truthy(cursor)) return out;
        }
    } catch (const std::exception&) {
        if (strict) throw;                    // the loss limit: "could not read" is not "flat"
        return out;                           // never raises: say so and start conservatively
    }
}

Value Client::cancel_order(const std::string& order_id) {
    return request("DELETE", "/portfolio/events/orders/" + order_id, {}, nullptr, true, 20, CANCEL_TOKEN_COST);
}

Value Client::batch_create_orders(const std::vector<Value>& orders) {
    if (orders.empty()) return Value::make_array({});
    Value body = Value::make_object({});
    body.set("orders", Value::make_array(orders));
    const Value resp = request("POST", "/portfolio/events/orders/batched", {}, &body, true, 20,
                               ORDER_TOKEN_COST * (double)orders.size());
    const Value r = pyget(resp, "orders");
    return truthy(r) ? r : Value::make_array({});
}

Value Client::batch_cancel_orders(const std::vector<std::string>& order_ids) {
    if (order_ids.empty()) return Value::make_array({});
    Value arr = Value::make_array({});
    for (const auto& id : order_ids) {
        Value o = Value::make_object({});
        o.set("order_id", Value::make_string(id));
        arr.push(o);
    }
    Value body = Value::make_object({});
    body.set("orders", arr);
    const Value resp = request("DELETE", "/portfolio/events/orders/batched", {}, &body, true, 20,
                               CANCEL_TOKEN_COST * (double)order_ids.size());
    const Value r = pyget(resp, "orders");
    return truthy(r) ? r : Value::make_array({});
}

Value Client::fills(const Params& params) { return paged("/portfolio/fills", "fills", true, params); }

Value Client::endpoint_costs() {
    try {
        return request("GET", "/account/endpoint_costs", {}, nullptr, true);
    } catch (const std::exception& e) {
        Value out = Value::make_object({});
        out.set("error", Value::make_string(py_exc_text(e)));
        return out;
    }
}

Value Client::write_budget() {
    try {
        const Value lim = request("GET", "/account/limits", {}, nullptr, true);
        const Value w = or_dict(pyget(lim, "write"));
        const Value capv = pyget(w, "bucket_capacity");
        const double cap = py_float(truthy(capv) ? capv : Value::make_int(0));
        const Value refv = pyget(w, "refill_rate");
        const double refill = py_float(truthy(refv) ? refv : Value::make_int(0));
        Value out = Value::make_object({});
        out.set("usage_tier", pyget(lim, "usage_tier"));
        out.set("bucket_capacity", Value::make_number(cap));
        out.set("refill_rate", Value::make_number(refill));
        out.set("write_req_per_s", refill != 0.0 ? Value::make_number(refill / ORDER_TOKEN_COST) : Value());
        out.set("max_batch", Value::make_int(max_batch(cap, refill)));
        return out;
    } catch (const std::exception& e) {
        Value out = Value::make_object({});
        out.set("error", Value::make_string(py_exc_text(e)));
        out.set("max_batch", Value::make_int(BATCH_FALLBACK));
        return out;
    }
}

namespace {
// sorted({o.get("usage_tier") for o in obs if o.get("usage_tier")}) -- the tiers are strings.
Value tiers_seen(const Value& obs) {
    std::set<std::string> s;
    for (const auto& o : obs.arr()) {
        const Value* t = o.find("usage_tier");
        if (t && truthy(*t)) s.insert(py_str(*t));
    }
    Value out = Value::make_array({});
    for (const auto& t : s) out.push(Value::make_string(t));
    return out;
}
std::optional<double> opt_float(const Value& v) {
    if (v.is_null()) return std::nullopt;
    return py_float(v);
}
}  // namespace

Value Client::set_write_budget(std::optional<double> refill_rate, int samples) {
    Value obs = Value::make_array({});
    Value b = Value::make_object({});
    std::optional<double> r, cap;
    Value cap_v;                                   // what `cap` IS, for `b.get("bucket_capacity")`
    if (!refill_rate) {
        for (int i = 0; i < std::max(1, samples); ++i) {
            b = write_budget();
            Value o = Value::make_object({});
            o.set("usage_tier", pyget(b, "usage_tier"));
            o.set("refill_rate", pyget(b, "refill_rate"));
            o.set("bucket_capacity", pyget(b, "bucket_capacity"));
            o.set("error", pyget(b, "error"));
            obs.push(o);
        }
        std::vector<double> rates, caps;
        for (const auto& o : obs.arr()) {
            if (truthy(*o.find("refill_rate"))) rates.push_back(py_float(*o.find("refill_rate")));
            if (truthy(*o.find("bucket_capacity"))) caps.push_back(py_float(*o.find("bucket_capacity")));
        }
        if (!rates.empty()) r = *std::min_element(rates.begin(), rates.end());
        if (!caps.empty()) cap = *std::min_element(caps.begin(), caps.end());
        if (cap && *cap != 0.0) {
            b.set("bucket_capacity", Value::make_number(*cap));
            b.set("refill_rate", r ? Value::make_number(*r) : Value());
            b.set("max_batch", Value::make_int(max_batch(cap, r)));
        }
    } else {
        b = write_budget();
        r = *refill_rate;
        cap = opt_float(pyget(b, "bucket_capacity"));
    }
    // (`_WRITE_TOKENS_PER_SEC` / `_WRITE_CAPACITY` are assigned in the reference and read by
    // nothing, so they are not carried.)
    if (bucket_.resize)
        bucket_.resize((cap && *cap != 0.0) ? cap : std::nullopt, (r && *r != 0.0) ? r : std::nullopt);
    Value out = b;
    out.set("observations", obs);
    out.set("tiers_seen", tiers_seen(obs));
    out.set("bucket", bucket_.detail ? bucket_.detail() : Value());
    return out;
}

Value Client::set_read_budget(std::optional<double> refill_rate, double cap, int samples, double share) {
    Value obs = Value::make_array({});
    std::optional<double> r;
    if (!refill_rate) {
        for (int i = 0; i < std::max(1, samples); ++i) {
            try {
                const Value lim = request("GET", "/account/limits", {}, nullptr, true);
                Value o = Value::make_object({});
                const Value tier = pyget(lim, "usage_tier");
                const Value rr = pyget(or_dict(pyget(lim, "read")), "refill_rate");
                const double x = py_float(truthy(rr) ? rr : Value::make_int(0));
                o.set("usage_tier", tier);
                o.set("read_refill_rate", Value::make_number(x));
                obs.push(o);
            } catch (const std::exception& e) {
                Value o = Value::make_object({});
                o.set("error", Value::make_string(py_exc_text(e)));
                obs.push(o);
            }
        }
        std::vector<double> rates;
        for (const auto& o : obs.arr()) {
            const Value* x = o.find("read_refill_rate");
            if (x && truthy(*x)) rates.push_back(x->num());
        }
        if (!rates.empty()) r = *std::min_element(rates.begin(), rates.end());
    } else {
        r = *refill_rate * ORDER_TOKEN_COST;
    }
    const double before = read_rate();
    if (r && *r != 0.0) {
        const double x = *r / DEFAULT_TOKEN_COST * 0.9 * share;
        const double want = x < cap ? x : cap;                    // min(cap, x)
        set_read_rate(share < 1.0 ? want : (want > before ? want : before));
    }
    Value out = Value::make_object({});
    out.set("observations", obs);
    out.set("min_read_refill_rate", r ? Value::make_number(*r) : Value());
    out.set("req_per_s_before", Value::make_number(before));
    out.set("req_per_s_after", Value::make_number(read_rate()));
    out.set("cap", Value::make_number(cap));
    out.set("share", Value::make_number(share));
    out.set("tiers_seen", tiers_seen(obs));
    return out;
}

}  // namespace kp::kalshi

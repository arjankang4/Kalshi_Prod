#include "kp/massive/streams.hpp"

#include <cmath>
#include <cstdio>

#include "detail.hpp"
#include "kp/massive/index_rest.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/time/et.hpp"

namespace kp::massive::streams {

using kp::json::Value;

namespace {

class MassiveStreamError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

const char* exc_name(const std::exception& e) {
    if (dynamic_cast<const MassiveStreamError*>(&e)) return "MassiveStreamError";
    if (dynamic_cast<const WsTimeout*>(&e)) return "WebSocketTimeoutException";
    if (dynamic_cast<const detail::PyTypeError*>(&e)) return "TypeError";
    if (dynamic_cast<const std::invalid_argument*>(&e)) return "ValueError";
    if (dynamic_cast<const std::out_of_range*>(&e)) return "KeyError";
    return "WebSocketException";
}

std::string err(const char* where, const std::exception& e, std::size_t n) {
    return std::string(where) + ": " + exc_name(e) + ": " + detail::head(e.what(), n);
}

std::string action_frame(const char* action, const std::string& params) {
    Value v;
    v.set("action", Value::make_string(action));
    v.set("params", Value::make_string(params));
    return kp::json::dumps(v);
}

// `int(m["t"]) * 1_000_000 if m.get("t") else recv_ns`
long long ts_of(const Value& m, long long recv_ns) {
    const Value* t = m.find("t");
    if (!t || !detail::truthy(*t)) return recv_ns;
    return detail::py_int(*t) * 1000000LL;
}

std::string today_et() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const auto p = kp::time::to_et(kp::time::Instant{(std::int64_t)ts.tv_sec * 1000000000LL});
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", p.year, p.month, p.day);
    return buf;
}

}  // namespace

// --- Stream -----------------------------------------------------------------------------------

Stream::Stream(std::string url, std::vector<std::string> subs, std::string name, Tee tee,
               std::string api_key, WsFactory ws, Clock clock, bool threaded)
    : clock_(std::move(clock)), url_(std::move(url)), subs_(std::move(subs)), name_(std::move(name)),
      tee_(std::move(tee)), key_(std::move(api_key)), ws_factory_(std::move(ws)), threaded_(threaded) {}

Stream::~Stream() {
    // Derived destructors call stop() first, so the pump never runs a half-destroyed on_message.
    stop();
}

void Stream::connect() {
    auto ws = ws_factory_();
    ws->connect(url_, 15.0);
    auto recv = [&]() {
        auto r = ws->recv(15.0);
        if (!r) throw WsTimeout("timed out");
        return *r;
    };
    recv();                                            // {"ev":"status","status":"connected"}
    {
        Value a;
        a.set("action", Value::make_string("auth"));
        a.set("params", Value::make_string(key_));
        ws->send_text(kp::json::dumps(a));
    }
    const std::string auth = recv();
    if (auth.find("auth_success") == std::string::npos) {
        try { ws->close(); } catch (...) {}
        throw MassiveStreamError(name_ + " auth failed: " + detail::head(auth, 160));
    }
    for (const auto& s : subs_) ws->send_text(action_frame("subscribe", s));
    ws_ = std::move(ws);
    have_ws_ = true;
    std::lock_guard<std::mutex> g(mu_);
    connected_at_ = clock_.monotonic();
}

void Stream::close_ws() {
    if (ws_) {
        try { ws_->close(); } catch (...) {}
    }
    ws_.reset();
    have_ws_ = false;
}

Stream& Stream::start(double /*timeout*/) {
    try {
        connect();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> g(mu_);
        last_error_ = err("connect", e, 120);
        ++errors_;
        return *this;
    }
    stop_ = false;
    running_ = true;
    if (threaded_) thread_ = std::thread([this] { run_pump(); });
    return *this;
}

Stream& Stream::stop() {
    {
        std::lock_guard<std::mutex> g(stop_mu_);
        stop_ = true;
    }
    stop_cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    running_ = false;
    close_ws();
    return *this;
}

void Stream::wait_stop(double s) {
    if (!threaded_) { clock_.sleep(s); return; }
    std::unique_lock<std::mutex> lk(stop_mu_);
    stop_cv_.wait_for(lk, std::chrono::duration<double>(s), [this] { return stop_.load(); });
}

bool Stream::ok() const { return running_ && have_ws_; }

std::optional<std::string> Stream::last_error() const {
    std::lock_guard<std::mutex> g(mu_);
    return last_error_;
}

StreamHealth Stream::health(std::optional<double> now) const {
    // `now = now or time.monotonic()`: 0.0 reads the clock too.
    const double t = (now && *now != 0.0) ? *now : clock_.monotonic();
    StreamHealth h;
    h.name = name_;
    h.ok = ok();
    std::lock_guard<std::mutex> g(mu_);
    h.frames = frames_;
    h.updates = updates_;
    h.reconnects = reconnects_;
    h.errors = errors_;
    h.last_error = last_error_;
    if (last_frame_t_) h.silent_s = kp::pycompat::py_round_nd(t - *last_frame_t_, 3);
    return h;
}

bool Stream::run_pump() {
    running_ = true;
    double backoff = RECONNECT_BACKOFF_S;
    while (!stop_) {
        if (!ws_) {
            // RECONNECT RESUBSCRIBES: a subscription lives on the connection.
            try {
                connect();
                std::lock_guard<std::mutex> g(mu_);
                ++reconnects_;
                backoff = RECONNECT_BACKOFF_S;
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> g(mu_);
                    ++errors_;
                    last_error_ = err("reconnect", e, 100);
                }
                wait_stop(backoff);
                backoff = std::min(MAX_BACKOFF_S, backoff * 2);
                continue;
            }
        }
        std::optional<std::string> raw;
        try {
            raw = ws_->recv(1.0);
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> g(mu_);
                ++errors_;
                last_error_ = err("recv", e, 100);
            }
            close_ws();
            continue;
        }
        if (!raw) continue;
        const long long recv_ns = clock_.time_ns();
        if (!ingest(*raw, recv_ns)) {   // the reference's thread dies
            running_ = false;
            return true;
        }
    }
    // Left on stop: stop() owns `running_` from here, so a PAUSED test pump still reads as the
    // reference's live FakeThread (ok() = thread alive and a socket up).
    return false;
}

bool Stream::ingest(const std::string& raw, long long recv_ns, std::optional<double> mono_now) {
    Value msgs;
    try {
        msgs = kp::json::parse(raw);
    } catch (const std::exception&) {
        return true;                                   // ValueError: continue
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        last_frame_t_ = mono_now ? *mono_now : clock_.monotonic();
    }
    const std::vector<Value> one = msgs.is_array() ? std::vector<Value>{} : std::vector<Value>{msgs};
    const std::vector<Value>& ms = msgs.is_array() ? msgs.arr() : one;
    for (const Value& m : ms) {
        if (!m.is_object()) return false;              // m.get on a non-dict: uncaught
        const Value* ev = m.find("ev");
        if (ev && ev->is_string() && ev->str() == "status") {
            const Value* st = m.find("status");
            const bool fine = st && st->is_string()
                && (st->str() == "success" || st->str() == "connected" || st->str() == "auth_success");
            if (!fine) {
                std::lock_guard<std::mutex> g(mu_);
                last_error_ = "status: " + detail::head(detail::py_repr(m), 120);
            }
            continue;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> g(mu_);
            ++frames_;
            try {
                changed = on_message(m, recv_ns);
                if (changed) ++updates_;
            } catch (const std::exception& e) {
                ++errors_;
                last_error_ = err("handle", e, 100);
            }
        }
        // ONLY WHEN SOMETHING CHANGED -- see the module note. Outside the lock: the tape is a
        // queue and must never hold up the feed's readers.
        if (changed && tee_) {
            try {
                tee_(recv_ns, m);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> g(mu_);
                ++errors_;
                last_error_ = err("handle", e, 100);
            }
        }
    }
    return true;
}

// --- FuturesFeed ------------------------------------------------------------------------------

namespace {
std::vector<std::string> futures_subs(const std::vector<std::string>& underlyings,
                                      const std::map<std::string, std::string>& contracts,
                                      const FuturesFeed::Resolver& resolve, const std::string& day,
                                      std::vector<std::pair<std::string, std::string>>& contract,
                                      std::map<std::string, std::string>& by_sym,
                                      std::map<std::string, std::string>& resolve_error) {
    const std::string d = day.empty() ? today_et() : day;
    for (const auto& u : underlyings) {
        std::optional<std::string> tk;
        if (auto it = contracts.find(u); it != contracts.end()) {
            tk = it->second;
        } else if (resolve) {
            try {
                tk = resolve(u, d);
            } catch (const std::exception& e) {
                resolve_error[u] = std::string(exc_name(e)) + ": " + detail::head(e.what(), 90);
                tk.reset();
            }
        }
        if (tk && !tk->empty()) {
            bool seen = false;
            for (auto& [cu, ct] : contract)
                if (cu == u) { ct = *tk; seen = true; }
            if (!seen) contract.emplace_back(u, *tk);
            by_sym[*tk] = u;
        }
    }
    std::vector<std::string> subs;
    for (const auto& [u, t] : contract) subs.push_back("Q." + t);
    return subs;
}
}  // namespace

FuturesFeed::FuturesFeed(std::vector<std::string> underlyings, std::string api_key, WsFactory ws,
                         const std::map<std::string, std::string>& contracts,
                         const Resolver& resolve, const std::string& day, Tee tee, Clock clock,
                         std::string url, bool threaded)
    : Stream(std::move(url), {}, "futures", std::move(tee), std::move(api_key), std::move(ws),
             std::move(clock), threaded),
      underlyings_(std::move(underlyings)) {
    const auto subs = futures_subs(underlyings_, contracts, resolve, day, contract_, by_sym_,
                                   resolve_error_);
    set_subs(subs);
}

FuturesFeed::~FuturesFeed() { stop(); }

bool FuturesFeed::on_message(const Value& m, long long recv_ns) {
    const Value* sym = m.find("sym");
    if (sym && (sym->is_array() || sym->is_object()))
        throw detail::PyTypeError("unhashable type");          // TypeError in the reference
    if (!sym || !sym->is_string()) return false;
    const auto it = by_sym_.find(sym->str());
    if (it == by_sym_.end()) return false;
    const std::string& u = it->second;
    const Value* bpv = m.find("bp");
    const Value* apv = m.find("ap");
    if (!bpv || bpv->is_null() || !apv || apv->is_null()) return false;
    const double bp = detail::py_float(*bpv), ap = detail::py_float(*apv);
    if (ap < bp) return false;                                    // crossed: not a mid
    const double mid = (bp + ap) / 2.0;
    const long long ts_ns = ts_of(m, recv_ns);
    const auto prev = mid_.find(u);
    const bool repeat = prev != mid_.end() && prev->second.first == mid;
    mid_[u] = {mid, ts_ns};                  // a repeat still refreshes the timestamp
    if (repeat) return false;
    ++ticks_[u];
    auto& h = hist_[u];
    h.emplace_back(ts_ns, mid);
    if (h.size() > HIST_MAX) h.pop_front();
    return true;
}

MidAge FuturesFeed::mid(const std::string& underlying, std::optional<double> max_age_s,
                        std::optional<long long> now_ns) const {
    const double mx = max_age_s ? *max_age_s : FUTURES_MAX_QUOTE_AGE_S;
    std::pair<double, long long> got;
    {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = mid_.find(underlying);
        if (it == mid_.end()) return {};
        got = it->second;
    }
    const long long now = now_ns ? *now_ns : clock_.time_ns();
    const double age = detail::py_true_div(now - got.second, NS);
    if (age < 0 || age > mx) return {std::nullopt, age};
    return {got.first, age};
}

std::optional<double> FuturesFeed::move_scale(const std::string& underlying, double horizon_s,
                                              double sigmas, std::size_t min_ticks) const {
    std::vector<std::pair<long long, double>> h;
    {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = hist_.find(underlying);
        if (it != hist_.end()) h.assign(it->second.begin(), it->second.end());
    }
    if (h.size() < min_ticks || h.empty()) return std::nullopt;
    const double span_s = detail::py_true_div(h.back().first - h.front().first, NS);
    if (span_s <= 0) return std::nullopt;
    // sum((b[1] - a[1]) ** 2 for a, b in zip(h, h[1:])) -- compensated, and pow() not x*x.
    detail::PyFloatSum ss;
    for (std::size_t i = 1; i < h.size(); ++i) ss.add(detail::py_sq(h[i].second - h[i - 1].second));
    const double var_per_s = ss.result() / span_s;
    if (var_per_s <= 0) return 0.0;
    return sigmas * std::sqrt(var_per_s * horizon_s);
}

long FuturesFeed::ticks(const std::string& underlying) const {
    std::lock_guard<std::mutex> g(mu_);
    const auto it = ticks_.find(underlying);
    return it == ticks_.end() ? 0 : it->second;
}

// --- IndexFeed --------------------------------------------------------------------------------

namespace {
std::vector<std::string> index_subs(const std::vector<std::string>& underlyings,
                                    std::map<std::string, std::string>& by_tk) {
    std::vector<std::string> subs, seen;
    for (const auto& u : underlyings) {
        const auto it = kp::massive::index::INDEX_TICKER().find(u);
        if (it == kp::massive::index::INDEX_TICKER().end()) continue;
        if (std::find(seen.begin(), seen.end(), u) != seen.end()) continue;
        seen.push_back(u);
        subs.push_back("V." + it->second);
        by_tk[it->second] = u;
    }
    return subs;
}
}  // namespace

IndexFeed::IndexFeed(std::vector<std::string> underlyings, std::string api_key, WsFactory ws,
                     Tee tee, Clock clock, std::string url, bool threaded)
    : Stream(std::move(url), {}, "index", std::move(tee), std::move(api_key), std::move(ws),
             std::move(clock), threaded),
      underlyings_(std::move(underlyings)) {
    const auto subs = index_subs(underlyings_, by_tk_);
    set_subs(subs);
}

IndexFeed::~IndexFeed() { stop(); }

bool IndexFeed::on_message(const Value& m, long long recv_ns) {
    // `V` carries `T` (ticker) and `val`; the `A` aggregate carries `sym`. Accept either.
    const Value* T = m.find("T");
    const Value* tk = (T && detail::truthy(*T)) ? T : m.find("sym");
    if (tk && (tk->is_array() || tk->is_object())) throw detail::PyTypeError("unhashable type");
    const auto it = (tk && tk->is_string()) ? by_tk_.find(tk->str()) : by_tk_.end();
    const Value* val = m.find("val");
    if (it == by_tk_.end() || !val || val->is_null()) return false;
    const long long ts_ns = ts_of(m, recv_ns);
    const double v = detail::py_float(*val);
    const std::string& u = it->second;
    const auto prev = q_.find(u);
    const bool changed = prev == q_.end() || prev->second.value != std::optional<double>(v);
    kp::massive::index::IndexQuote q;
    q.underlying = u;
    q.ticker = tk->str();
    q.value = v;
    q.ts_ns = ts_ns;
    q.market_status = "open";
    q_[u] = std::move(q);
    return changed;
}

std::map<std::string, kp::massive::index::IndexQuote>
IndexFeed::snapshot(const std::vector<std::string>& underlyings) const {
    const auto& want = underlyings.empty() ? underlyings_ : underlyings;
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, kp::massive::index::IndexQuote> out;
    for (const auto& u : want)
        if (auto it = q_.find(u); it != q_.end()) out.emplace(u, it->second);
    return out;
}

}  // namespace kp::massive::streams

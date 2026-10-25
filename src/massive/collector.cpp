#include "kp/massive/client.hpp"

#include <algorithm>
#include <filesystem>

#include "detail.hpp"
#include "kp/massive/rest.hpp"

namespace kp::massive::client {

using kp::json::Value;

// --- QuoteCollector ---------------------------------------------------------------------------

namespace {

const char* exc_name(const std::exception& e) {
    if (dynamic_cast<const MassiveError*>(&e)) return "MassiveError";
    if (dynamic_cast<const WsTimeout*>(&e)) return "WebSocketTimeoutException";
    if (dynamic_cast<const std::invalid_argument*>(&e)) return "ValueError";
    return "WebSocketException";
}

std::string action_frame(const char* action, const std::string& params) {
    Value v;
    v.set("action", Value::make_string(action));
    v.set("params", Value::make_string(params));
    return kp::json::dumps(v);
}

std::string join_q(const std::vector<std::string>& tickers) {
    std::string s;
    for (const auto& t : tickers) {
        if (!s.empty()) s += ',';
        s += "Q.";
        s += t;
    }
    return s;
}

// A dict key for `sym`. Strings are themselves; None and the other hashables get a NUL-prefixed
// spelling no OCC symbol can collide with. nullopt = unhashable (TypeError in the reference).
std::optional<std::string> sym_key(const Value* s) {
    if (!s || s->is_null()) return std::string("\0None", 5);
    if (s->is_string()) return s->str();
    if (s->is_array() || s->is_object()) return std::nullopt;
    return std::string(1, '\0') + detail::py_repr(*s);
}

bool numeric(const Value& v) { return v.is_number() || v.is_bool(); }

long double as_ld(const Value& v) {
    if (v.is_bool()) return v.boolean() ? 1.0L : 0.0L;
    return v.is_int() ? (long double)v.integer() : (long double)v.num();   // both exact in 64 bits
}

// Python's == over json values: numbers by value (int/float/bool mix), containers structurally,
// anything else of differing type unequal.
bool py_eq(const Value& a, const Value& b) {
    if (numeric(a) && numeric(b)) {
        if (a.is_int() && b.is_int()) return a.integer() == b.integer();
        return as_ld(a) == as_ld(b);
    }
    if (a.type() != b.type()) return false;
    if (a.is_null()) return true;
    if (a.is_string()) return a.str() == b.str();
    if (a.is_array()) {
        if (a.arr().size() != b.arr().size()) return false;
        for (std::size_t i = 0; i < a.arr().size(); ++i)
            if (!py_eq(a.arr()[i], b.arr()[i])) return false;
        return true;
    }
    if (a.is_object()) {
        if (a.obj().size() != b.obj().size()) return false;
        for (const auto& [k, v] : a.obj()) {
            const Value* w = b.find(k);
            if (!w || !py_eq(v, *w)) return false;
        }
        return true;
    }
    return false;
}

// Python's ordering: -1/0/1, or nullopt where it raises TypeError. Lists compare lexicographically
// (first unequal element decides, then length); numbers and strings natively; nothing else orders.
std::optional<int> py_cmp(const Value& a, const Value& b) {
    if (numeric(a) && numeric(b)) {
        if (a.is_int() && b.is_int()) return a.integer() < b.integer() ? -1 : a.integer() > b.integer();
        const long double x = as_ld(a), y = as_ld(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.is_string() && b.is_string()) return a.str() < b.str() ? -1 : (a.str() > b.str() ? 1 : 0);
    if (a.is_array() && b.is_array()) {
        const auto& x = a.arr();
        const auto& y = b.arr();
        for (std::size_t i = 0; i < x.size() && i < y.size(); ++i)
            if (!py_eq(x[i], y[i])) return py_cmp(x[i], y[i]);
        return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
    }
    return std::nullopt;
}

// Python's `a >= b`; nullopt where Python raises TypeError.
std::optional<bool> py_ge(const Value& a, const Value& b) {
    const auto c = py_cmp(a, b);
    if (!c) return std::nullopt;
    return *c >= 0;
}

std::optional<double> field(const Value& m, const char* k) {
    const Value* v = m.find(k);
    if (!v || !numeric(*v)) return std::nullopt;
    return detail::py_float(*v);
}

}  // namespace

QuoteCollector::QuoteCollector(std::vector<std::string> tickers, std::string api_key, WsFactory ws,
                               Clock clock, std::string raw_path, std::string url, bool threaded)
    : tickers_(std::move(tickers)), key_(std::move(api_key)), ws_factory_(std::move(ws)),
      clock_(std::move(clock)), raw_path_(std::move(raw_path)), url_(std::move(url)),
      threaded_(threaded), subscribed_(tickers_.begin(), tickers_.end()) {}

QuoteCollector::~QuoteCollector() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    close_socket();
    close_raw();
}

void QuoteCollector::close_socket() {
    std::lock_guard<std::mutex> g(send_mu_);
    if (ws_) {
        try { ws_->close(); } catch (...) {}
    }
    ws_.reset();
}

void QuoteCollector::close_raw() {
    if (raw_fh_) std::fclose(raw_fh_);
    raw_fh_ = nullptr;
}

void QuoteCollector::connect_and_auth() {
    {
        auto ws = ws_factory_();
        ws->connect(url_, 20.0);
        std::lock_guard<std::mutex> g(send_mu_);
        ws_ = std::move(ws);
    }
    auto recv = [&]() {
        auto raw = ws_->recv(20.0);
        if (!raw) throw WsTimeout("timed out");
        return kp::json::parse(*raw);
    };
    auto log = [&](Value v) {
        std::lock_guard<std::mutex> g(mu_);
        status_log_.push_back(std::move(v));       // deque(maxlen): the oldest falls off, uncounted
        if (status_log_.size() > STATUS_LOG_MAX) status_log_.pop_front();
    };
    log(recv());
    {
        Value a;
        a.set("action", Value::make_string("auth"));
        a.set("params", Value::make_string(key_));
        std::lock_guard<std::mutex> g(send_mu_);
        ws_->send_text(kp::json::dumps(a));
    }
    const Value auth = recv();
    log(auth);
    // any(m.get("status") == "auth_success" for m in auth), with Python's iteration rules.
    bool ok = false;
    if (auth.is_array()) {
        for (const auto& m : auth.arr()) {
            if (!m.is_object()) throw std::runtime_error("'" + std::string("element") + "' object has no attribute 'get'");
            const Value* s = m.find("status");
            if (s && s->is_string() && s->str() == "auth_success") { ok = true; break; }
        }
    } else if ((auth.is_object() && !auth.obj().empty()) || (auth.is_string() && !auth.str().empty())) {
        throw std::runtime_error("'str' object has no attribute 'get'");
    } else if (auth.is_number() || auth.is_bool() || auth.is_null()) {
        throw std::runtime_error("object is not iterable");
    }
    if (!ok)
        throw MassiveError("auth failed: " + (auth.is_string() ? auth.str() : detail::py_repr(auth)));
    std::lock_guard<std::mutex> g(send_mu_);
    ws_->send_text(action_frame("subscribe", join_q(tickers_)));
}

void QuoteCollector::spawn() {
    stop_ = false;
    running_ = true;
    if (threaded_) thread_ = std::thread([this] { run_pump(); });
}

void QuoteCollector::join_pump() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

QuoteCollector& QuoteCollector::start(int attempts, double backoff) {
    if (!raw_path_.empty()) {
        std::filesystem::path p(raw_path_);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        raw_fh_ = std::fopen(raw_path_.c_str(), "a");
    }
    for (int attempt = 0; attempt < attempts; ++attempt) {
        try {
            connect_and_auth();
            break;
        } catch (const MassiveError& e) {
            close_socket();
            if (std::string(e.what()).find("max_connections") == std::string::npos
                || attempt == attempts - 1) {
                close_raw();
                throw;
            }
            clock_.sleep(backoff);
        } catch (...) {
            close_socket();
            close_raw();
            throw;
        }
    }
    spawn();
    return *this;
}

QuoteCollector& QuoteCollector::stop() {
    join_pump();
    running_ = false;
    close_socket();
    close_raw();
    return *this;
}

bool QuoteCollector::reconnect(int attempts, double backoff) {
    // The reference closes the socket first and lets the pump die on it; closing a socket another
    // thread is reading is not something kp::net::WebSocket promises, so stop and join FIRST
    // (the pump's 1 s recv bounds the wait), then close. The end state is the same.
    join_pump();
    close_socket();
    stop_ = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        latest_.clear();
    }
    if (!subscribed_.empty()) tickers_.assign(subscribed_.begin(), subscribed_.end());
    for (int attempt = 0; attempt < attempts; ++attempt) {
        try {
            connect_and_auth();
            break;
        } catch (const MassiveError& e) {
            close_socket();
            if (std::string(e.what()).find("max_connections") == std::string::npos
                || attempt == attempts - 1) {
                std::lock_guard<std::mutex> g(mu_);
                pump_error_ = "reconnect failed: " + detail::head(e.what(), 160);
                return false;
            }
            clock_.sleep(backoff);
        } catch (const std::exception& e) {
            close_socket();
            std::lock_guard<std::mutex> g(mu_);
            pump_error_ = std::string("reconnect failed: ") + exc_name(e) + ": " + detail::head(e.what(), 160);
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        pump_error_.reset();
        pump_exited_at_.reset();
        last_msg_t_.reset();
    }
    ++reconnects_;
    spawn();
    return true;
}

bool QuoteCollector::run_pump() {
    running_ = true;
    while (!stop_) {
        std::optional<std::string> raw;
        try {
            if (!ws_) throw std::runtime_error("'NoneType' object has no attribute 'recv'");
            raw = ws_->recv(1.0);
        } catch (const std::exception& e) {
            // Recorded, not swallowed: a feed that is DOWN must be distinguishable from one that
            // is quiet, and `latest` alone cannot tell them apart.
            std::lock_guard<std::mutex> g(mu_);
            pump_error_ = std::string(exc_name(e)) + ": " + detail::head(e.what(), 200);
            pump_exited_at_ = clock_.monotonic();
            running_ = false;
            return false;
        }
        if (!raw || raw->empty()) continue;
        if (raw_fh_) {
            std::fwrite(raw->data(), 1, raw->size(), raw_fh_);
            if (raw->back() != '\n') std::fputc('\n', raw_fh_);
        }
        if (!ingest(*raw)) {
            running_ = false;
            return true;
        }
    }
    // Leaving on stop: stop()/reconnect() own `running_` from here (a paused test pump is still
    // "alive" to the reference's FakeThread; stop_ already makes alive() false).
    return false;
}

bool QuoteCollector::ingest(const std::string& raw) {
    Value msgs;
    try {
        msgs = kp::json::parse(raw);
    } catch (const std::exception&) {
        return true;                                  // json.JSONDecodeError: continue
    }
    std::lock_guard<std::mutex> g(mu_);
    ++frame_count_;
    if (!msgs.is_array()) {
        // `for m in msgs` over a non-list: a dict yields its keys and a str its characters, and
        // the first `m.get` raises; a number/bool/None is not iterable at all. Empty containers
        // iterate nothing.
        if ((msgs.is_object() && !msgs.obj().empty()) || (msgs.is_string() && !msgs.str().empty())) {
            ++msg_count_;
            return false;
        }
        return msgs.is_object() || msgs.is_string();
    }
    for (const Value& m : msgs.arr()) {
        ++msg_count_;
        if (!m.is_object()) return false;
        const Value* ev = m.find("ev");
        if (!(ev && ev->is_string() && ev->str() == "Q")) {
            if (status_log_.size() == STATUS_LOG_MAX) ++status_dropped_;
            status_log_.push_back(m);
            if (status_log_.size() > STATUS_LOG_MAX) status_log_.pop_front();
            continue;
        }
        const auto key = sym_key(m.find("sym"));
        if (!key) return false;
        const Value* qv = m.find("q");
        const Value q = qv ? *qv : Value::make_int(0);
        auto it = latest_.find(*key);
        bool take = it == latest_.end();
        if (!take) {
            const auto ge = py_ge(q, it->second.q);
            if (!ge) return false;
            take = *ge;
        }
        if (take) {
            Entry e;
            e.quote.q.bp = field(m, "bp");
            e.quote.q.ap = field(m, "ap");
            e.quote.q.bs = field(m, "bs");
            e.quote.q.as = field(m, "as");
            e.quote.q.present = true;
            if (const Value* t = m.find("t"); t && t->is_int()) e.quote.t_ms = t->integer();
            e.q = q;
            latest_[*key] = std::move(e);
        }
        last_msg_t_ = clock_.monotonic();
    }
    return true;
}

bool QuoteCollector::alive() const { return running_ && !stop_; }

CollectorHealth QuoteCollector::health(std::optional<double> now) const {
    const double t = now ? *now : clock_.monotonic();
    CollectorHealth h;
    h.alive = alive();
    std::lock_guard<std::mutex> g(mu_);
    h.pump_error = pump_error_;
    if (last_msg_t_) h.quote_age_s = t - *last_msg_t_;
    h.n_symbols = latest_.size();
    h.reconnects = reconnects_;
    h.frames = frame_count_;
    return h;
}

kp::massive::OptionSnapshot QuoteCollector::snapshot() const {
    std::lock_guard<std::mutex> g(mu_);
    kp::massive::OptionSnapshot out;
    out.reserve(latest_.size());
    for (const auto& [k, e] : latest_) out.emplace(k, e.quote);
    return out;
}

bool QuoteCollector::wait_for_coverage(std::size_t min_symbols, double timeout, double poll) const {
    const double deadline = clock_.monotonic() + timeout;
    while (clock_.monotonic() < deadline) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (latest_.size() >= min_symbols) return true;
        }
        clock_.sleep(poll);
    }
    return false;
}

std::size_t QuoteCollector::send(const char* action, const std::vector<std::string>& tickers) {
    if (tickers.empty()) return 0;
    const std::string frame = action_frame(action, join_q(tickers));
    std::lock_guard<std::mutex> g(send_mu_);
    if (!ws_) throw std::runtime_error("'NoneType' object has no attribute 'send'");
    ws_->send_text(frame);
    return tickers.size();
}

std::size_t QuoteCollector::subscribe(const std::vector<std::string>& tickers) {
    std::vector<std::string> fresh;
    for (const auto& t : tickers)
        if (!subscribed_.count(t)) fresh.push_back(t);
    if (fresh.empty()) return 0;
    if (subscribed_.size() + fresh.size() > MAX_TICKERS)
        throw MassiveError("subscribe would reach " + std::to_string(subscribed_.size() + fresh.size())
                           + " tickers, over the " + std::to_string(MAX_TICKERS) + " cap");
    send("subscribe", fresh);
    subscribed_.insert(fresh.begin(), fresh.end());
    return fresh.size();
}

std::size_t QuoteCollector::unsubscribe(const std::vector<std::string>& tickers) {
    std::vector<std::string> gone;
    for (const auto& t : tickers)
        if (subscribed_.count(t)) gone.push_back(t);
    if (gone.empty()) return 0;
    send("unsubscribe", gone);
    for (const auto& t : gone) subscribed_.erase(t);
    // Evict the cached quotes too: a symbol we no longer receive would otherwise serve its last
    // value forever, and the pricer treats a present quote as a current one.
    std::lock_guard<std::mutex> g(mu_);
    for (const auto& t : gone) latest_.erase(t);
    return gone.size();
}

std::optional<std::pair<std::size_t, std::size_t>>
QuoteCollector::recenter(const std::vector<std::string>& want, bool force) {
    const double now = clock_.monotonic();
    if (!force && (now - last_recenter_) < RECENTER_MIN_INTERVAL) return std::nullopt;
    const std::set<std::string> w(want.begin(), want.end());
    if (w.size() > MAX_TICKERS)
        throw MassiveError(std::to_string(w.size()) + " tickers requested, over the "
                           + std::to_string(MAX_TICKERS) + " cap");
    // Set iteration order in the reference is hash order, so the tickers inside one
    // (un)subscribe frame come out in an arbitrary order there; here they are sorted. The SET
    // sent, and every count, are identical.
    std::vector<std::string> gone, fresh;
    std::set_difference(subscribed_.begin(), subscribed_.end(), w.begin(), w.end(),
                        std::back_inserter(gone));
    const std::size_t removed = unsubscribe(gone);
    std::set_difference(w.begin(), w.end(), subscribed_.begin(), subscribed_.end(),
                        std::back_inserter(fresh));
    const std::size_t added = subscribe(fresh);
    last_recenter_ = now;
    tickers_.assign(subscribed_.begin(), subscribed_.end());
    if (added || removed) ++recenter_count_;
    return std::make_pair(added, removed);
}

long QuoteCollector::frame_count() const { std::lock_guard<std::mutex> g(mu_); return frame_count_; }
long QuoteCollector::msg_count() const { std::lock_guard<std::mutex> g(mu_); return msg_count_; }
std::size_t QuoteCollector::status_log_size() const { std::lock_guard<std::mutex> g(mu_); return status_log_.size(); }
long QuoteCollector::status_dropped() const { std::lock_guard<std::mutex> g(mu_); return status_dropped_; }
std::optional<double> QuoteCollector::last_msg_t() const { std::lock_guard<std::mutex> g(mu_); return last_msg_t_; }
std::optional<std::string> QuoteCollector::pump_error() const { std::lock_guard<std::mutex> g(mu_); return pump_error_; }

}  // namespace kp::massive::client

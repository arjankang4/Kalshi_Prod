#include "kp/kalshi/ws.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "kp/storage/pyerr.hpp"

namespace kp::kalshi {

using kp::json::Value;
using kp::storage::PyError;
using kp::storage::py_float;
using kp::storage::py_slice;
using kp::storage::py_str;
using kp::storage::py_type_name;

// --- clock ------------------------------------------------------------------------------------

double FeedClock::monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void FeedClock::sleep(double s) {
    if (s > 0) std::this_thread::sleep_for(std::chrono::duration<double>(s));
}
std::int64_t FeedClock::time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
FeedClock& FeedClock::real() {
    static FeedClock c;
    return c;
}

std::string fp_micros(std::int64_t micros) {
    char b[64];
    std::snprintf(b, sizeof b, "%.4f", (double)micros / 1000000.0);
    return b;
}
std::string cc_centi(std::int64_t centi) {
    char b[64];
    std::snprintf(b, sizeof b, "%.2f", (double)centi / 100.0);
    return b;
}

namespace {

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

// d.get(k) where d must be a dict.
Value pyget(const Value& d, const char* k) {
    if (!d.is_object()) throw PyError("AttributeError", "'" + py_type_name(d) + "' object has no attribute 'get'");
    const Value* v = d.find(k);
    return v ? *v : Value();
}

bool is_num(const Value& v) { return v.is_number() || v.is_bool(); }
bool num_is_int(const Value& v) { return v.is_bool() || v.is_int(); }
long long as_int(const Value& v) { return v.is_bool() ? (v.boolean() ? 1 : 0) : v.integer(); }
double as_dbl(const Value& v) { return v.is_bool() ? (v.boolean() ? 1.0 : 0.0) : v.is_int() ? (double)v.integer() : v.num(); }

// Python ==, for the JSON-decoded values a frame can carry.
bool py_eq(const Value& a, const Value& b) {
    if (is_num(a) && is_num(b)) {
        if (num_is_int(a) && num_is_int(b)) return as_int(a) == as_int(b);
        return as_dbl(a) == as_dbl(b);
    }
    if (a.type() != b.type()) return false;
    if (a.is_null()) return true;
    if (a.is_string()) return a.str() == b.str();
    return kp::json::dumps(a) == kp::json::dumps(b);
}
bool py_eq_int(const Value& a, long long x) { return is_num(a) && (num_is_int(a) ? as_int(a) == x : as_dbl(a) == (double)x); }

void check_hashable(const Value& v) {
    if (v.is_array() || v.is_object())
        throw PyError("TypeError", "unhashable type: '" + py_type_name(v) + "'");
}

// self.seq + 1
Value plus_one(const Value& s) {
    if (num_is_int(s)) return Value::make_int(as_int(s) + 1);
    if (s.is_number()) return Value::make_number(s.num() + 1.0);
    if (s.is_string()) throw PyError("TypeError", "can only concatenate str (not \"int\") to str");
    if (s.is_array()) throw PyError("TypeError", "can only concatenate list (not \"int\") to list");
    throw PyError("TypeError", "unsupported operand type(s) for +: '" + py_type_name(s) + "' and 'int'");
}

// A ticker as a map key. Real tickers are strings; anything else is keyed by its str() (a
// documented divergence -- the reference would key by the raw value).
std::string key_of(const Value& t) { return py_str(t); }

std::string exc_text(const std::exception& e, std::size_t n) {
    std::string type = "RuntimeError";
    if (const auto* pe = dynamic_cast<const PyError*>(&e)) type = pe->type;
    else if (dynamic_cast<const kp::net::WsClosed*>(&e)) type = "WsClosed";
    else if (dynamic_cast<const kp::net::WsError*>(&e)) type = "WsError";
    return type + ": " + py_slice(e.what(), n);
}

Value str_array(const std::vector<std::string>& v, std::size_t from, std::size_t to) {
    Value a = Value::make_array({});
    for (std::size_t i = from; i < to && i < v.size(); ++i) a.push(Value::make_string(v[i]));
    return a;
}

}  // namespace

MarketFeed::MarketFeed(std::vector<std::string> tickers, std::vector<std::string> channels, TeeFn tee,
                       FeedClock* clock)
    : tickers_(std::move(tickers)), channels_(std::move(channels)), tee_(std::move(tee)),
      clock_(clock ? clock : &FeedClock::real()) {}

MarketFeed::~MarketFeed() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (ws_) ws_->close();
}

// --- lifecycle --------------------------------------------------------------------------------

MarketFeed& MarketFeed::start(const std::string& ws_url, const std::function<kp::net::Headers()>& auth,
                              int attempts, double backoff) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        try {
            // Signed exactly as the reference signs it: _auth_headers("GET", path) minus
            // Content-Type. NO Origin header -- net::WebSocket refuses to send one.
            kp::net::Headers h;
            for (auto& kv : auth()) if (kv.first != "Content-Type") h.push_back(kv);
            auto ws = std::make_unique<kp::net::WebSocket>();
            ws->connect(ws_url, h, 20.0);
            ws_ = std::move(ws);
            break;
        } catch (const std::exception&) {
            ws_.reset();
            if (attempt == attempts - 1) throw;
            clock_->sleep(backoff);
        }
    }
    kp::net::WebSocket* ws = ws_.get();
    send_ = [ws](const std::string& s) { ws->send_text(s); };
    {
        std::lock_guard<std::mutex> lk(mu_);
        started_t_ = clock_->monotonic();
    }
    // THE PUMP STARTS BEFORE THE SUBSCRIBE: a chunked subscribe waits for the first ack's sid,
    // which only the pump can record.
    stop_ = false;
    pumping_ = true;
    thread_ = std::thread([this, ws] {
        run_pump([ws]() { return ws->recv(1.0); });
        pumping_ = false;
    });
    subscribe(tickers_);
    return *this;
}

MarketFeed& MarketFeed::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();      // recv times out every 1 s, so this is bounded
    if (ws_) { try { ws_->close(); } catch (const std::exception&) {} }
    ws_.reset();
    send_ = nullptr;
    return *this;
}

bool MarketFeed::alive() const { return pumping_ && !stop_; }

// --- the pump ---------------------------------------------------------------------------------

void MarketFeed::run_pump(const RecvFn& recv) {
    while (!stop_) {
        std::optional<std::string> raw;
        try {
            raw = recv();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mu_);
            pump_error_ = exc_text(e, 80);
            return;
        }
        if (!raw || raw->empty()) continue;
        on_raw(*raw);
    }
}

void MarketFeed::on_raw(const std::string& raw) {
    if (raw.empty()) return;
    // Stamped at RECEIPT, before parsing: the only timestamp in the record that is ours.
    const std::int64_t recv_ns = clock_->time_ns();
    Value msg;
    try {
        msg = kp::json::parse(raw);
    } catch (const std::exception&) {
        return;                                              // not JSON: skipped, not counted
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++frames_;
        last_frame_t_ = clock_->monotonic();
    }
    // BEFORE handle and outside its guard: the frame that makes the handler throw is exactly the
    // frame worth having on the tape.
    if (tee_) tee_(recv_ns, msg);
    try {
        handle(msg);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(mu_);
        pump_error_ = "handle: " + exc_text(e, 80);
    }
}

void MarketFeed::invalidate_all() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : valid_) kv.second = false;
}

void MarketFeed::handle(const Value& msg) {
    const Value typ = pyget(msg, "type");
    const std::string t = typ.is_string() ? typ.str() : std::string("\x01<non-str>");
    if (t == "subscribed") {
        Value p = pyget(msg, "msg");
        if (!truthy(p)) p = Value::make_object({});
        const Value ch = pyget(p, "channel");
        const Value sid = pyget(p, "sid");
        check_hashable(ch);
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : sids_) if (py_eq(kv.first, ch)) { kv.second = sid; return; }
        sids_.emplace_back(ch, sid);
        return;
    }
    if (t == "error") {
        on_error(msg);
        return;
    }
    const Value seq = pyget(msg, "seq");
    check_hashable(typ);
    const bool seq_bearing = t == "orderbook_delta" || t == "orderbook_snapshot" || t == "ok" || t == "unsubscribed";
    if (seq_bearing && !seq.is_null()) {
        Value cur;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cur = seq_;
        }
        if (!cur.is_null() && !py_eq(seq, plus_one(cur))) {
            // A gap on the SUBSCRIPTION invalidates every book on it: we do not know which market
            // the missing frame belonged to.
            std::vector<std::string> all;
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++gaps_;
                for (auto& kv : valid_) kv.second = false;
                for (const auto& kv : books_) all.push_back(kv.first);
            }
            request_snapshot(all);
        }
        std::lock_guard<std::mutex> lk(mu_);
        seq_ = seq;
    }
    Value body = pyget(msg, "msg");
    if (!truthy(body)) body = Value::make_object({});
    if (t == "orderbook_snapshot") {
        on_snapshot(body, seq);
    } else if (t == "orderbook_delta") {
        on_delta(body);
    } else if (t == "fill") {
        {
            std::lock_guard<std::mutex> lk(mu_);
            fills_.push_back(body);
        }
        // post_position_fp is our position AFTER this fill: current the instant we learn of it.
        const Value tk = pyget(body, "market_ticker");
        const Value pp = pyget(body, "post_position_fp");
        set_position(&tk, &pp);
    } else if (t == "market_position") {
        // SINGULAR: `market_positions` is the channel, `market_position` the frame type.
        const Value tk = pyget(body, "market_ticker");
        const Value pp = pyget(body, "position_fp");
        set_position(&tk, &pp);
    }
}

void MarketFeed::on_error(const Value& msg) {
    Value body = pyget(msg, "msg");
    if (!truthy(body)) body = msg;
    const Value code = pyget(body, "code");
    const Value m = pyget(body, "msg");
    Value rec = Value::make_object({});
    rec.set("code", code);
    rec.set("msg", Value::make_string(py_slice(py_str(m), 80)));
    {
        std::lock_guard<std::mutex> lk(mu_);
        command_errors_.push(rec);
    }
    if (py_eq_int(code, ERR_TOO_MANY_COMMANDS)) {
        // Back off multiplicatively: the limit is unpublished, so the only way to find it is to
        // stop asking for a while.
        std::lock_guard<std::recursive_mutex> lk(cmd_mu_);
        const double d = cmd_backoff_.load() * 2;
        const double hi = COMMAND_BACKOFF_S > d ? COMMAND_BACKOFF_S : d;           // max(2.0, b*2)
        cmd_backoff_ = hi < COMMAND_BACKOFF_MAX_S ? hi : COMMAND_BACKOFF_MAX_S;     // min(30.0, .)
    } else if (py_eq_int(code, ERR_MARKET_LIMIT)) {
        // Surfaced, never silently trimmed: the universe has to shrink or be split.
    } else {
        check_hashable(code);
        if (py_eq_int(code, 10) || py_eq_int(code, 17) || py_eq_int(code, 25)) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& kv : valid_) kv.second = false;
                ++reconnects_;
            }
            subscribe(tickers_);
        }
    }
}

void MarketFeed::on_snapshot(const Value& body, const Value& seq) {
    const Value t = pyget(body, "market_ticker");
    if (!truthy(t)) return;
    check_hashable(t);
    const std::string k = key_of(t);
    const std::int64_t now_ns = clock_->time_ns();
    std::lock_guard<std::mutex> lk(mu_);
    kp::storage::Book& b = books_[k];                       // setdefault
    std::optional<std::int64_t> s;
    if (seq.is_int()) s = seq.integer();
    b.load(body, now_ns, s);                                 // may throw half-way, as the reference
    valid_[k] = true;
    // A snapshot repays the command backoff: the stream is answering us again. (Never take the
    // command lock while holding mu_: send() takes them in the other order.)
    cmd_backoff_ = 0.0;
}

void MarketFeed::on_delta(const Value& body) {
    const Value t = pyget(body, "market_ticker");
    check_hashable(t);
    const std::string k = key_of(t);
    bool refused = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        kp::storage::Book* b = t.is_null() ? nullptr : books_.find(k);
        const bool* v = t.is_null() ? nullptr : valid_.find(k);
        // No snapshot yet, or invalidated by a gap: a delta on an unknown base is refused.
        if (!b || !v || !*v) return;
        try {
            b->apply(body);
        } catch (const PyError& e) {
            if (!e.is_value_error) throw;
            // A negative size is proof of a frame we never saw: refuse the book, ask again.
            valid_[k] = false;
            ++gaps_;
            refused = true;
        }
    }
    if (refused) request_snapshot({k});
}

void MarketFeed::set_position(const Value* ticker, const Value* value_fp) {
    if (!ticker || !truthy(*ticker) || !value_fp || value_fp->is_null()) return;
    double n;
    try {
        n = py_float(*value_fp);
    } catch (const PyError&) {
        return;                                              // TypeError / ValueError: ignored
    }
    check_hashable(*ticker);
    std::lock_guard<std::mutex> lk(mu_);
    positions_[key_of(*ticker)] = n;
}

// --- commands ---------------------------------------------------------------------------------

std::optional<long> MarketFeed::send(Value payload) {
    std::lock_guard<std::recursive_mutex> lk(cmd_mu_);
    const double now = clock_->monotonic();
    const double wait = std::max(0.0, next_cmd_t_ - now);
    if (wait != 0.0) clock_->sleep(wait);
    ++cmd_id_;
    payload.set("id", Value::make_int(cmd_id_));
    try {
        if (!send_) throw PyError("AttributeError", "'NoneType' object has no attribute 'send'");
        send_(kp::json::dumps(payload, kp::json::COMPACT));
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> l2(mu_);
        command_errors_.push(Value::make_string("send: " + exc_text(e, 60)));
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> l2(mu_);
        ++commands_;
    }
    next_cmd_t_ = clock_->monotonic() + COMMAND_MIN_INTERVAL_S + cmd_backoff_.load();
    return cmd_id_;
}

Value MarketFeed::sid_of(const std::string& channel) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : sids_) if (kv.first.is_string() && kv.first.str() == channel) return kv.second;
    return Value();
}

Value MarketFeed::await_sid(const std::string& channel, double timeout) {
    const double deadline = clock_->monotonic() + timeout;
    while (clock_->monotonic() < deadline) {
        const Value sid = sid_of(channel);
        if (!sid.is_null()) return sid;
        if (stop_ || !send_) return Value();
        clock_->sleep(0.05);
    }
    return sid_of(channel);
}

std::optional<long> MarketFeed::subscribe(const std::vector<std::string>& want, std::size_t chunk,
                                          double sid_timeout) {
    if (want.empty()) return std::nullopt;
    if (chunk == 0) chunk = MAX_SUBSCRIBE_MARKETS;
    // The ACCOUNT channels take NO market filter: a list FILTERS them, and a chunked list once
    // made every fill outside the first 500 tickers silently stop arriving.
    std::vector<std::string> market_scoped, account_scoped;
    for (const auto& c : channels_) {
        bool m = false;
        for (const auto& s : MARKET_SCOPED_CHANNELS) if (s == c) m = true;
        (m ? market_scoped : account_scoped).push_back(c);
    }
    std::optional<long> first;
    if (!account_scoped.empty()) {
        Value p = Value::make_object({});
        p.set("cmd", Value::make_string("subscribe"));
        Value params = Value::make_object({});
        params.set("channels", str_array(account_scoped, 0, account_scoped.size()));
        p.set("params", params);
        first = send(p);
    }
    if (market_scoped.empty()) return first;
    Value p = Value::make_object({});
    p.set("cmd", Value::make_string("subscribe"));
    Value params = Value::make_object({});
    params.set("channels", str_array(market_scoped, 0, market_scoped.size()));
    params.set("market_tickers", str_array(want, 0, chunk));
    p.set("params", params);
    const auto cmd_id = send(p);
    if (!first) first = cmd_id;
    if (want.size() <= chunk) return first;
    // Every later chunk is add_markets against the FIRST sid -- a second subscribe would open a
    // second sid, and request_snapshot reads exactly one, so half the board would never be
    // re-snapshotted after a gap.
    const Value sid = await_sid(market_scoped[0], sid_timeout);
    for (std::size_t i = chunk; i < want.size(); i += chunk) {
        const std::size_t n = std::min(chunk, want.size() - i);
        Value q = Value::make_object({});
        Value qp = Value::make_object({});
        if (sid.is_null()) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "subscribe: no sid after %.0fs, adding %zu markets as a second subscription",
                          sid_timeout, n);
            {
                std::lock_guard<std::mutex> lk(mu_);
                command_errors_.push(Value::make_string(buf));
            }
            q.set("cmd", Value::make_string("subscribe"));
            qp.set("channels", str_array(market_scoped, 0, market_scoped.size()));
            qp.set("market_tickers", str_array(want, i, i + n));
        } else {
            q.set("cmd", Value::make_string("update_subscription"));
            qp.set("sids", Value::make_array({sid}));
            qp.set("action", Value::make_string("add_markets"));
            qp.set("market_tickers", str_array(want, i, i + n));
        }
        q.set("params", qp);
        send(q);
    }
    return first;
}

int MarketFeed::request_snapshot(const std::vector<std::string>& want) {
    const Value sid = sid_of("orderbook_delta");
    if (sid.is_null()) return 0;
    int sent = 0;
    for (std::size_t i = 0; i < want.size(); i += MAX_SNAPSHOT_REQUEST) {
        Value q = Value::make_object({});
        q.set("cmd", Value::make_string("update_subscription"));
        Value qp = Value::make_object({});
        qp.set("sids", Value::make_array({sid}));
        qp.set("action", Value::make_string("get_snapshot"));
        qp.set("market_tickers", str_array(want, i, i + MAX_SNAPSHOT_REQUEST));
        q.set("params", qp);
        const auto id = send(q);
        if (id && *id != 0) ++sent;
    }
    return sent;
}

// --- reads ------------------------------------------------------------------------------------

std::optional<Value> MarketFeed::entry(const std::string& ticker) const {
    std::lock_guard<std::mutex> lk(mu_);
    const bool* v = valid_.find(ticker);
    if (!v || !*v) return std::nullopt;
    const kp::storage::Book* b = books_.find(ticker);
    if (!b) return std::nullopt;
    auto side = [](const std::map<std::int64_t, std::int64_t>& m) {
        Value a = Value::make_array({});
        for (const auto& [p, s] : m)
            a.push(Value::make_array({Value::make_string(fp_micros(p)), Value::make_string(cc_centi(s))}));
        return a;
    };
    Value e = Value::make_object({});
    e.set("yes", side(b->yes));
    e.set("no", side(b->no));
    return e;
}

Value MarketFeed::books(const std::optional<std::vector<std::string>>& tickers) const {
    const std::vector<std::string>& want = tickers ? *tickers : tickers_;
    Value out = Value::make_object({});
    for (const auto& t : want) {
        auto e = entry(t);
        if (e) out.set(t, std::move(*e));
    }
    return out;
}

std::vector<Value> MarketFeed::drain_fills() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Value> out;
    out.swap(fills_);
    return out;
}

std::vector<Value> MarketFeed::pending_fills() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fills_;
}

Value MarketFeed::positions() const {
    std::lock_guard<std::mutex> lk(mu_);
    Value out = Value::make_object({});
    for (const auto& [k, v] : positions_) out.set(k, Value::make_number(v));
    return out;
}

double MarketFeed::position(const std::string& ticker) const {
    std::lock_guard<std::mutex> lk(mu_);
    const double* v = positions_.find(ticker);
    return v ? *v : 0.0;
}

long MarketFeed::seed_positions(const Value& positions) {
    std::lock_guard<std::mutex> lk(mu_);
    if (truthy(positions)) {
        if (!positions.is_object()) throw PyError("AttributeError", "'" + py_type_name(positions) + "' object has no attribute 'items'");
        for (const auto& [tk, n] : positions.obj())
            if (!positions_.contains(tk)) positions_[tk] = py_float(n);
    }
    return (long)positions_.size();
}

Value MarketFeed::health(std::optional<double> now) const {
    const double t = now ? *now : clock_->monotonic();
    std::lock_guard<std::mutex> lk(mu_);
    long n_valid = 0;
    for (const auto& kv : valid_) if (kv.second) ++n_valid;
    Value h = Value::make_object({});
    h.set("alive", Value::make_bool(alive()));
    h.set("pump_error", pump_error_ ? Value::make_string(*pump_error_) : Value());
    h.set("frame_age_s", last_frame_t_ ? Value::make_number(t - *last_frame_t_) : Value());
    h.set("n_books", Value::make_int((long long)books_.size()));
    h.set("n_valid", Value::make_int(n_valid));
    h.set("n_wanted", Value::make_int((long long)tickers_.size()));
    h.set("coverage", Value::make_number(tickers_.empty() ? 0.0 : (double)n_valid / (double)tickers_.size()));
    h.set("seq", seq_);
    h.set("gaps", Value::make_int(gaps_));
    h.set("frames", Value::make_int(frames_));
    h.set("commands", Value::make_int(commands_));
    h.set("reconnects", Value::make_int(reconnects_));
    h.set("command_backoff_s", Value::make_number(cmd_backoff_.load()));
    Value last3 = Value::make_array({});
    const auto& ce = command_errors_.arr();
    for (std::size_t i = ce.size() > 3 ? ce.size() - 3 : 0; i < ce.size(); ++i) last3.push(ce[i]);
    h.set("command_errors", last3);
    return h;
}

double MarketFeed::wait_for_coverage(double fraction, double timeout, double poll) {
    const double deadline = clock_->monotonic() + timeout;
    while (clock_->monotonic() < deadline) {
        const double cov = health().find("coverage")->num();
        if (cov >= fraction) return cov;
        clock_->sleep(poll);
    }
    return health().find("coverage")->num();
}

std::optional<std::string> MarketFeed::pump_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pump_error_;
}
Value MarketFeed::command_errors() const {
    std::lock_guard<std::mutex> lk(mu_);
    return command_errors_;
}
Value MarketFeed::sids() const {
    std::lock_guard<std::mutex> lk(mu_);
    Value a = Value::make_array({});
    for (const auto& [k, v] : sids_) a.push(Value::make_array({k, v}));
    return a;
}
std::vector<std::string> MarketFeed::book_tickers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    for (const auto& kv : books_) out.push_back(kv.first);
    return out;
}

Value MarketFeed::valid_map() const {
    std::lock_guard<std::mutex> lk(mu_);
    Value a = Value::make_array({});
    for (const auto& [k, v] : valid_) a.push(Value::make_array({Value::make_string(k), Value::make_bool(v)}));
    return a;
}

}  // namespace kp::kalshi

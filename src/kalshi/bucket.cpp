#include "kp/kalshi/bucket.hpp"
#include "kp/massive/rest.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "kp/kalshi/error.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::kalshi {

bool is_write_method(const std::string& method) {
    std::string m;
    for (char ch : method) m.push_back((char)std::toupper((unsigned char)ch));
    return m == "POST" || m == "DELETE" || m == "PUT" || m == "PATCH";
}

double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

kp::json::Value BucketDetail::to_json() const {
    kp::json::Value v;
    v.set("available", kp::json::Value::make_number(available));
    v.set("capacity", kp::json::Value::make_number(capacity));
    v.set("refill_rate", kp::json::Value::make_number(refill_rate));
    v.set("spent", kp::json::Value::make_number(spent));
    v.set("denied", kp::json::Value::make_int(denied));
    return v;
}

// --- TokenBucket -----------------------------------------------------------------------------

TokenBucket::TokenBucket(double capacity, double refill_rate, Clock clock)
    : capacity_(capacity), refill_rate_(refill_rate), level_(capacity), t_(0.0),
      clock_(clock ? std::move(clock) : Clock(steady_seconds)) {
    t_ = clock_();
}

double TokenBucket::refill_locked(double now) {
    if (now > t_) {
        const double lv = level_ + (now - t_) * refill_rate_;
        level_ = (lv < capacity_) ? lv : capacity_;     // min(capacity, lv): lv only if strictly less
        t_ = now;
    }
    return level_;
}

double TokenBucket::available() {
    std::lock_guard<std::mutex> g(mu_);
    return refill_locked(clock_());
}

bool TokenBucket::can_afford(double tokens) { return available() >= tokens; }

double TokenBucket::debit(double tokens) {
    std::lock_guard<std::mutex> g(mu_);
    refill_locked(clock_());
    level_ -= tokens;
    spent_ += tokens;
    return level_;
}

void TokenBucket::resize(OptD capacity, OptD refill_rate) {
    std::lock_guard<std::mutex> g(mu_);
    refill_locked(clock_());
    if (capacity) {
        capacity_ = *capacity;
        level_ = (capacity_ < level_) ? capacity_ : level_;   // min(level, capacity)
    }
    if (refill_rate) refill_rate_ = *refill_rate;
}

double TokenBucket::penalise(double tokens) {
    {
        std::lock_guard<std::mutex> g(mu_);
        denied_ += 1;
    }
    return debit(tokens);
}

BucketDetail TokenBucket::detail() {
    BucketDetail d;
    d.available = kp::pycompat::py_round_nd(available(), 1);
    std::lock_guard<std::mutex> g(mu_);
    d.capacity = capacity_;
    d.refill_rate = refill_rate_;
    d.spent = kp::pycompat::py_round_nd(spent_, 1);
    d.denied = denied_;
    return d;
}

double TokenBucket::capacity() { std::lock_guard<std::mutex> g(mu_); return capacity_; }
double TokenBucket::refill_rate() { std::lock_guard<std::mutex> g(mu_); return refill_rate_; }
double TokenBucket::spent() { std::lock_guard<std::mutex> g(mu_); return spent_; }
long TokenBucket::denied() { std::lock_guard<std::mutex> g(mu_); return denied_; }

// --- formatting and arithmetic ---------------------------------------------------------------

std::string fp(double x, int places) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", places, x);
    return buf;
}

// CPython Objects/floatobject.c: _float_div_mod, then the floor. Kept literal so that a negative
// or fractional capacity lands where Python puts it.
double py_floordiv(double vx, double wx) {
    if (wx == 0.0) throw std::domain_error("float floor division by zero");
    double mod = std::fmod(vx, wx);
    double div = (vx - mod) / wx;
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) {
            mod += wx;
            div -= 1.0;
        }
    }
    double floordiv;
    if (div != 0.0) {
        floordiv = std::floor(div);
        if (div - floordiv > 0.5) floordiv += 1.0;
    } else {
        floordiv = std::copysign(0.0, vx / wx);
    }
    return floordiv;
}

long max_batch(OptD capacity, OptD refill_rate) {
    // figures = [f for f in (capacity, refill) if f] -- truthy: present and nonzero (NaN is truthy).
    bool any = false;
    double lo = 0.0;
    for (const OptD& f : {capacity, refill_rate}) {
        if (!f || *f == 0.0) continue;
        if (!any || *f < lo) lo = *f;   // min(): first wins on a tie, and on NaN the first stays
        any = true;
    }
    if (!any) return BATCH_FALLBACK;
    const double q = py_floordiv(lo, (double)ORDER_TOKEN_COST);
    if (!std::isfinite(q)) throw std::invalid_argument("cannot convert float to integer");
    const long n = (long)q;              // int() truncates; q is already integral
    return n > 1 ? n : 1;
}

kp::json::Value order_body(const std::string& ticker, const std::string& side,
                           const std::string& price, const std::string& count,
                           const std::string& client_order_id, const std::string& time_in_force,
                           bool post_only, const std::string& self_trade_prevention_type) {
    using kp::json::Value;
    Value b;
    b.set("ticker", Value::make_string(ticker));
    b.set("side", Value::make_string(side));
    b.set("count", Value::make_string(count));
    b.set("price", Value::make_string(price));
    b.set("time_in_force", Value::make_string(time_in_force));
    b.set("self_trade_prevention_type", Value::make_string(self_trade_prevention_type));
    b.set("post_only", Value::make_bool(post_only));
    if (!client_order_id.empty()) b.set("client_order_id", Value::make_string(client_order_id));
    return b;
}

namespace {
// `(d.get(k) or <dflt>)` on a JSON object that may itself be absent/falsy.
const kp::json::Value* get_truthy(const kp::json::Value* obj, const std::string& k) {
    if (obj == nullptr || !obj->is_object()) return nullptr;
    const kp::json::Value* v = obj->find(k);
    return (v != nullptr && py_truthy(*v)) ? v : nullptr;
}
}  // namespace

kp::json::Value write_budget_from_limits(const kp::json::Value& lim) {
    using kp::json::Value;
    const Value* w = get_truthy(&lim, "write");
    const Value* capv = get_truthy(w, "bucket_capacity");
    const Value* refv = get_truthy(w, "refill_rate");
    const double cap = capv ? py_float(*capv) : 0.0;
    const double refill = refv ? py_float(*refv) : 0.0;
    Value out;
    const Value* tier = lim.is_object() ? lim.find("usage_tier") : nullptr;
    out.set("usage_tier", tier ? *tier : Value());
    out.set("bucket_capacity", Value::make_number(cap));
    out.set("refill_rate", Value::make_number(refill));
    out.set("write_req_per_s", refill != 0.0 ? Value::make_number(refill / (double)ORDER_TOKEN_COST)
                                             : Value());
    out.set("max_batch", Value::make_int(max_batch(cap, refill)));
    return out;
}

std::string py_type_name(const std::exception& e) {
    if (const auto* n = dynamic_cast<const PyNamedError*>(&e)) return n->py_type;
    if (dynamic_cast<const KalshiError*>(&e) != nullptr) return "KalshiError";
    if (dynamic_cast<const kp::massive::MassiveHistoryError*>(&e) != nullptr) return "MassiveHistoryError";
    if (dynamic_cast<const kp::massive::MassiveError*>(&e) != nullptr) return "MassiveError";
    return "Exception";
}

std::string utf8_prefix(const std::string& s, std::size_t n) {
    std::size_t i = 0, cps = 0;
    while (i < s.size() && cps < n) {
        const unsigned char c = (unsigned char)s[i];
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
        ++cps;
    }
    return s.substr(0, i < s.size() ? i : s.size());
}

// --- Python float() and truthiness ----------------------------------------------------------

double py_float_str(const std::string& raw) {
    auto bad = [&]() -> double {
        throw std::invalid_argument("could not convert string to float: '" + raw + "'");
    };
    std::size_t a = 0, b = raw.size();
    while (a < b && std::isspace((unsigned char)raw[a])) ++a;
    while (b > a && std::isspace((unsigned char)raw[b - 1])) --b;
    std::string s = raw.substr(a, b - a);
    if (s.empty()) return bad();
    // Underscores are allowed only BETWEEN digits, and are then dropped.
    std::string t;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            if (i == 0 || i + 1 >= s.size() || !std::isdigit((unsigned char)s[i - 1])
                || !std::isdigit((unsigned char)s[i + 1]))
                return bad();
            continue;
        }
        t.push_back(s[i]);
    }
    std::size_t i = 0;
    std::string body = t;
    if (!body.empty() && (body[0] == '+' || body[0] == '-')) i = 1;
    std::string word;
    for (std::size_t k = i; k < body.size(); ++k) word.push_back((char)std::tolower((unsigned char)body[k]));
    if (word == "inf" || word == "infinity" || word == "nan") {
        const double v = (word == "nan") ? std::nan("") : HUGE_VAL;
        return (body[0] == '-') ? -v : v;
    }
    // [digits][.digits][e[+-]digits], at least one digit in the mantissa.
    std::size_t k = i, nd = 0;
    while (k < body.size() && std::isdigit((unsigned char)body[k])) { ++k; ++nd; }
    if (k < body.size() && body[k] == '.') {
        ++k;
        while (k < body.size() && std::isdigit((unsigned char)body[k])) { ++k; ++nd; }
    }
    if (nd == 0) return bad();
    if (k < body.size() && (body[k] == 'e' || body[k] == 'E')) {
        ++k;
        if (k < body.size() && (body[k] == '+' || body[k] == '-')) ++k;
        std::size_t ne = 0;
        while (k < body.size() && std::isdigit((unsigned char)body[k])) { ++k; ++ne; }
        if (ne == 0) return bad();
    }
    if (k != body.size()) return bad();
    errno = 0;
    return std::strtod(body.c_str(), nullptr);   // correctly rounded; overflow -> inf as Python
}

double py_float(const kp::json::Value& v) {
    switch (v.type()) {
        case kp::json::Value::Type::Number: return v.num();
        case kp::json::Value::Type::Bool: return v.boolean() ? 1.0 : 0.0;
        case kp::json::Value::Type::String: return py_float_str(v.str());
        default: throw std::invalid_argument("float() argument must be a string or a real number");
    }
}

bool py_truthy(const kp::json::Value& v) {
    switch (v.type()) {
        case kp::json::Value::Type::Null: return false;
        case kp::json::Value::Type::Bool: return v.boolean();
        case kp::json::Value::Type::Number: return v.is_int() ? v.integer() != 0 : v.num() != 0.0;
        case kp::json::Value::Type::String: return !v.str().empty();
        case kp::json::Value::Type::Array: return !v.arr().empty();
        case kp::json::Value::Type::Object: return !v.obj().empty();
    }
    return false;
}

}  // namespace kp::kalshi

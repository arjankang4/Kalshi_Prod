#include "kp/strategy/deltaadj.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "kp/pycompat/round.hpp"

namespace kp::strategy::deltaadj {

namespace json = kp::json;

namespace {

json::Value num(const OptD& v) { return v ? json::Value::make_number(*v) : json::Value(); }
json::Value ostr(const std::optional<std::string>& s) {
    return s ? json::Value::make_string(*s) : json::Value();
}
double py_max0(double x) { return x > 0.0 ? x : 0.0; }   // max(0.0, x)

}  // namespace

double system_wall() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
double system_mono() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

json::Value TickerDetail::to_json() const {
    json::Value d;
    d.set("product", json::Value::make_string(product));
    d.set("contract", ostr(contract));
    d.set("ok", json::Value::make_bool(ok));
    d.set("mid", num(mid));
    d.set("age_s", num(age_s));
    d.set("polls", json::Value::make_int(polls));
    d.set("errors", json::Value::make_int(errors));
    d.set("last_error", ostr(last_error));
    return d;
}

FuturesTicker::FuturesTicker(std::string underlying, Fetch fetch, double poll_s, Clock wall,
                             Clock mono)
    : underlying_(std::move(underlying)),
      product_(kp::massive::futures::product_for(underlying_)),
      poll_s_(poll_s),
      fetch_(std::move(fetch)),
      wall_(std::move(wall)),
      mono_(std::move(mono)) {}

FuturesTicker::~FuturesTicker() { stop(); }

void FuturesTicker::set_error(std::string e) {
    std::lock_guard<std::mutex> g(err_mu_);
    last_error_ = std::move(e);
}

std::optional<std::string> FuturesTicker::last_error() const {
    std::lock_guard<std::mutex> g(err_mu_);
    return last_error_;
}

FuturesTicker& FuturesTicker::start(const kp::massive::futures::Date& day,
                                    const FrontMonth& front_month) {
    try {
        // By VOLUME, not by calendar -- see massive::futures::front_month on the roll.
        ticker_ = front_month(product_, day);
    } catch (const FetchError& e) {
        set_error("front_month: " + e.type_name + ": " + std::string(e.what()).substr(0, 120));
    } catch (const std::exception& e) {
        set_error("front_month: Exception: " + std::string(e.what()).substr(0, 120));
    }
    if (!ticker_) {
        if (!last_error()) set_error("no front-month contract for " + product_);
        return *this;
    }
    {
        std::lock_guard<std::mutex> g(stop_mu_);
        stop_ = false;
    }
    alive_ = true;
    thread_ = std::thread([this] { run(); });
    return *this;
}

FuturesTicker& FuturesTicker::stop() {
    {
        std::lock_guard<std::mutex> g(stop_mu_);
        stop_ = true;
    }
    stop_cv_.notify_all();
    // The reference joins with a 2s timeout and abandons a daemon thread stuck in a request. A
    // std::thread cannot be abandoned safely while it touches *this, so this joins -- the injected
    // fetch is expected to carry its own timeout.
    if (thread_.joinable()) thread_.join();
    return *this;
}

bool FuturesTicker::ok() const { return ticker_ && !ticker_->empty() && alive_.load(); }

double FuturesTicker::poll_once() {
    const double t0 = mono_();
    try {
        // limit=1 and the vendor's own descending order give the newest quote in one row.
        const std::optional<FuturesRow> r = fetch_(*ticker_);
        if (r) {
            const OptD bid = r->bid_price, ask = r->ask_price;
            if (bid && *bid != 0.0 && ask && *ask != 0.0 && *ask >= *bid) {
                std::lock_guard<std::mutex> g(mu_);
                if (!r->timestamp) throw FetchError("KeyError", "'timestamp'");
                t_ns_ = *r->timestamp;
                mid_ = (*bid + *ask) / 2.0;
            }
        }
        polls_ += 1;
    } catch (const FetchError& e) {
        errors_ += 1;
        set_error(e.type_name + ": " + std::string(e.what()).substr(0, 120));
    } catch (const std::exception& e) {
        errors_ += 1;
        set_error("Exception: " + std::string(e.what()).substr(0, 120));
    }
    return py_max0(poll_s_ - (mono_() - t0));
}

void FuturesTicker::run() {
    for (;;) {
        {
            std::lock_guard<std::mutex> g(stop_mu_);
            if (stop_) break;
        }
        const double wait = poll_once();
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait_for(lk, std::chrono::duration<double>(wait), [this] { return stop_; });
    }
    alive_ = false;
}

MidAge FuturesTicker::mid(double max_age_s) const {
    std::optional<long long> t_ns;
    OptD m;
    {
        std::lock_guard<std::mutex> g(mu_);
        t_ns = t_ns_;
        m = mid_;
    }
    if (!m || !t_ns) return {std::nullopt, std::nullopt};
    const double age = wall_() - (double)*t_ns / 1e9;
    if (age < 0 || age > max_age_s) return {std::nullopt, age};
    return {m, age};
}

TickerDetail FuturesTicker::detail() const {
    const MidAge ma = mid();
    TickerDetail d;
    d.product = product_;
    d.contract = ticker_;
    d.ok = ok();
    d.mid = ma.mid;
    if (ma.age_s) d.age_s = kp::pycompat::py_round_nd(*ma.age_s, 3);
    d.polls = polls_.load();
    d.errors = errors_.load();
    d.last_error = last_error();
    return d;
}

// --- DeltaAdjuster ---------------------------------------------------------------------------

json::Value AdjDetail::to_json() const {
    json::Value d;
    d.set("delta_adj", json::Value::make_number(delta_adj));
    d.set("f_anchor", num(f_anchor));
    d.set("f_now", num(f_now));
    d.set("f_move", num(f_move));
    d.set("reason", ostr(reason));
    return d;
}

json::Value AdjusterDetail::to_json() const {
    json::Value d;
    d.set("adjusted", json::Value::make_int(adjusted));
    d.set("unadjusted", json::Value::make_int(unadjusted));
    d.set("refused_size", json::Value::make_int(refused_size));
    d.set("anchor", num(anchor));
    d.set("futures", futures ? futures->to_json() : json::Value());
    return d;
}

DeltaAdjuster::DeltaAdjuster(const FuturesTicker* ticker, double max_adj)
    : ticker_(ticker), max_adj_(max_adj) {}

OptD DeltaAdjuster::anchor_now() {
    if (ticker_) {
        const MidAge ma = ticker_->mid();
        anchor_ = ma.mid;
        anchor_age_s_ = ma.age_s;
    } else {
        anchor_.reset();
        anchor_age_s_.reset();
    }
    return anchor_;
}

std::pair<OptD, AdjDetail> DeltaAdjuster::adjust(OptD theo, OptD delta) {
    AdjDetail d;
    d.f_anchor = anchor_;
    if (!theo) {
        d.reason = "no_theo";
        return {theo, d};
    }
    if (!ticker_ || !anchor_) {
        d.reason = "no_anchor";
        unadjusted_ += 1;
        return {theo, d};
    }
    if (!delta) {
        // An unquotable market, or one inside MIN_MINUTES_TO_CLOSE: neither is carried forward.
        d.reason = "no_delta";
        unadjusted_ += 1;
        return {theo, d};
    }
    const MidAge now = ticker_->mid();
    if (!now.mid) {
        if (now.age_s) {
            char b[64];
            std::snprintf(b, sizeof b, "futures_stale_%.1fs", *now.age_s);
            d.reason = std::string(b);
        } else {
            d.reason = "no_futures_quote";
        }
        unadjusted_ += 1;
        return {theo, d};
    }
    const double move = *now.mid - *anchor_;
    const double adj = *delta * move;
    if (std::fabs(adj) > max_adj_) {
        // On a $1 binary this size of "repricing" is a broken increment. Refuse, never clamp.
        d.f_now = now.mid;
        d.f_move = move;
        d.reason = "adj_over_max";
        refused_size_ += 1;
        return {theo, d};
    }
    adjusted_ += 1;
    d.delta_adj = adj;
    d.f_now = now.mid;
    d.f_move = move;
    return {*theo + adj, d};
}

AdjusterDetail DeltaAdjuster::detail() const {
    AdjusterDetail d;
    d.adjusted = adjusted_;
    d.unadjusted = unadjusted_;
    d.refused_size = refused_size_;
    d.anchor = anchor_;
    if (ticker_) d.futures = ticker_->detail();
    return d;
}

}  // namespace kp::strategy::deltaadj

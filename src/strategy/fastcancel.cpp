#include "kp/strategy/fastcancel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include "kp/kalshi/bucket.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::strategy::fastcancel {

using kp::json::Value;
using orders::Order;

std::shared_ptr<DailyShift> DailyShift::copy_of(const kp::pricing::pricer::Pricer& p) {
    auto own = std::make_shared<kp::pricing::pricer::Pricer>(p);
    auto out = std::make_shared<DailyShift>(*own);
    out->own_ = std::move(own);
    return out;
}

std::shared_ptr<HourlyShift> HourlyShift::copy_of(const kp::pricing::hourly::HourlyPricer& p) {
    auto own = std::make_shared<kp::pricing::hourly::HourlyPricer>(p);
    auto out = std::make_shared<HourlyShift>(*own);
    out->own_ = std::move(own);
    return out;
}

kp::pycompat::ODict<std::string, double> reprice_shifted(ShiftablePricer* daily,
                                                         ShiftablePricer* hourly,
                                                         OptD base_daily_fwd, OptD base_hourly_fwd,
                                                         double move,
                                                         const std::vector<RepriceItem>& items) {
    // THE SMILE CURVE CARRIES ITS OWN FORWARD, and it has to move too (reference fix 2026-09-23).
    // With a non-flat curve installed -- the live case -- both pricers price the digital through
    // SkewFit::p_above_at, which reads the CURVE's forward, not Pricer.spot / fit.forward; moving
    // only those left the fair unchanged, so the guard and reprice-at-send were no-ops whenever a
    // smile was on. Each distinct curve is shifted once, and its projection memo swapped out.
    // (The reference's two pricers usually SHARE one curve object; here each holds its own copy,
    // and shifting each copy once by the same move is the same thing.)
    struct Curve { kp::pricing::skew::SkewFit* sf; OptD f0; kp::pricing::skew::SkewFit::ProjMemo proj; };
    std::vector<Curve> curves;
    for (ShiftablePricer* p : {daily, hourly}) {
        kp::pricing::skew::SkewFit* sf = p != nullptr ? p->curve() : nullptr;
        if (sf == nullptr || sf->flat() || !sf->forward() || *sf->forward() == 0.0) continue;
        bool seen = false;
        for (const auto& c : curves) seen = seen || c.sf == sf;
        if (!seen) curves.push_back({sf, sf->forward(), {}});
    }

    // The `finally`: restore both forwards and every curve however this scope is left.
    struct Restore {
        ShiftablePricer* d; ShiftablePricer* h; OptD bd, bh;
        std::vector<Curve>* curves;
        ~Restore() {
            if (d != nullptr && bd) d->set_forward(bd);
            if (h != nullptr && bh) h->set_forward(bh);
            for (auto& c : *curves) {
                c.sf->set_forward(c.f0);
                c.sf->put_proj(std::move(c.proj));
            }
        }
    } restore{daily, hourly, base_daily_fwd, base_hourly_fwd, &curves};

    kp::pycompat::ODict<std::string, double> out;
    if (daily != nullptr && base_daily_fwd) daily->set_forward(*base_daily_fwd + move);
    if (hourly != nullptr && base_hourly_fwd) hourly->set_forward(*base_hourly_fwd + move);
    for (auto& c : curves) {
        c.sf->set_forward(*c.f0 + move);
        c.proj = c.sf->take_proj();
    }
    for (const auto& [m, pricer] : items) {
        if (pricer == nullptr) throw std::runtime_error("reprice_shifted: no pricer for " + m->ticker);
        const OptD fair = pricer->market_fair(*m);
        if (fair) out.set(m->ticker, *fair);
    }
    return out;
}

// --- GuardDetail ------------------------------------------------------------------------------

Value GuardDetail::to_json() const {
    Value v;
    v.set("ok", Value::make_bool(ok));
    v.set("wakes", Value::make_int(wakes));
    v.set("repriced", Value::make_int(repriced));
    v.set("cancels", Value::make_int(cancels));
    v.set("errors", Value::make_int(errors));
    v.set("last_error", last_error ? Value::make_string(*last_error) : Value());
    Value b = Value::make_array({});
    for (const auto& s : boards) b.push(Value::make_string(s));
    v.set("boards", b);
    v.set("reprice_ms_p50", reprice_ms_p50 ? Value::make_number(*reprice_ms_p50) : Value());
    v.set("reprice_ms_p95", reprice_ms_p95 ? Value::make_number(*reprice_ms_p95) : Value());
    v.set("reprice_ms_max", Value::make_number(reprice_ms_max));
    return v;
}

// --- FastGuard --------------------------------------------------------------------------------

FastGuard::FastGuard(FuturesFeed& futures, std::map<std::string, SpotTracker*> trackers,
                     orders::RestingBook& book, kp::kalshi::Api* api, bool live,
                     std::shared_ptr<std::recursive_mutex> apply_lock, OptD interval_s,
                     OptD min_move, OnCancel on_cancel, long max_batch, kp::kalshi::Clock clock)
    : futures_(futures), trackers_(std::move(trackers)), book_(book), api_(api), live_(live),
      apply_lock_(apply_lock ? std::move(apply_lock) : std::make_shared<std::recursive_mutex>()),
      interval_s_(interval_s ? *interval_s : FAST_REPRICE_S),
      min_move_(min_move ? *min_move : MIN_MOVE_POINTS), on_cancel_(std::move(on_cancel)),
      max_batch_(max_batch),
      clock_(clock ? std::move(clock) : kp::kalshi::Clock(kp::kalshi::steady_seconds)) {
    if (live_ && api_ == nullptr) throw std::invalid_argument("FastGuard: live without an Api");
}

FastGuard::~FastGuard() { stop(); }

void FastGuard::publish(const std::string& underlying, std::shared_ptr<ShiftablePricer> daily,
                        std::shared_ptr<ShiftablePricer> hourly, const std::vector<Market>& daily_m,
                        const std::vector<Market>& hourly_m, OptD spot) {
    auto st = std::make_shared<BoardState>();
    st->underlying = underlying;
    st->daily = std::move(daily);
    st->hourly = std::move(hourly);
    for (const auto& m : daily_m) st->by_ticker[m.ticker] = {m, st->daily.get()};
    if (st->hourly)
        for (const auto& m : hourly_m) st->by_ticker[m.ticker] = {m, st->hourly.get()};
    st->base_spot = spot;
    st->base_daily_fwd = st->daily ? st->daily->forward() : std::nullopt;
    st->base_hourly_fwd = st->hourly ? st->hourly->forward() : std::nullopt;
    st->stamp = clock_();
    std::lock_guard<std::mutex> g(lock_);
    boards_.set(underlying, std::move(st));    // in place: a board keeps its position
}

void FastGuard::retire(const std::string& underlying) {
    std::lock_guard<std::mutex> g(lock_);
    boards_.erase(underlying);
}

FastGuard& FastGuard::start() {
    {
        std::lock_guard<std::mutex> g(stop_mu_);
        stop_ = false;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return *this;
}

FastGuard& FastGuard::stop() {
    {
        std::lock_guard<std::mutex> g(stop_mu_);
        stop_ = true;
    }
    stop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    return *this;
}

bool FastGuard::ok() const { return running_.load(); }

void FastGuard::run() {
    using clk = std::chrono::steady_clock;
    for (;;) {
        {
            std::lock_guard<std::mutex> g(stop_mu_);
            if (stop_) break;
        }
        const auto t0 = clk::now();
        try {
            wakes_ += 1;
            tick();
        } catch (const std::exception& e) {
            // A guard that dies silently is worse than no guard. Record and keep going.
            errors_ += 1;
            std::lock_guard<std::mutex> g(stats_);
            last_error_ = kp::kalshi::py_type_name(e) + ": " + kp::kalshi::utf8_prefix(e.what(), 120);
        } catch (...) {
            errors_ += 1;
            std::lock_guard<std::mutex> g(stats_);
            last_error_ = "Exception: ";
        }
        const double spent = std::chrono::duration<double>(clk::now() - t0).count();
        const double wait = std::max(0.0, interval_s_ - spent);
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait_for(lk, std::chrono::duration<double>(wait), [this] { return stop_; });
        if (stop_) break;
    }
    running_ = false;
}

void FastGuard::tick() {
    std::vector<std::shared_ptr<BoardState>> boards;
    {
        std::lock_guard<std::mutex> g(lock_);
        for (const auto& kv : boards_) boards.push_back(kv.second);
    }
    if (boards.empty()) return;
    std::vector<Order> doomed;
    for (const auto& st : boards) {
        if (!st->daily || !st->base_spot) continue;
        const auto it = trackers_.find(st->underlying);
        if (it == trackers_.end() || it->second == nullptr) continue;
        const OptD mid = futures_.mid(st->underlying).first;
        const OptD spot = it->second->spot(mid).first;
        if (!spot) continue;
        const double move = *spot - *st->base_spot;
        if (std::fabs(move) < min_move_) continue;
        for (auto& o : reprice_board(*st, move)) doomed.push_back(std::move(o));
    }
    if (!doomed.empty()) cancel(doomed);
}

std::vector<Order> FastGuard::reprice_board(BoardState& st, double move) {
    const auto t0 = std::chrono::steady_clock::now();
    // WALK THE BOOK, NOT THE BOARD: only resting markets can be cancelled.
    std::vector<RepriceItem> mine;
    {
        std::lock_guard<std::recursive_mutex> g(*apply_lock_);
        for (const auto& t : book_.resting_tickers()) {
            const auto it = st.by_ticker.find(t);
            if (it != st.by_ticker.end()) mine.emplace_back(&it->second.first, it->second.second);
        }
    }
    if (mine.empty()) return {};
    const auto fairs = reprice_shifted(st.daily.get(), st.hourly.get(), st.base_daily_fwd,
                                       st.base_hourly_fwd, move, mine);
    std::vector<Order> dead;
    for (const auto& [m, _p] : mine) {
        const double* f = fairs.find(m->ticker);
        for (auto& o : judge_at(*m, f ? OptD(*f) : std::nullopt)) dead.push_back(std::move(o));
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> g(stats_);
        max_reprice_ms_ = std::max(max_reprice_ms_, ms);
        reprice_ms_.push_back(ms);
        if (reprice_ms_.size() > 2000) reprice_ms_.erase(reprice_ms_.begin(), reprice_ms_.begin() + 1000);
    }
    repriced_ += 1;
    return dead;
}

std::vector<Order> FastGuard::judge_at(const Market& m, OptD fair) {
    if (!fair) return {};                       // cannot price it: not evidence that it is bad
    std::vector<Order> out;
    std::lock_guard<std::recursive_mutex> g(*apply_lock_);
    orders::MarketState& st = book_.state(m.ticker);
    for (const char* side : {"bid", "ask"}) {
        for (const auto& o : st.side(side)) {
            // THE SAME RULE plan_side uses: non-positive edge means we no longer believe it.
            const double p = o.price.value();
            const double edge = (std::string(side) == "bid") ? (*fair - p) : (p - *fair);
            if (edge <= 0) {
                Order c = o;
                c.side = side;
                c.ticker = m.ticker;
                c.reason = "fast_edge_negative";
                out.push_back(std::move(c));
            }
        }
    }
    return out;
}

void FastGuard::cancel(const std::vector<Order>& dead) {
    std::set<std::string> seen;
    std::vector<Order> uniq;
    for (const auto& c : dead) {
        if (!c.order_id || seen.count(*c.order_id)) continue;
        seen.insert(*c.order_id);
        uniq.push_back(c);
    }
    if (uniq.empty()) return;
    orders::ApplyResult res;
    {
        std::lock_guard<std::recursive_mutex> g(*apply_lock_);
        res = orders::apply({}, uniq, book_, api_, live_, max_batch_, nullptr, clock_());
    }
    cancels_ += res.cancelled + res.cancel_gone;
    if (on_cancel_) {
        try {
            on_cancel_(uniq, res);
        } catch (...) {
        }
    }
}

GuardDetail FastGuard::detail() const {
    GuardDetail d;
    d.ok = ok();
    d.wakes = wakes_;
    d.repriced = repriced_;
    d.cancels = cancels_;
    d.errors = errors_;
    {
        std::lock_guard<std::mutex> g(lock_);
        for (const auto& kv : boards_) d.boards.push_back(kv.first);
    }
    std::sort(d.boards.begin(), d.boards.end());
    std::lock_guard<std::mutex> g(stats_);
    d.last_error = last_error_;
    std::vector<double> ms = reprice_ms_;
    std::sort(ms.begin(), ms.end());
    if (!ms.empty()) {
        d.reprice_ms_p50 = kp::pycompat::py_round_nd(ms[ms.size() / 2], 3);
        d.reprice_ms_p95 = kp::pycompat::py_round_nd(ms[(std::size_t)(0.95 * (double)(ms.size() - 1))], 3);
    }
    d.reprice_ms_max = kp::pycompat::py_round_nd(max_reprice_ms_, 3);
    return d;
}

}  // namespace kp::strategy::fastcancel

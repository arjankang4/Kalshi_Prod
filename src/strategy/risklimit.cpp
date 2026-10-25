#include "kp/strategy/risklimit.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/strategy/journal.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <chrono>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::strategy::risklimit {

namespace {

std::string default_halt_path() {
    const char* env = std::getenv("KALSHI_ROOT");
    const std::string base = env ? env : KP_KALSHI_ROOT_DEFAULT;
    return base + "/" + kp::config::HALT_FILE_REL;
}

// Python's `f"{x:+.2f}"`.
std::string fmt_signed_2(double x) {
    char b[64];
    std::snprintf(b, sizeof b, "%+.2f", x);
    return std::string(b);
}

}  // namespace

SessionRisk::SessionRisk(OptD limit_dollars, std::optional<std::string> halt_path)
    : limit_(limit_dollars ? limit_dollars : OptD(kp::config::DAILY_LOSS_LIMIT_DOLLARS)),
      halt_path_(halt_path ? *halt_path : default_halt_path()) {}

long SessionRisk::on_fills(const std::vector<Fill>& fills) {
    long n_ok = 0;
    for (const auto& fl : fills) {
        // The reference's `_f(x, -1.0)` turns an absent or unparseable field into a sentinel that
        // the guard below rejects; absence is NOT zero-with-a-warning here.
        const double n = fl.count_fp.value_or(-1.0);
        const double p = fl.yes_price_dollars.value_or(-1.0);
        // `(outcome_side or side or "").lower()` -- an empty string is falsy in Python, so the
        // fallback fires on absent AND on empty.
        std::string side = !fl.outcome_side.empty() ? fl.outcome_side : fl.side;
        for (char& ch : side) ch = (char)std::tolower((unsigned char)ch);
        if (n <= 0.0 || p < 0.0 || (side != "yes" && side != "no")) {
            ++skipped_;
            continue;
        }
        cash_ += (side == "no") ? (n * p) : -(n * p);
        const double fee = fl.fee_cost.value_or(0.0);   // `_f(x)` defaults to 0.0 for the fee
        fees_ += fee;
        cash_ -= fee;
        contracts_ += n;
        ++fills_;
        ++n_ok;
    }
    return n_ok;
}

OptD SessionRisk::mark(const Positions& positions, const Fairs& fairs) {
    // Index the fairs once. The SUMMATION order below still follows `positions`, which is the
    // part that has to match Python bit-for-bit; a linear scan per position would preserve that
    // too but is quadratic, and the live board is ~3,290 markets.
    std::map<std::string, OptD> fair_by_ticker;
    for (const auto& [ft, fp] : fairs) fair_by_ticker[ft] = fp;   // last wins, as a dict does

    double value = 0.0;
    long unmarked = 0;
    for (const auto& [t, q] : positions) {
        if (q == 0.0) continue;                 // Python `if not q`
        OptD p;
        {
            const auto it = fair_by_ticker.find(t);
            if (it != fair_by_ticker.end()) p = it->second;
        }
        if (!p) {
            const auto it = last_fair_.find(t);
            if (it != last_fair_.end()) p = it->second;
        }
        if (!p) { ++unmarked; continue; }
        last_fair_[t] = *p;
        value += q * *p;
    }
    for (const auto& [t, p] : fairs) {
        if (p) last_fair_[t] = *p;
    }
    pnl_ = cash_ + value;
    ++marked_;
    if (*pnl_ < worst_) worst_ = *pnl_;
    unmarked_ = unmarked;
    has_unmarked_ = true;
    return pnl_;
}

std::string SessionRisk::halt_reason() {
    if (!halted_.empty()) return halted_;
    if (!halt_path_.empty()) {
        struct stat st{};
        if (::stat(halt_path_.c_str(), &st) == 0) {
            halted_ = "halt file present: " + halt_path_;
            return halted_;
        }
        // An unreadable halt path is not a reason to keep trading, but it is also not proof of an
        // operator decision. Say nothing and let the loss limit do its job.
    }
    if (stale_s_ && *stale_s_ > kp::config::RISK_ACCOUNT_STALE_HALT_S) {
        char b[160];
        std::snprintf(b, sizeof b, "account mark is %.0fs stale (> %gs) -- the loss limit is blind",
                      *stale_s_, kp::config::RISK_ACCOUNT_STALE_HALT_S);
        halted_ = b;
        return halted_;
    }
    if (limit_ && pnl_ && *pnl_ <= -std::fabs(*limit_)) {
        halted_ = "session loss " + fmt_signed_2(*pnl_) + " through the "
                + fmt_signed_2(-std::fabs(*limit_)) + " limit";
        return halted_;
    }
    return std::string();
}

RiskSummary SessionRisk::summary() const {
    RiskSummary s;
    s.fills = fills_;
    s.skipped_fills = skipped_;
    s.contracts = kp::pycompat::py_round_nd(contracts_, 2);
    s.cash = kp::pycompat::py_round_nd(cash_, 4);
    s.fees = kp::pycompat::py_round_nd(fees_, 4);
    s.pnl = pnl_ ? OptD(kp::pycompat::py_round_nd(*pnl_, 4)) : MISSING;
    s.worst = kp::pycompat::py_round_nd(worst_, 4);
    s.limit = limit_;
    s.halted = halted_;
    s.equity = equity_;
    s.opening_equity = base_;
    if (has_unmarked_) s.unmarked = unmarked_;
    s.mark_sources = mark_sources_;
    return s;
}

// --- the account mark ---------------------------------------------------------------------------
OptD SessionRisk::mark_account(const std::optional<AccountSnap>& snap, const Tops& books,
                               const std::map<std::string, OptD>* fairs, double now_mono) {
    if (!acct_init_) {
        acct_init_ = true;
        last_mark_.clear();
        unmarked_ = 0;
        has_unmarked_ = true;
        stale_s_.reset();
    }
    if (!snap) {
        stale_s_ = snap_t_ ? OptD(now_mono - *snap_t_) : std::nullopt;
        return std::nullopt;
    }
    snap_t_ = snap->t;
    stale_s_ = now_mono - snap->t;
    double value = 0.0;
    long unmarked = 0;
    std::vector<std::pair<std::string, long>> how;
    const auto bump = [&](const char* src) {
        for (auto& [k, n] : how) if (k == src) { ++n; return; }
        how.emplace_back(src, 1);
    };
    for (const auto& [tk, q] : snap->positions) {
        if (q == 0.0) continue;
        OptD bb, ba;
        if (const auto it = books.find(tk); it != books.end()) { bb = it->second.first; ba = it->second.second; }
        OptD m;
        const char* src = nullptr;
        if (bb && ba && 0 < *bb && *bb < *ba && *ba < 1) { m = (*bb + *ba) / 2.0; src = "mid"; }
        else if (q > 0 && bb && *bb > 0) { m = *bb; src = "bid"; }          // long YES, one-sided
        else if (q < 0 && ba && *ba < 1) { m = *ba; src = "ask"; }          // long NO, one-sided
        else if (const auto lm = last_mark_.find(tk); lm != last_mark_.end()) { m = lm->second; src = "last"; }
        else if (fairs) {
            if (const auto f = fairs->find(tk); f != fairs->end() && f->second) { m = *f->second; src = "theo"; }
        }
        if (!m) { ++unmarked; continue; }
        if (std::string(src) == "mid" || std::string(src) == "bid" || std::string(src) == "ask")
            last_mark_[tk] = *m;
        bump(src);
        value += q > 0 ? q * *m : (-q) * (1.0 - *m);
    }
    // An unmarked position counts at ZERO: its cost already left the balance, so this errs toward
    // a LOSS, the direction a risk control may be wrong in.
    const double equity = snap->balance + value;
    equity_ = equity;
    unmarked_ = unmarked;
    mark_sources_ = how;
    const double base = opening_equity(equity);
    pnl_ = equity - base;
    ++marked_;
    worst_ = std::min(worst_, *pnl_);
    return pnl_;
}

double SessionRisk::opening_equity(double equity_now) {
    if (base_) return *base_;
    const kp::time::Instant now = wall_ ? wall_() : kp::time::Instant{
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() / 1000 * 1000};
    const auto et = kp::time::to_et(now);
    char day[16];
    std::snprintf(day, sizeof day, "%04d-%02d-%02d", et.year, et.month, et.day);
    std::string dir = state_dir_;
    if (dir.empty()) {
        const char* env = std::getenv("KALSHI_ROOT");
        dir = std::string(env ? env : KP_KALSHI_ROOT_DEFAULT) + "/var/risk";
    }
    const std::string p = dir + "/opening_equity_" + day + ".json";
    // read: float(json.loads(p.read_text())["equity"]); OSError/ValueError/KeyError fall through
    bool got = false;
    {
        std::ifstream in(p);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            try {
                const kp::json::Value doc = kp::json::parse(ss.str());
                const kp::json::Value* e = doc.find("equity");
                if (e && e->is_number()) { base_ = e->num(); got = true; }
                else if (e && e->is_string()) { base_ = std::stod(e->str()); got = true; }
            } catch (const std::exception&) {
            }
        }
    }
    if (got) {
        base_source_ = p;
        return *base_;
    }
    base_ = equity_now;
    base_source_ = "this process (first mark of the day)";
    try {
        std::filesystem::create_directories(dir);
        kp::json::Value o = kp::json::Value::make_object({});
        o.set("equity", kp::json::Value::make_number(*base_));
        o.set("day", kp::json::Value::make_string(day));
        o.set("written_utc", kp::json::Value::make_string(kp::strategy::journal::isoformat_utc(now)));
        const std::string tmp = dir + "/opening_equity_" + day + ".tmp";
        {
            std::ofstream out(tmp);
            out << kp::json::dumps(o);
            if (!out) throw std::runtime_error("write");
        }
        std::filesystem::rename(tmp, p);
    } catch (const std::exception&) {
    }
    return *base_;
}

// --- AccountPoller --------------------------------------------------------------------------------
AccountPoller::AccountPoller(Balance balance, Positions positions, OptD interval_s,
                             std::function<double()> mono)
    : balance_(std::move(balance)), positions_(std::move(positions)),
      interval_s_(interval_s.value_or(kp::config::RISK_ACCOUNT_POLL_S)), mono_(std::move(mono)) {
    if (!mono_) mono_ = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
}

AccountPoller::~AccountPoller() { stop(); }

bool AccountPoller::poll_once() {
    double bal = 0.0;
    std::vector<std::pair<std::string, double>> pos;
    try {
        const kp::json::Value b = balance_();
        if (const kp::json::Value* bd = b.find("balance_dollars")) {
            bal = bd->is_number() ? bd->num() : std::stod(bd->str());
        } else {
            const kp::json::Value* c = b.find("balance");
            if (c == nullptr) throw kp::kalshi::PyNamedError("KeyError", "'balance'");
            bal = c->num() / 100.0;
        }
        const kp::json::Value p = positions_();
        for (const auto& [tk, v] : p.obj()) pos.emplace_back(tk, v.num());
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> g(mu_);
        ++errors_;
        last_error_ = kp::kalshi::py_type_name(e) + ": " + kp::kalshi::utf8_prefix(e.what(), 80);
        return false;
    }
    std::lock_guard<std::mutex> g(mu_);
    snap_ = AccountSnap{mono_(), bal, std::move(pos)};
    return true;
}

std::optional<AccountSnap> AccountPoller::latest() const {
    std::lock_guard<std::mutex> g(mu_);
    return snap_;
}

AccountPoller& AccountPoller::start() {
    poll_once();
    th_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(stop_mu_);
        while (!stop_cv_.wait_for(lk, std::chrono::duration<double>(interval_s_), [this] { return stop_; })) {
            lk.unlock();
            poll_once();
            lk.lock();
        }
    });
    return *this;
}

void AccountPoller::stop() {
    {
        std::lock_guard<std::mutex> g(stop_mu_);
        stop_ = true;
    }
    stop_cv_.notify_all();
    if (th_.joinable()) th_.join();
}

long AccountPoller::errors() const { std::lock_guard<std::mutex> g(mu_); return errors_; }
std::optional<std::string> AccountPoller::last_error() const {
    std::lock_guard<std::mutex> g(mu_);
    return last_error_;
}

}  // namespace kp::strategy::risklimit

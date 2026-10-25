// Session loss limit and halt switch -- the two controls this stack did not have.
//
// Port of Kalshi/strategy/risklimit.py.
//
// Every limit in config before this one bounds EXPOSURE. Not one of them bounds LOSS: a model
// wrong in a consistent direction fills inside every one of those caps, all day, and no code path
// in the reference would have noticed -- there was no P&L accumulator, no drawdown figure and no
// halt path anywhere before 2026-08-25.
//
// THE P&L IS MARK-TO-MARKET, NOT REALISED, and that is the point. A market maker's realised cash
// flow is meaningless mid-session: a book that has "made" $400 of realised spread while
// accumulating a position worth -$900 is losing.
//
//     pnl = cash + sum_over_markets(net_position * fair)
//
// Marking to our own model is self-referential and is still the honest choice: it is the number
// the position was taken at, and marking to the Kalshi mid instead would book the very
// disagreement we are being paid for as a loss the instant we fill.
//
// SIGN CONVENTION -- everything is in SIGNED YES CONTRACTS. A fill carries outcome_side, 'yes'
// for buy-yes and sell-no, 'no' for buy-no and sell-yes. Both sides print at the same
// yes_price_dollars, so 'yes' is `position += n, cash -= n*p` and 'no' is the mirror. The 'no'
// leg is written as "sold YES at p" rather than "bought NO at 1-p" ON PURPOSE: the two differ by
// a constant $1 per contract (the collateral returned at settlement) and only the first makes
// `cash + position*p` converge to the realised result as p goes to its 0/1 outcome. Getting this
// backwards produces a P&L that looks fine and is off by the size of the book.
//
// Neither control cancels anything itself. Both set the loop's stop flag and let the existing
// teardown run cancel_all, the one kill path that verifies by re-reading.
#pragma once
#include "kp/time/et.hpp"
#include "kp/json/value.hpp"
#include <condition_variable>
#include <thread>
#include <mutex>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kp::strategy::risklimit {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// One `fill` frame, carrying only what the accumulator reads. Kalshi sends counts and prices as
// fixed-point STRINGS; absence here is genuine absence, and is counted in `skipped` rather than
// read as zero -- an unparseable field means we do not know what happened, and this module's job
// is to be conservative.
struct Fill {
    OptD count_fp, yes_price_dollars, fee_cost;
    // The reference reads `(outcome_side or side or "").lower()`. Both spellings are carried
    // because both appear -- the WS fill frame uses outcome_side and the REST fill row uses
    // side -- and the CASE FOLD is load-bearing: a frame that says "YES" must not be skipped as
    // an unknown side, which would silently drop the fill from the P&L instead of failing.
    std::string outcome_side, side;
};

// {ticker: signed yes contracts}, AS AN ORDERED SEQUENCE.
//
// The order is load-bearing and this is not a stylistic choice: `mark` sums `q * p` across the
// book in iteration order, and floating-point addition is not associative, so a std::map's sorted
// order would give a different last bit from the Python dict's insertion order. The caller must
// pass the feed's insertion order. Making that explicit in the type is the only way it survives
// contact with a future refactor.
using Positions = std::vector<std::pair<std::string, double>>;
using Fairs = std::vector<std::pair<std::string, OptD>>;

struct RiskSummary {
    long fills = 0, skipped_fills = 0;
    double contracts = 0.0, cash = 0.0, fees = 0.0;
    OptD pnl;
    double worst = 0.0;
    OptD limit;
    std::string halted;            // empty = Python None
    // The account mark (2026-09-23). Absent until mark_account has run, as the reference's
    // getattr(self, ..., None).
    OptD equity, opening_equity;
    std::optional<long> unmarked;
    std::optional<std::vector<std::pair<std::string, long>>> mark_sources;   // first-seen order
};

// AccountPoller.latest(): (monotonic time of the read, cash balance in dollars, {ticker: signed
// yes contracts}) -- both read in the same poll so they describe the same instant.
struct AccountSnap {
    double t = 0.0;
    double balance = 0.0;
    std::vector<std::pair<std::string, double>> positions;   // API order
};
// {ticker: (best_bid, best_ask)} in YES dollars, for the markets we hold.
using Tops = std::map<std::string, std::pair<OptD, OptD>>;

// Cash from fills, marked against live positions and fairs, plus the halt file.
//
// Pure of I/O except the halt-file stat. Nothing here sends, cancels or prices.
class SessionRisk {
public:
    // An absent `limit_dollars` means "use the configured limit", NOT "no limit" -- there is
    // deliberately no way to construct an unlimited one. The argument exists so a gate can pin
    // the arithmetic at a convenient number, and the one direction a risk control must never be
    // wrong in is the direction where a caller that forgot to pass anything gets no protection.
    explicit SessionRisk(OptD limit_dollars = MISSING,
                         std::optional<std::string> halt_path = std::nullopt);

    // Fold a batch of fill frames into cash. Returns the number applied.
    //
    // Fees are subtracted whatever the schedule says. The maker fee was measured at zero on a
    // real fill, but a fee model is exactly the thing that changes without asking, and a loss
    // limit that trusts a constant is not a loss limit.
    long on_fills(const std::vector<Fill>& fills);

    // Re-mark the book.
    //
    // A position in a market we could not price this cycle is marked at its LAST known fair if we
    // have one, and otherwise counted in `unmarked` rather than dropped. Dropping it would make
    // the P&L improve every time the pricer failed, which is the direction a risk control must
    // never be wrong in.
    OptD mark(const Positions& positions, const Fairs& fairs);

    // THE LOSS LIMIT'S MARK since 2026-09-23: ACCOUNT equity marked to the Kalshi mid, against the
    // day's opening equity (persisted, so a restart does not reset it):
    //     equity = balance + sum_{q>0} q * mid  +  sum_{q<0} |q| * (1 - mid)
    // The fill-cash mark above read +$1,744.50 on a -$899.05 day: cash starts at 0 in every
    // process while positions are seeded, hourly settlements pay out with no fill frame, and
    // marking to our own theo booked our disagreement with the market as profit.
    //
    // A missing snapshot leaves `pnl` alone and ages `stale_s`; halt_reason() stops the loop once
    // that passes RISK_ACCOUNT_STALE_HALT_S. Mark per position: two-sided mid, else the side we
    // could trade out at, else the last good mark, else our theo; unmarkable counts at ZERO (its
    // cost already left the balance -- the loss direction). Returns the pnl, or absent.
    OptD mark_account(const std::optional<AccountSnap>& snap, const Tops& books,
                      const std::map<std::string, OptD>* fairs, double now_mono);

    // The day's first marked equity, persisted to <state_dir>/opening_equity_<ET date>.json.
    double opening_equity(double equity_now);

    // Where opening equity persists (config.RISK_STATE_DIR), and the wall clock that names the
    // day. Defaults: <KALSHI_ROOT>/var/risk and the system clock.
    void set_state_dir(std::string d) { state_dir_ = std::move(d); }
    void set_wall_clock(std::function<kp::time::Instant()> c) { wall_ = std::move(c); }
    OptD stale_s() const { return stale_s_; }

    // The reason to stop, or empty. STICKY: once tripped it stays tripped.
    //
    // Checked before pricing, so a halt works on a cycle the feed gate would have skipped.
    std::string halt_reason();

    RiskSummary summary() const;

    double cash() const { return cash_; }
    double fees() const { return fees_; }
    long fills() const { return fills_; }
    long skipped() const { return skipped_; }
    double contracts() const { return contracts_; }
    OptD pnl() const { return pnl_; }
    long marked() const { return marked_; }
    double worst() const { return worst_; }
    long unmarked() const { return unmarked_; }
    OptD limit() const { return limit_; }
    const std::string& halt_path() const { return halt_path_; }

private:
    OptD limit_;
    std::string halt_path_;
    double cash_ = 0.0, fees_ = 0.0, contracts_ = 0.0, worst_ = 0.0;
    long fills_ = 0, skipped_ = 0, marked_ = 0, unmarked_ = 0;
    OptD pnl_;                     // absent until the first mark -- NOT 0.0, which reads as flat
    std::string halted_;
    std::map<std::string, double> last_fair_;
    // the account mark
    bool acct_init_ = false, has_unmarked_ = false;
    std::map<std::string, double> last_mark_;
    OptD stale_s_, snap_t_, equity_, base_;
    std::optional<std::vector<std::pair<std::string, long>>> mark_sources_;
    std::string state_dir_, base_source_;
    std::function<kp::time::Instant()> wall_;
};

// Reads the balance and every open position from REST every RISK_ACCOUNT_POLL_S seconds, in one
// poll. A failed read keeps the last good snapshot and lets it age -- SessionRisk acts on the age.
// The two reads are injected (the real ones are kalshi::Client::balance / open_positions(strict)).
class AccountPoller {
public:
    using Balance = std::function<kp::json::Value()>;
    using Positions = std::function<kp::json::Value()>;       // {ticker: float}; throws on failure
    AccountPoller(Balance balance, Positions positions, OptD interval_s = std::nullopt,
                  std::function<double()> mono = nullptr);
    ~AccountPoller();
    AccountPoller(const AccountPoller&) = delete;
    AccountPoller& operator=(const AccountPoller&) = delete;

    bool poll_once();
    std::optional<AccountSnap> latest() const;
    AccountPoller& start();                  // one synchronous poll, then the thread
    void stop();
    double interval_s() const { return interval_s_; }
    long errors() const;
    std::optional<std::string> last_error() const;

private:
    Balance balance_;
    Positions positions_;
    double interval_s_;
    std::function<double()> mono_;
    mutable std::mutex mu_;
    std::optional<AccountSnap> snap_;
    long errors_ = 0;
    std::optional<std::string> last_error_;
    std::mutex stop_mu_;
    std::condition_variable stop_cv_;
    bool stop_ = false;
    std::thread th_;
};

}  // namespace kp::strategy::risklimit

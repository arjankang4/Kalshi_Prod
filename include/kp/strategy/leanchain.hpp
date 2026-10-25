// The realized-vol shadow probability chain the lean reads.
//
// Port of Kalshi/strategy/leanchain.py.
//
// Two probability chains over the same board: p_impl(K), the theo (the fitted smile's -dC/dK,
// scaled through the variance calendar on the hourly boards), and p_rv(K), the same strikes
// priced off the realized-vol model's sigma. pricing::lean::edge_adjust turns the disagreement
// into a per-side EDGE adjustment; this module only produces p_rv, p_ref and the two sigmas, and
// fails soft: any missing piece yields a row with p_rv = None and a `reason`.
//
// THREE RULES (2026-08-25), each pinned by the suite:
//   1. The 4pm board never gets a lean ("4pm_board_implied_only"): its implied vol is the
//      market's own terminal distribution and no fitted horizon reaches it.
//   2. p_rv goes through the SMILE (the same SkewFit the pricer installed), at each market's OWN
//      settlement -- w_impl = w_impl_4pm * sqrt(remaining_ratio(now, settlement)).
//   3. The reference is RAW implied, no variance-risk-premium centring. Uncentred, this is a
//      short-vol premium harvest, not a forecasting claim.
//
// COMPUTED EVEN WHEN DISABLED: `enabled` gates p_rv, not p_rv_shadow.
//
// HOW run_04 DRIVES IT (the integrator's contract):
//   startup   LeanChain lc(U, fetch); lc.start(now);   -- synchronous in the reference: one
//             backfill (two REST fetches) on the startup thread; there is no background thread
//             in leanchain.py or rvol/live.py, so none is added here.
//   cycle     if (lc.ok()) { lc.observe(index_quote_or_null);
//                            map = lc.chain(markets, quotes, strikes, now, mono_s, spot, {},
//                                           pricer_skew_or_null); }
//             inside a catch-all that logs and KEEPS the previous map (run_04 does that).
//   readers   build_desired: rows[tk].p_ref / p_rv -> edge_adjust.
//             journal:       p_rv, p_rv_shadow, p_ref ("p_impl_ref"), sigma_rv, rv_horizon, reason.
//             risktape:      the FIRST entry of the Python dict with a truthy sigma_rv -- and the
//                            dict's first entry is `_meta`, so that is meta.sigma_rv (the session
//                            forecast to 16:00) whenever it exists, else the first row's. See
//                            Chain::first_sigma_rv().
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kp/massive/index.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/rvol/live.hpp"
#include "kp/rvol/model.hpp"
#include "kp/time/et.hpp"

namespace kp::strategy::leanchain {

using OptD = std::optional<double>;
using OptS = std::optional<std::string>;

inline constexpr int CLOSE_H = 16, CLOSE_M = 0;
inline constexpr double TRADING_DAYS = 252.0;

// The `detail` dict of sigmas(), plus chain()'s `enabled`/`smiled`: out["_meta"]. Fields the
// reference had not set yet on an early return are None.
struct Meta {
    OptS lean_reason;
    double buffer_span_s = 0.0;
    OptD variance_left, w_impl, sigma_impl, forward;
    // {k: None if v is None or NaN else round(v, 6)} over the f_* and pf_* features, in order.
    std::optional<std::vector<std::pair<std::string, OptD>>> rv_features;
    OptS rv_horizon;
    OptD minutes_left, sigma_rv;
    bool enabled = false, smiled = false;
};

// out[ticker]. `reason` empty == None. w_impl / rv_horizon / minutes are None until the row
// got as far as a prediction (the reference adds those keys only then).
struct Row {
    OptD p_rv, p_rv_shadow, p_ref, sigma_rv, sigma_impl, w_rv, w_impl;
    bool enabled = false;
    std::string reason;
    OptS rv_horizon;
    OptD minutes;              // round(minutes, 1)
};

struct Chain {
    Meta meta;                                        // out["_meta"], the dict's first entry
    kp::pycompat::ODict<std::string, Row> rows;       // then one per market, in market order
    // next((r for r in out.values() if r.get("sigma_rv")), {}).get("sigma_rv")
    OptD first_sigma_rv() const;
};

struct Sigmas { OptD sigma_rv, sigma_impl; Meta detail; };

class LeanChain {
public:
    // Loads the variance calendar and the rvol model. A failure leaves ok() false with a reason
    // and quoting unleaned -- never an exception.
    LeanChain(std::string underlying, kp::rvol::live::Fetch fetch, std::string root = "");

    bool ok() const { return ok_; }
    const std::string& reason() const { return reason_; }   // empty == None

    // Backfill the buffer (see header note: synchronous, as in the reference). True when the
    // chain is usable AND the backfill succeeded.
    bool start(const kp::time::Instant& now);

    // Append one polled index level (nullptr == no quote this cycle).
    void observe(const kp::massive::index::IndexQuote* index_quote);

    // (sigma_rv to 16:00, sigma_impl, detail). Either sigma may be None.
    Sigmas sigmas(const kp::time::Instant& now, double mono_s, const kp::pricing::skew::Board& quotes,
                  const std::vector<double>& strikes);

    // {ticker: row} for every market. `forward` is run_04's spot; `enabled` absent means
    // config::LEAN_ENABLED; `skew_fit` null means flat Black-Scholes.
    Chain chain(const std::vector<kp::pricing::theo::Market>& markets,
                const kp::pricing::skew::Board& quotes, const std::vector<double>& strikes,
                const kp::time::Instant& now, double mono_s, OptD forward,
                std::optional<bool> enabled = std::nullopt,
                const kp::pricing::skew::SkewFit* skew_fit = nullptr);

    const std::string& underlying() const { return underlying_; }
    const kp::pricing::varcal::VarianceCalendar& calendar() const { return *cal_; }
    const kp::rvol::model::RealizedVolModel& model() const { return *model_; }
    kp::rvol::live::IndexBuffer& buffer() { return *buf_; }
    kp::rvol::live::LiveRealizedVol& rv() { return *rv_; }

private:
    std::string underlying_, root_;
    kp::rvol::live::Fetch fetch_;
    bool ok_ = false;
    std::string reason_;
    std::shared_ptr<const kp::pricing::varcal::VarianceCalendar> cal_;
    std::optional<kp::rvol::model::RealizedVolModel> model_;
    std::shared_ptr<kp::rvol::live::IndexBuffer> buf_;
    std::optional<kp::rvol::live::LiveRealizedVol> rv_;
};

}  // namespace kp::strategy::leanchain

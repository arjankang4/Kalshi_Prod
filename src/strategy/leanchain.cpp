#include "kp/strategy/leanchain.hpp"

#include <cmath>
#include <cstdio>
#include <exception>

#include "kp/config.hpp"
#include "kp/pricing/atmvol.hpp"
#include "kp/pricing/bs.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/rvol/features.hpp"
#include "kp/strategy/edgemgr.hpp"

namespace kp::strategy::leanchain {

namespace {
// Python max(1.0, x): x only if strictly greater (NaN -> 1.0).
double max1(double x) { return x > 1.0 ? x : 1.0; }
}  // namespace

OptD Chain::first_sigma_rv() const {
    // A dict value's .get("sigma_rv") is truthy when present, not None and non-zero.
    if (meta.sigma_rv && *meta.sigma_rv != 0.0) return meta.sigma_rv;
    for (const auto& [tk, r] : rows)
        if (r.sigma_rv && *r.sigma_rv != 0.0) return r.sigma_rv;
    return std::nullopt;
}

LeanChain::LeanChain(std::string underlying, kp::rvol::live::Fetch fetch, std::string root)
    : underlying_(std::move(underlying)), root_(std::move(root)), fetch_(std::move(fetch)) {
    try {
        cal_ = std::make_shared<const kp::pricing::varcal::VarianceCalendar>(
            kp::pricing::varcal::VarianceCalendar::load(underlying_, "default", root_));
        model_.emplace(kp::rvol::model::RealizedVolModel::load(underlying_, root_));
        buf_ = std::make_shared<kp::rvol::live::IndexBuffer>(underlying_);
        rv_.emplace(underlying_, cal_, buf_, root_);
        ok_ = true;
    } catch (const std::exception& e) {
        // The reference formats f"{type(e).__name__}: {str(e)[:80]}". C++ exceptions carry no
        // Python type name, so the text differs; it is diagnostic only (log + _meta).
        reason_ = std::string(e.what()).substr(0, 100);
        std::printf("[lean] unavailable (%s) -- quoting unleaned\n", reason_.c_str());
        std::fflush(stdout);
    }
}

bool LeanChain::start(const kp::time::Instant& now) {
    if (ok_) buf_->backfill(now, fetch_);
    return ok_ && buf_->backfilled();
}

void LeanChain::observe(const kp::massive::index::IndexQuote* index_quote) {
    if (ok_) buf_->append(index_quote);
}

Sigmas LeanChain::sigmas(const kp::time::Instant& now, double mono_s,
                         const kp::pricing::skew::Board& quotes,
                         const std::vector<double>& strikes) {
    Sigmas s;
    Meta& d = s.detail;
    d.buffer_span_s = ok_ ? buf_->span_s() : 0.0;
    if (!ok_) {
        d.lean_reason = reason_.empty() ? std::string("unavailable") : reason_;
        return s;
    }
    const double left = 1.0 - cal_->phi(now);
    d.variance_left = left;
    if (left <= 1e-6) {
        d.lean_reason = "no_variance_left";
        return s;
    }
    const auto fit = kp::pricing::atmvol::fit(quotes, strikes);
    if (!fit.ok()) {
        d.lean_reason = "atm_" + (fit.reason.empty() ? std::string("unfit") : fit.reason);
        return s;
    }
    // Same clock both sides: one RTH session is one variance-day, 252 a year.
    const double sigma_impl = *fit.w / std::sqrt(left / TRADING_DAYS);
    d.w_impl = fit.w;
    d.sigma_impl = sigma_impl;
    d.forward = fit.forward;
    s.sigma_impl = sigma_impl;

    const auto feats = rv_->features(now, mono_s);
    if (!feats || feats->empty()) {
        d.lean_reason = "no_rv_features";
        return s;
    }
    std::vector<std::pair<std::string, OptD>> shown;
    for (const auto& [k, v] : *feats) {
        if (k.rfind("f_", 0) != 0 && k.rfind("pf_", 0) != 0) continue;
        shown.emplace_back(k, (!v || std::isnan(*v)) ? OptD()
                                                     : OptD(kp::pycompat::py_round_nd(*v, 6)));
    }
    d.rv_features = std::move(shown);

    const kp::time::Instant close = kp::time::et_same_day(now, CLOSE_H, CLOSE_M);
    const double minutes_left = max1(kp::rvol::features::wall_seconds(now, close) / 60.0);
    const auto [sigma_rv, horizon] = model_->predict_for_minutes(minutes_left, *feats);
    d.rv_horizon = horizon;
    d.minutes_left = minutes_left;
    if (!sigma_rv) {
        d.lean_reason = "rv_predict_none";
        return s;
    }
    d.sigma_rv = sigma_rv;
    s.sigma_rv = sigma_rv;
    return s;
}

Chain LeanChain::chain(const std::vector<kp::pricing::theo::Market>& markets,
                       const kp::pricing::skew::Board& quotes, const std::vector<double>& strikes,
                       const kp::time::Instant& now, double mono_s, OptD forward,
                       std::optional<bool> enabled_opt,
                       const kp::pricing::skew::SkewFit* skew_fit) {
    const bool enabled = enabled_opt ? *enabled_opt : kp::config::LEAN_ENABLED;
    Sigmas sg = sigmas(now, mono_s, quotes, strikes);
    Chain out;
    out.meta = sg.detail;
    out.meta.enabled = enabled;
    out.meta.smiled = skew_fit != nullptr && skew_fit->ok();
    if (!sg.sigma_impl || !forward) return out;
    const double sigma_impl = *sg.sigma_impl;

    // The second read is inside the recompute window and returns the same cached features.
    std::optional<kp::rvol::model::Features> feats;
    if (ok_) feats = rv_->features(now, mono_s);
    const OptD w_impl_4pm = sg.detail.w_impl;

    auto p_of = [&](double k, OptD w) -> OptD {
        if (!w || *w <= 0) return std::nullopt;
        if (skew_fit != nullptr && skew_fit->ok() && !skew_fit->flat())
            return skew_fit->p_above_at(k, *w);
        return kp::pricing::bs::p_above(*forward, k, *w);
    };

    for (const auto& m : markets) {
        Row row;
        row.sigma_impl = sigma_impl;
        row.enabled = enabled;
        const auto settlement = kp::strategy::edgemgr::settlement_of(m);
        if (!settlement) {
            row.reason = "no_settlement";
            out.rows.set(m.ticker, row);
            continue;
        }
        if (kp::time::to_et(*settlement).hour >= CLOSE_H) {
            // Rule 1. Not an error -- the 4pm board is quoted on implied vol by design.
            row.reason = "4pm_board_implied_only";
            out.rows.set(m.ticker, row);
            continue;
        }
        const OptD ratio = cal_->remaining_ratio(now, *settlement);
        const bool no_ratio = !ratio || *ratio == 0.0;              // `not ratio`
        if (no_ratio || !feats || !w_impl_4pm) {
            row.reason = no_ratio ? "no_variance_left" : !feats ? "no_rv_features" : "no_w_impl";
            out.rows.set(m.ticker, row);
            continue;
        }
        const double minutes = max1(kp::time::total_seconds(now, *settlement) / 60.0);
        const auto [sigma_rv, horizon] = model_->predict_for_minutes(minutes, *feats);
        if (!sigma_rv) {
            row.reason = "rv_predict_none";
            out.rows.set(m.ticker, row);
            continue;
        }
        const double w_impl = *w_impl_4pm * std::sqrt(*ratio);
        const OptD w_rv = sigma_impl != 0.0 ? OptD(w_impl * (*sigma_rv / sigma_impl)) : OptD();
        OptD k;
        if (m.strike_type != "between") k = m.floor_strike;
        const OptD p_rv = k ? p_of(*k, w_rv) : OptD();
        // The reference is the IMPLIED chain itself. No premium centring -- see rule 3.
        const OptD p_ref = k ? p_of(*k, w_impl) : OptD();
        row.p_rv_shadow = p_rv;
        row.p_ref = p_ref;
        row.sigma_rv = sigma_rv;
        row.w_rv = w_rv;
        row.w_impl = w_impl;
        row.rv_horizon = horizon;
        row.minutes = kp::pycompat::py_round_nd(minutes, 1);
        row.p_rv = (enabled && p_ref) ? p_rv : OptD();
        out.rows.set(m.ticker, row);
    }
    return out;
}

}  // namespace kp::strategy::leanchain

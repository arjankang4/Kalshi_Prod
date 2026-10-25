#include "kp/pricing/hourly.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "kp/pricing/bs.hpp"

namespace kp::pricing::hourly {

namespace {

// Python's `x ** 0.5`, which is pow and NOT sqrt -- they differ on ~0.086% of values, and both
// call sites here feed a vol that feeds every digital on the board. Per-site, never global.
inline double pow_half(double x) { return std::pow(x, 0.5); }

}  // namespace

std::optional<kp::time::Instant> parse_settlement(const theo::Market& m) {
    if (!m.close_time_s) return std::nullopt;
    return kp::time::Instant{*m.close_time_s * 1000000000LL};
}

kp::time::Instant expiry_4pm(const kp::time::Instant& now) {
    return kp::time::et_same_day(now, CLOSE_HOUR, CLOSE_MINUTE);
}

std::string isoformat_utc(const kp::time::Instant& t) {
    // Floor-divide, so a pre-epoch instant does not round toward zero and land a second late.
    std::int64_t secs = t.ns / 1000000000LL;
    if (t.ns % 1000000000LL != 0 && t.ns < 0) --secs;
    const std::time_t tt = (std::time_t)secs;
    std::tm g{};
    gmtime_r(&tt, &g);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d+00:00",
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return std::string(buf);
}

HourlyPricer::HourlyPricer(Board quotes, std::vector<double> strikes, kp::time::Instant now,
                           const varcal::VarianceCalendar& calendar,
                           std::optional<atmvol::AtmFit> fit, OptD index_forward,
                           std::optional<skew::SkewFit> skew_fit)
    : quotes_(std::move(quotes)),
      strikes_(std::move(strikes)),
      now_(now),
      calendar_(&calendar),
      skew_(std::move(skew_fit)) {
    std::stable_sort(strikes_.begin(), strikes_.end());
    fit_ = fit ? *fit : atmvol::fit(quotes_, strikes_, /*gate=*/true, index_forward);
    expiry_4pm_ = expiry_4pm(now_);
    skew_built_ = skew_.has_value();
}

HourlyPricer& HourlyPricer::use_skew(const skew::SkewFit& f) {
    skew_ = f;
    skew_built_ = true;
    return *this;
}

const skew::SkewFit* HourlyPricer::skew() const {
    if (!skew_built_) {
        if (fit_.ok()) skew_ = skew::fit(quotes_, strikes_, fit_.forward, fit_.w);
        skew_built_ = true;
    }
    return skew_ ? &*skew_ : nullptr;
}

OptD HourlyPricer::sigma_annualized() const {
    if (!fit_.ok()) return MISSING;
    const double left = 1.0 - calendar_->phi(now_);
    if (left <= MIN_VARIANCE_LEFT) return MISSING;
    // Divide first, then pow -- the reference is `(left / 252) ** 0.5`, and reassociating it
    // would move the last bit.
    return *fit_.w / pow_half(left / (double)TRADING_DAYS_PER_YEAR);
}

const HorizonVol& HourlyPricer::total_vol_to(
        const std::optional<kp::time::Instant>& settlement) const {
    const std::optional<std::int64_t> key =
        settlement ? std::optional<std::int64_t>(settlement->ns) : std::nullopt;
    auto it = w_cache_.find(key);
    if (it != w_cache_.end()) return it->second;

    HorizonVol out;
    if (!fit_.ok()) {
        out.reason = fit_.reason.empty() ? "no_atm_fit" : fit_.reason;
    } else if (!settlement) {
        out.reason = "no_close_time";
    } else if (settlement->ns <= now_.ns) {
        out.reason = "expired";
    } else if (1.0 - calendar_->phi(now_) <= MIN_VARIANCE_LEFT) {
        out.reason = "no_variance_left";
    } else {
        const OptD ratio = calendar_->remaining_ratio(now_, *settlement);
        if (!ratio) {
            out.reason = "no_variance_left";
        } else if (*ratio <= 0.0) {
            out.reason = "no_variance_in_window";
        } else {
            // w scales as the square root of variance, and the ratio is a variance ratio.
            //
            // Nothing here is annualised. The 4pm vol already prices only the variance from now
            // to 16:00, so the overnight gap is outside both the numerator and the denominator
            // and no intraday fraction has to be applied -- or double-applied. At
            // settlement == 16:00 the ratio is exactly 1 and this returns w_4pm untouched, which
            // is the invariant that ties this model to the validated daily one.
            out.w = *fit_.w * pow_half(*ratio);
            out.ratio = ratio;
        }
    }
    return w_cache_.emplace(key, std::move(out)).first->second;
}

std::function<OptD(double)> HourlyPricer::p_above_for(
        const std::optional<kp::time::Instant>& settlement) const {
    const HorizonVol& hv = total_vol_to(settlement);
    if (!hv.w) return [](double) { return MISSING; };
    const double w = *hv.w;
    const double F = *fit_.forward;
    const skew::SkewFit* sf = skew();
    if (sf == nullptr || sf->flat()) {
        return [F, w](double k) { return bs::p_above(F, k, w); };
    }
    // p_above_at, not bs::p_above(F, k, w * ratio(k)): the digital is -dC/dK, so a
    // strike-dependent vol contributes -F*phi(d1)*dw/dK on top of N(d2). The ratio is still read
    // in 16:00 sigmas (see the header) but the vol it multiplies is the HORIZON's, so the term
    // scales with w, and w_atm=w is what carries that through.
    return [sf, w](double k) { return sf->p_above_at(k, w); };
}

std::pair<OptD, HourlyDetail> HourlyPricer::market_fair(const theo::Market& m) const {
    const std::optional<kp::time::Instant> settlement = parse_settlement(m);
    const HorizonVol& hv = total_vol_to(settlement);

    auto [fair, bracket] = theo::bracket_fair(m.strike_type, m.floor_strike, m.cap_strike,
                                              p_above_for(settlement));
    HourlyDetail d;
    d.bracket = std::move(bracket);
    d.spot = fit_.forward;
    d.forward = fit_.forward;
    d.w = hv.w;
    d.w_4pm = fit_.w;
    d.var_ratio = hv.ratio;
    d.settlement = settlement;
    d.minutes_to_settlement =
        settlement ? OptD(kp::time::total_seconds(now_, *settlement) / 60.0) : MISSING;
    d.sigma_annualized = sigma_annualized();
    d.variance_left_to_4pm = 1.0 - calendar_->phi(now_);
    d.atm_reason = fit_.reason;
    d.forward_source = fit_.forward_source;
    d.parity_forward = fit_.parity_forward_;
    d.index_minus_parity = fit_.index_minus_parity;
    d.cp_spread = fit_.cp_spread;
    d.vol_dispersion = fit_.dispersion;

    // bracket_fair can only report "no_digital" when p_above returns MISSING. Overwrite with the
    // specific cause so a board full of unpriced markets says WHY, not just that it failed.
    if (!hv.reason.empty() && !fair) {
        d.bracket.reason = hv.reason;
        // The reference interpolates d["settlement"], which is None when there is no close_time,
        // and Python renders that as the literal "None".
        d.bracket.error = "no vol for settlement "
                        + (settlement ? isoformat_utc(*settlement) : std::string("None"))
                        + ": " + hv.reason;
    }
    return {fair, std::move(d)};
}

}  // namespace kp::pricing::hourly

#include "kp/strategy/edgemgr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "kp/config.hpp"
#include "kp/pycompat/repr.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::strategy::edgemgr {

namespace th = kp::pricing::theo;
namespace gk = kp::pricing::greeks;
namespace vc = kp::pricing::varcal;
namespace ed = kp::pricing::edge;

std::optional<kp::time::Instant> settlement_of(const th::Market& m) {
    if (!m.close_time_s) return std::nullopt;
    return kp::time::Instant{*m.close_time_s * 1000000000LL};
}

EdgeManager::EdgeManager(std::string underlying,
                         std::optional<vc::VarianceCalendar> calendar,
                         std::optional<ed::Curve> curve, std::optional<bool> enabled,
                         const std::string& root)
    : underlying_(std::move(underlying)),
      enabled_(enabled.value_or(kp::config::EDGE_MANAGER_ENABLED)),
      calendar_(std::move(calendar)),
      curve_(std::move(curve)) {
    // The reason names the Python exception type, because that is what the reference journals:
    // a missing artifact is FileNotFoundError there, anything else it could raise while parsing
    // a present one is reported as ValueError (json.JSONDecodeError and the degenerate-anchor
    // check are both ValueError subclasses; the name differs only on a corrupt artifact).
    const auto why = [](const std::string& path) {
        return std::filesystem::exists(path) ? std::string("ValueError")
                                             : std::string("FileNotFoundError");
    };
    if (!calendar_) {
        try {
            calendar_ = vc::VarianceCalendar::load(underlying_, "default", root);
        } catch (const std::exception&) {
            reason_ = "no_calendar: " + why(vc::calendar_path(underlying_, "default", root));
        }
    }
    if (!curve_) {
        try {
            curve_ = ed::Curve::load(underlying_, root);
        } catch (const std::exception&) {
            // A missing calibration artifact installs the flat curve AND disables the manager.
            // That is a FAILURE, not the operator's choice -- see operator_flat_.
            reason_ = "no_calibration: " + why(ed::curve_path(underlying_, root));
            enabled_ = false;
        }
    }
}

EdgeRecord EdgeManager::flat(const std::string& reason) const {
    EdgeRecord r;
    r.edge_micros = (long)kp::pycompat::py_round_i(kp::config::QUOTE_OFFSET * 1e6);
    r.edge = kp::config::QUOTE_OFFSET;
    r.floor_micros = r.edge_micros;
    r.bonus_micros = 0L;
    r.risk_u = 0.0;
    r.scale_pos = 0.0;
    r.quotable = false;
    r.source = "no_R";
    r.reason = !reason.empty() ? reason : (!reason_.empty() ? reason_ : std::string("no_R"));
    return r;
}

EdgeRecord EdgeManager::flat_override() const {
    EdgeRecord r = flat("edge_manager_disabled_by_operator");
    r.quotable = true;
    r.source = "flat_override";
    return r;
}

EdgeRecord EdgeManager::no_r(const std::string& reason) const {
    return operator_flat_ ? flat_override() : flat(reason);
}

const EdgeMap& EdgeManager::refresh(
        const std::vector<th::Market>& markets, OptD forward, OptD sigma,
        const kp::time::Instant& now, double mono_s, bool force, OptD move_scale) {
    if (!force && has_cache_ && !cache_.empty()
        && (mono_s - cache_t_) < kp::config::EDGE_RECOMPUTE_S) {
        return cache_;
    }
    if (!available() || !forward || !sigma || *sigma <= 0.0) {
        EdgeMap out;
        for (const auto& m : markets) out[m.ticker] = no_r();
        cache_ = std::move(out);
        cache_t_ = mono_s;
        has_cache_ = true;
        return cache_;
    }

    EdgeMap out;
    for (const auto& m : markets) {
        const std::optional<kp::time::Instant> settle = settlement_of(m);
        if (!settle) {
            out[m.ticker] = no_r("no_settlement_ts");
            continue;
        }
        const double mins_left = kp::time::total_seconds(now, *settle) / 60.0;
        if (mins_left < kp::config::MIN_MINUTES_TO_CLOSE) {
            // Not a Greek problem to solve -- once the next minute reaches settlement the
            // contract resolves and its theta is (payoff - price), measured at -0.4999 for an ATM
            // rung at 15:59. Refuse explicitly.
            EdgeRecord r;
            r.risk_u = 1.0;
            r.scale_pos = 1.0;
            r.quotable = false;
            r.source = "gate";
            // f"within_{MIN_MINUTES_TO_CLOSE}min_of_close" -- Python spells the float "2.0".
            r.reason = "within_" + kp::pycompat::py_float_repr(kp::config::MIN_MINUTES_TO_CLOSE)
                       + "min_of_close";
            r.minutes_to_close = mins_left;
            out[m.ticker] = std::move(r);
            continue;
        }
        const gk::MarketGreeks g = gk::market_greeks(m.strike_type, m.floor_strike, m.cap_strike,
                                                     forward, sigma, now, *settle, *calendar_);
        const OptD R = g.R;
        const std::optional<long> em = R ? curve_->edge_micros(R) : std::nullopt;
        if (!R || !em) {
            // No R, or an R past anything a year of history produced. The first degrades to flat;
            // the second is a REFUSAL and must NOT degrade -- an R beyond the calibrated range is
            // a market we do not understand, not one to charge more for.
            if (!R) {
                out[m.ticker] = no_r(!g.reason.empty() ? g.reason : std::string("no_R"));
            } else {
                EdgeRecord r;
                r.risk_u = 1.0;
                r.scale_pos = 1.0;
                r.R = R;
                r.delta = g.delta;
                r.vega = g.vega;
                r.theta = g.theta;
                r.quotable = false;
                r.source = "curve";
                r.reason = "R_above_calibrated_max";
                out[m.ticker] = std::move(r);
            }
            continue;
        }
        // THE ADVERSE-SELECTION GATE. If a fast trader's expected gain over our cancel window,
        // |delta| * move_scale, exceeds the edge we charged, the quote is mispriced by
        // construction and the only move is not to write the option.
        if (move_scale && g.delta) {
            const double adverse = std::fabs(*g.delta) * *move_scale;
            if (adverse > (double)*em / 1e6) {
                EdgeRecord r;
                r.risk_u = curve_->risk_u(R);
                r.R = R;
                r.delta = g.delta;
                r.vega = g.vega;
                r.theta = g.theta;
                r.w = g.w;
                r.var_left = g.var_left;
                r.scale_pos = curve_->scale_position(R);
                r.quotable = false;
                r.source = "curve";
                r.reason = "adverse_selection";
                r.adverse_dollars = adverse;
                r.move_scale = *move_scale;
                out[m.ticker] = std::move(r);
                continue;
            }
        }
        const OptD bonus = curve_->bonus_dollars(R);
        EdgeRecord r;
        r.edge_micros = em;
        r.edge = (double)*em / 1e6;
        r.floor_micros = (long)kp::pycompat::py_round_i(ed::EDGE_FLOOR * 1e6);
        r.bonus_micros = bonus ? (long)kp::pycompat::py_round_i(*bonus * 1e6) : 0L;
        r.risk_u = curve_->risk_u(R);
        r.scale_pos = curve_->scale_position(R);
        r.R = R;
        r.delta = g.delta;
        r.vega = g.vega;
        r.theta = g.theta;
        r.w = g.w;
        r.var_left = g.var_left;
        r.quotable = true;
        r.source = "curve";
        out[m.ticker] = std::move(r);
    }
    cache_ = std::move(out);
    cache_t_ = mono_s;
    has_cache_ = true;
    last_computed_ = now;
    return cache_;
}

EdgeRecord EdgeManager::get(const std::string& ticker) const {
    if (const EdgeRecord* r = cache_.find(ticker)) return *r;
    return no_r("unknown_ticker");
}

EdgeSummary EdgeManager::summary() const {
    EdgeSummary s;
    long n_with_r = 0;
    std::vector<double> es;
    long refused = 0;
    for (const auto& [_t, v] : cache_) {
        if (v.R) {
            ++n_with_r;
            if (v.edge) es.push_back(*v.edge);
        }
        if (!v.quotable) ++refused;
    }
    if (n_with_r == 0) {
        s.n = 0;
        s.reason = reason_;
        s.priced = false;
        return s;
    }
    std::stable_sort(es.begin(), es.end());
    s.priced = true;
    s.n = (long)cache_.size();
    s.n_priced = (long)es.size();
    s.refused = refused;
    if (!es.empty()) {
        s.edge_min = es.front();
        s.edge_med = es[es.size() / 2];
        s.edge_max = es.back();
    }
    s.enabled = enabled_;
    return s;
}

}  // namespace kp::strategy::edgemgr

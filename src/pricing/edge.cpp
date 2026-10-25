#include "kp/pricing/edge.hpp"
#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace kp::pricing::edge {

namespace pc = kp::pycompat;

Curve::Curve(const kp::json::Value& art) {
    underlying_ = art.opt_str("underlying").value_or("");
    R_min_ = art.opt_num("R_min").value_or(0.0);
    R_p99_ = art.opt_num("R_p99").value_or(0.0);
    R_max_ = art.opt_num("R_max").value_or(0.0);
    built_utc_ = art.opt_str("built_utc").value_or("");
    if (const kp::json::Value* ns = art.find("n_sessions"); ns && ns->is_number())
        n_sessions_ = (long)ns->integer();
    if (!(R_min_ < R_p99_ && R_p99_ < R_max_))
        throw std::runtime_error("degenerate anchors");

    // Python: sorted((float(v), float(k)) for k, v in pct.items()) -- pairs of (R, percentile),
    // sorted by R with the percentile as the tie-break.
    std::vector<std::pair<double, double>> pairs;
    if (const kp::json::Value* pct = art.find("percentiles")) {
        for (const auto& m : pct->obj())
            if (m.second.is_number())
                pairs.emplace_back(m.second.num(), std::strtod(m.first.c_str(), nullptr));
    }
    std::sort(pairs.begin(), pairs.end());
    pct_r_.reserve(pairs.size());
    pct_p_.reserve(pairs.size());
    for (const auto& p : pairs) { pct_r_.push_back(p.first); pct_p_.push_back(p.second / 100.0); }
}

std::string Curve::repr() const {
    char b[200];
    std::snprintf(b, sizeof b, "<Curve %s R_min=%.5f R_p99=%.5f R_max=%.5f from %s sessions>",
                  underlying_.c_str(), R_min_, R_p99_, R_max_,
                  n_sessions_ ? std::to_string(*n_sessions_).c_str() : "None");
    return b;
}

std::string curve_path(const std::string& underlying, const std::string& root) {
    std::string base = root;
    if (base.empty()) {
        const char* env = std::getenv("KALSHI_ROOT");
        base = env ? env : KP_KALSHI_ROOT_DEFAULT;
    }
    return base + "/var/model/edge/" + underlying + ".json";
}

Curve Curve::load(const std::string& underlying, const std::string& root) {
    return Curve(kp::json::parse_file(curve_path(underlying, root)));
}

OptD Curve::bonus_dollars(OptD R) const {
    if (!R || !std::isfinite(*R)) return MISSING;
    if (*R > R_max_) return MISSING;                  // refuse, do not clamp
    if (*R <= R_min_) return 0.0;
    if (*R <= R_p99_) {
        const double u = (*R - R_min_) / (R_p99_ - R_min_);
        // KNEE * (b**u - 1) / (b - 1): 0 at u=0, KNEE at u=1. `** u` is pow.
        return BONUS_KNEE * (std::pow(BONUS_BASE, u) - 1.0) / (BONUS_BASE - 1.0);
    }
    const double v = (*R - R_p99_) / (R_max_ - R_p99_);
    return BONUS_KNEE + (BONUS_CAP - BONUS_KNEE) * v; // linear across the top 1%
}

OptD Curve::edge_dollars(OptD R) const {
    const OptD b = bonus_dollars(R);
    if (!b) return MISSING;
    return EDGE_FLOOR + *b;
}

std::optional<long> Curve::edge_micros(OptD R) const {
    const OptD e = edge_dollars(R);
    if (!e) return std::nullopt;
    return pc::py_round_i(*e * MICROS);
}

double Curve::scale_position(OptD R) const {
    const OptD e = edge_dollars(R);
    if (!e) return 1.0;                               // refused: the most conservative answer
    const double span = EDGE_CAP - EDGE_FLOOR;
    if (span <= 0) return 0.0;
    const double x = (*e - EDGE_FLOOR) / span;
    const double lo = (x > 0.0) ? x : 0.0;            // Python max(0.0, min(1.0, x))
    return (1.0 < lo) ? 1.0 : lo;
}

double Curve::risk_u(OptD R) const {
    if (!R || !std::isfinite(*R) || pct_r_.empty()) return 0.0;
    if (*R <= pct_r_.front()) return 0.0;
    if (*R >= pct_r_.back())  return 1.0;
    // Python breaks on the FIRST index whose R is >= the query, and interpolates on [lo, lo+1].
    std::size_t lo = 0;
    for (std::size_t i = 1; i < pct_r_.size(); ++i) {
        if (pct_r_[i] >= *R) { lo = i - 1; break; }
    }
    const double r0 = pct_r_[lo], r1 = pct_r_[lo + 1];
    const double p0 = pct_p_[lo], p1 = pct_p_[lo + 1];
    if (r1 <= r0) return p0;
    return p0 + (p1 - p0) * (*R - r0) / (r1 - r0);
}

// --- FlatCurve ---------------------------------------------------------------------------
OptD FlatCurve::edge_dollars(OptD) const { return kp::config::QUOTE_OFFSET; }
std::optional<long> FlatCurve::edge_micros(OptD) const {
    return pc::py_round_i(kp::config::QUOTE_OFFSET * MICROS);
}
double FlatCurve::scale_position(OptD) const { return 0.0; }
double FlatCurve::risk_u(OptD) const { return 0.0; }

}  // namespace kp::pricing::edge

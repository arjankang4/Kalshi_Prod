#include "kp/rvol/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "kp/rvol/npcompat.hpp"

namespace kp::rvol::model {

namespace {

// float(h.rstrip("m"))
double minutes_of(const std::string& h) {
    std::string s = h;
    while (!s.empty() && s.back() == 'm') s.pop_back();
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size())
        throw std::runtime_error("ValueError: could not convert string to float: '" + s + "'");
    return v;
}

OptD get(const Features& f, const std::string& k) {
    const OptD* p = f.find(k);
    return p ? *p : std::nullopt;
}

}  // namespace

std::string model_path(const std::string& underlying, const std::string& root) {
    std::string base = root;
    if (base.empty()) {
        const char* env = std::getenv("KALSHI_ROOT");
        base = env ? env : KP_KALSHI_ROOT_DEFAULT;
    }
    return base + "/var/model/rvol/" + underlying + ".json";
}

RealizedVolModel::RealizedVolModel(const kp::json::Value& art) {
    const auto* u = art.find("underlying");
    if (!u) throw std::runtime_error("KeyError: 'underlying'");
    underlying_ = u->str();
    built_utc_ = art.opt_str("built_utc");
    if (const auto* v = art.find("spec_version"); v && v->is_number()) spec_version_ = v->integer();
    if (const auto* v = art.find("n_sessions"); v && v->is_number()) n_sessions_ = v->integer();
    for (const char* key : {"features", "extra"}) {
        const auto* a = art.find(key);
        if (!a) throw std::runtime_error(std::string("KeyError: '") + key + "'");
        auto& dst = std::string(key) == "features" ? features_ : extra_;
        for (const auto& e : a->arr()) dst.push_back(e.str());
    }
    const auto* ms = art.find("models");
    if (!ms) throw std::runtime_error("KeyError: 'models'");
    for (const auto& [label, m] : ms->obj()) {
        Horizon h;
        h.label = label;
        h.beta = m.num_array("beta");
        if (const auto* s = m.find("sigma_resid"); s && !s->is_null()) h.sigma_resid = s->num();
        models_.push_back(std::move(h));
    }
}

RealizedVolModel RealizedVolModel::load(const std::string& underlying, const std::string& root) {
    const std::string p = model_path(underlying, root);
    if (!std::ifstream(p)) {
        throw std::runtime_error("FileNotFoundError: no rvol model for " + underlying + " at " + p
                                 + " -- run: python -m rvol.fit --fit --underlying " + underlying);
    }
    return RealizedVolModel(kp::json::parse_file(p));
}

const Horizon* RealizedVolModel::find(const std::string& label) const {
    for (const auto& h : models_)
        if (h.label == label) return &h;
    return nullptr;
}

std::vector<std::string> RealizedVolModel::horizons() const {
    std::vector<std::string> out;
    for (const auto& h : models_) out.push_back(h.label);
    std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return minutes_of(a) < minutes_of(b);
    });
    return out;
}

OptD RealizedVolModel::predict(const std::string& horizon, const Features& feats) const {
    const Horizon* m = find(horizon);
    if (!m) return std::nullopt;
    std::vector<double> row;
    for (const auto& c : features_) {
        const OptD v = get(feats, c);
        if (!v || !std::isfinite(*v) || *v <= 0) return std::nullopt;
        row.push_back(std::log(*v));                 // math.log: glibc
    }
    for (const auto& c : extra_) {
        const OptD* p = feats.find(c);
        const OptD v = p ? *p : OptD(0.0);           // feats.get(c, 0.0)
        if (!v || !std::isfinite(*v)) return std::nullopt;
        row.push_back(*v);
    }
    row.push_back(1.0);
    if (row.size() != m->beta.size()) {
        throw std::runtime_error("ValueError: shapes (" + std::to_string(row.size()) + ",) and ("
                                 + std::to_string(m->beta.size()) + ",) not aligned");
    }
    const double y = np::dot(row, m->beta);
    if (!std::isfinite(y)) return std::nullopt;
    const OptD s = m->sigma_resid;
    if (!s || !std::isfinite(*s) || *s <= 0) return std::exp(y);   // uncorrected median
    return std::exp(y + 0.5 * *s * *s);
}

std::pair<OptD, std::optional<std::string>> RealizedVolModel::predict_for_minutes(
    OptD minutes, const Features& feats, bool blend) const {
    if (!minutes || *minutes <= 0) return {std::nullopt, std::nullopt};
    const double mn = *minutes;
    std::vector<std::pair<double, std::string>> pairs;
    for (const auto& h : models_) pairs.emplace_back(minutes_of(h.label), h.label);
    std::sort(pairs.begin(), pairs.end());          // tuple order; labels are distinct
    if (pairs.empty()) return {std::nullopt, std::nullopt};
    if (!blend || pairs.size() == 1) {
        // min((abs(v - minutes), h) for v, h in pairs)
        std::pair<double, std::string> best{std::fabs(pairs[0].first - mn), pairs[0].second};
        for (std::size_t i = 1; i < pairs.size(); ++i) {
            std::pair<double, std::string> c{std::fabs(pairs[i].first - mn), pairs[i].second};
            if (c < best) best = c;
        }
        return {predict(best.second, feats), best.second};
    }
    if (mn <= pairs.front().first) return {predict(pairs.front().second, feats), pairs.front().second};
    if (mn >= pairs.back().first) return {predict(pairs.back().second, feats), pairs.back().second};
    for (std::size_t i = 0; i + 1 < pairs.size(); ++i) {
        const auto& [v0, h0] = pairs[i];
        const auto& [v1, h1] = pairs[i + 1];
        if (v0 <= mn && mn <= v1) {
            const OptD lo = predict(h0, feats), hi = predict(h1, feats);
            if (!lo || !hi) return {!hi ? lo : hi, !hi ? h0 : h1};
            const double wt = v1 == v0 ? 0.0 : (mn - v0) / (v1 - v0);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.2f", wt);   // f"{wt:.2f}": exact-decimal rounding
            return {(1.0 - wt) * *lo + wt * *hi, h0 + "/" + h1 + "@" + buf};
        }
    }
    return {std::nullopt, std::nullopt};
}

std::pair<bool, double> RealizedVolModel::retransform(const std::string& horizon) const {
    const Horizon* m = find(horizon);
    const OptD s = m ? m->sigma_resid : std::nullopt;
    if (!s || !std::isfinite(*s) || *s <= 0) return {false, 1.0};
    return {true, std::exp(0.5 * *s * *s)};
}

}  // namespace kp::rvol::model

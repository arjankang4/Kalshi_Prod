// Serving side of the realized-vol model: load the fitted log-HAR artifact and predict.
//
// Port of the LIVE half of Kalshi/rvol/model.py. `rvol/fit.py` writes
// var/model/rvol/{SPX,NDX}.json offline; the only thing model.py takes from fit.py at runtime is
// `fit.load` -- read that file, raise if it is missing -- and that is all that is reproduced here.
// `metrics` and `beats_baselines` (study diagnostics, no live caller) are not ported.
//
// THE PREDICTION IS THE CONDITIONAL MEAN, exp(beta'x + s^2/2), not the median exp(beta'x): the
// HAR is fitted on log sigma and exp is convex. An artifact without `sigma_resid` degrades to the
// median, and `retransform` says which happened.
//
// beta'x is `float(np.dot(row, beta))` in the reference, i.e. OpenBLAS ddot -- see
// kp/rvol/npcompat.hpp for why that is an FMA-contracted fold and not a plain one.
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/pycompat/odict.hpp"

namespace kp::rvol::model {

using OptD = std::optional<double>;

// {"f_3h": sigma, "pf_3h": frac, "cov_3h": cov, ...} in insertion order. An absent key and a
// present None both read as None, as `dict.get` does.
using Features = kp::pycompat::ODict<std::string, OptD>;

struct Horizon {
    std::string label;                 // "60m", "30m", "7.5m"
    std::vector<double> beta;
    OptD sigma_resid;
};

class RealizedVolModel {
public:
    explicit RealizedVolModel(const kp::json::Value& art);

    // <root>/var/model/rvol/<underlying>.json (root empty: $KALSHI_ROOT, else the build default).
    // Throws std::runtime_error("FileNotFoundError: ...") when the artifact is missing, as
    // fit.load raises -- LeanChain turns that into "lean unavailable".
    static RealizedVolModel load(const std::string& underlying, const std::string& root = "");

    // Horizon labels sorted by their numeric minutes.
    std::vector<std::string> horizons() const;

    // Predicted annualized sigma at one fitted horizon, or None when any feature is missing,
    // non-finite or non-positive (a log-HAR has no answer without its inputs).
    OptD predict(const std::string& horizon, const Features& feats) const;

    // Prediction for an arbitrary horizon in minutes: (sigma, label). BLENDED between the two
    // bracketing fitted horizons by default, clamped to the nearest end outside them; the label
    // is "h0/h1@0.67" for a blend. `blend=false` is the nearest-horizon form the studies use.
    std::pair<OptD, std::optional<std::string>> predict_for_minutes(
        OptD minutes, const Features& feats, bool blend = true) const;

    // (applied?, exp(s^2/2)) -- whether this horizon serves the mean or the uncorrected median.
    std::pair<bool, double> retransform(const std::string& horizon) const;

    const std::string& underlying() const { return underlying_; }
    const std::vector<std::string>& features() const { return features_; }
    const std::vector<std::string>& extra() const { return extra_; }

private:
    const Horizon* find(const std::string& label) const;

    std::string underlying_;
    std::optional<std::string> built_utc_;
    std::optional<long long> spec_version_, n_sessions_;
    std::vector<std::string> features_, extra_;
    std::vector<Horizon> models_;      // artifact (insertion) order
};

std::string model_path(const std::string& underlying, const std::string& root = "");

}  // namespace kp::rvol::model

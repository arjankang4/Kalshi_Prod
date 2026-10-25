// rvol/model against the reference: both live artifacts and seven synthetic ones, every horizon
// predicted directly, predict_for_minutes blended and nearest across the fitted range, the
// horizon ordering, and the median/mean retransform flag.
#include <map>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/rvol/model.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace M = kp::rvol::model;

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    Tally T("rvol_model");
    T.expect({"load", "art", "horizons", "retransform", "feats", "predict", "pfm", "missing"});

    std::map<std::string, M::RealizedVolModel> models;
    std::map<int, M::Features> feats;

    // A missing artifact is an exception, not a default model.
    try {
        M::RealizedVolModel::load("NOSUCH", root);
        T.hit("missing", false, "load(NOSUCH) did not throw");
    } catch (const std::runtime_error& e) {
        T.hit("missing", std::string(e.what()).rfind("FileNotFoundError", 0) == 0, e.what());
    }

    for (const auto& line : read(dir, "rvol_model")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "load") {
            models.emplace(w[1], M::RealizedVolModel::load(w[1], root));
            T.hit(fn, models.at(w[1]).underlying() == w[1], line);
        } else if (fn == "art") {
            const std::string js = line.substr(line.find(w[1]) + w[1].size() + 1);
            models.emplace(w[1], M::RealizedVolModel(kp::json::parse(js)));
            T.hit(fn, true, line);
        } else if (fn == "horizons") {
            const auto hz = models.at(w[1]).horizons();
            bool ok = hz.size() + 2 == w.size();
            for (std::size_t i = 0; ok && i < hz.size(); ++i) ok = hz[i] == w[2 + i];
            T.hit(fn, ok, line);
        } else if (fn == "retransform") {
            const auto [applied, fac] = models.at(w[1]).retransform(w[2]);
            T.hit(fn, (applied ? "1" : "0") == w[3] && same(fac, w[4]), line);
        } else if (fn == "feats") {
            M::Features f;
            for (std::size_t i = 2; i < w.size(); ++i) {
                const auto eq = w[i].find('=');
                f.set(w[i].substr(0, eq), ohx(w[i].substr(eq + 1)));
            }
            feats[std::stoi(w[1])] = f;
            T.hit(fn, true, line);
        } else if (fn == "predict") {
            const auto got = models.at(w[1]).predict(w[3], feats.at(std::stoi(w[2])));
            T.hit(fn, same(got, w[4]), line);
        } else if (fn == "pfm") {
            const auto [s, lab] = models.at(w[1]).predict_for_minutes(
                ohx(w[3]), feats.at(std::stoi(w[2])), w[4] == "1");
            T.hit(fn, same(s, w[5]) && (lab ? *lab : std::string("-")) == w[6], line);
        } else {
            T.hit("unknown:" + fn, false, line);
        }
    }
    return T.report();
}

// The shared replay scaffold for tests/port/ suites: read a .vec, tally per function, report.
//
// Every port suite follows the same contract, so it lives in one place:
//   - a missing .vec is FATAL, never a skip (a suite that reads nothing passes while testing
//     nothing -- this harness has already had that failure twice);
//   - every function tag that appears must be checked at least once, and an EXPECTED tag that
//     never appears is a failure too (`expect`), so a generator that silently stops emitting a
//     branch turns the suite red;
//   - the first mismatching line of each tag is printed verbatim, so the failure is reproducible
//     from the log alone.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "harness.hpp"

namespace kp::test::vec {

inline bool none(const std::string& s) { return s == "None"; }
inline double hx(const std::string& s) {
    if (s == "nan") return std::nan("");
    if (s == "inf") return HUGE_VAL;
    if (s == "-inf") return -HUGE_VAL;
    return std::strtod(s.c_str(), nullptr);
}
inline std::optional<double> ohx(const std::string& s) {
    return none(s) ? std::nullopt : std::optional<double>(hx(s));
}
inline std::optional<long> oint(const std::string& s) {
    return none(s) ? std::nullopt : std::optional<long>(std::stol(s));
}
// "-" is how generators spell an empty/None string.
inline std::string ostr(const std::string& s) { return s == "-" ? std::string() : s; }

inline bool same(std::optional<double> g, const std::string& w) {
    if (none(w) || !g) return none(w) == !g.has_value();
    return kp::test::same_bits(*g, hx(w));
}
inline bool same(double g, const std::string& w) { return !none(w) && kp::test::same_bits(g, hx(w)); }
inline bool same(std::optional<long> g, const std::string& w) {
    if (none(w) || !g) return none(w) == !g.has_value();
    return *g == std::stol(w);
}

// Lines of <dir>/<name>.vec, header stripped. Exits the process on a missing file.
inline std::vector<std::string> read(const std::string& dir, const std::string& name) {
    std::ifstream in(dir + "/" + name + ".vec");
    if (!in) {
        std::printf("FATAL: %s/%s.vec missing -- run 'make vectors'\n", dir.c_str(), name.c_str());
        std::exit(1);
    }
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line))
        if (!line.empty() && line[0] != '#') out.push_back(line);
    if (out.empty()) {
        std::printf("FATAL: %s.vec has no cases\n", name.c_str());
        std::exit(1);
    }
    return out;
}

inline std::vector<std::string> words(const std::string& line) {
    std::istringstream ss(line);
    std::vector<std::string> w;
    std::string t;
    while (ss >> t) w.push_back(t);
    return w;
}

class Tally {
public:
    explicit Tally(std::string suite) : suite_(std::move(suite)) {}
    void expect(std::initializer_list<const char*> tags) { for (auto t : tags) expected_.insert(t); }
    void hit(const std::string& tag, bool ok, const std::string& line) {
        auto& t = t_[tag];
        ++t.n;
        if (!ok) { if (!t.bad) t.first = line; ++t.bad; }
    }
    int report() {
        std::printf("%s golden vectors\n", suite_.c_str());
        int rc = 0;
        for (const auto& e : expected_)
            if (!t_.count(e)) { std::printf("  FAIL %s -- expected tag never appeared\n", e.c_str()); rc = 1; }
        for (const auto& [tag, t] : t_) {
            if (t.bad) {
                std::printf("  FAIL %s -- %ld/%ld mismatched; first: %s\n", tag.c_str(), t.bad, t.n,
                            t.first.c_str());
                rc = 1;
            } else {
                std::printf("  ok   %s (%ld cases)\n", tag.c_str(), t.n);
            }
        }
        if (!kp::test::fails().empty()) rc = 1;
        std::printf("\ntest_%s: %s\n", suite_.c_str(), rc ? "FAILED" : "all green");
        return rc;
    }

private:
    struct T { long n = 0, bad = 0; std::string first; };
    std::string suite_;
    std::map<std::string, T> t_;
    std::set<std::string> expected_;
};

}  // namespace kp::test::vec

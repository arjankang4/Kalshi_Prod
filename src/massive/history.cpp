#include "kp/massive/history.hpp"

#include <algorithm>
#include <set>
#include <unordered_set>

#include "detail.hpp"

namespace kp::massive::history {

namespace {

// Python's s[2:-15] with its clamping.
std::string root_of(const std::string& t) {
    const long n = (long)t.size();
    const long lo = std::min(2L, n);
    const long hi = std::max(0L, n - 15);
    return hi > lo ? t.substr((std::size_t)lo, (std::size_t)(hi - lo)) : std::string();
}

const kp::json::Value& at(const kp::json::Value& obj, const std::string& k) {
    const kp::json::Value* v = obj.is_object() ? obj.find(k) : nullptr;
    if (!v) throw std::out_of_range("'" + k + "'");
    return *v;
}

kp::json::Value agg_page(const Rest& rest, const std::string& url) {
    kp::json::Value page = rest.get_url(url, 60.0, &rest.agg_limiter());
    const kp::json::Value* st = page.is_object() ? page.find("status") : nullptr;
    if (st && st->is_string() && st->str() == "ERROR") {
        const kp::json::Value* e = page.find("error");
        const kp::json::Value* m = page.find("message");
        const kp::json::Value* pick = (e && detail::truthy(*e)) ? e : m;
        std::string s = !pick ? "None" : pick->is_string() ? pick->str() : detail::py_repr(*pick);
        throw MassiveHistoryError("aggregates returned 200 with an error body: "
                                  + detail::head(s, 200));
    }
    return page;
}

}  // namespace

std::optional<std::string> pm_settled_root(const std::vector<kp::json::Value>& contracts) {
    std::set<std::string> roots;
    for (const auto& c : contracts) roots.insert(root_of(at(c, "ticker").str()));
    if (roots.empty()) return std::nullopt;
    for (const char* preferred : {"SPXW", "NDXP"})
        if (roots.count(preferred)) return std::string(preferred);
    if (roots.size() == 1) return *roots.begin();
    std::string msg = "ambiguous option roots, no PM-settled match: {";
    bool first = true;
    for (const auto& r : roots) {
        msg += (first ? "'" : ", '") + r + "'";
        first = false;
    }
    throw MassiveHistoryError(msg + "}");
}

std::vector<kp::json::Value> aggregate_bars(const Rest& rest, const std::string& ticker,
                                            long multiplier, const std::string& timespan,
                                            const std::string& start, const std::string& end,
                                            bool adjusted, long limit) {
    std::string url = rest.base() + "/v2/aggs/ticker/" + ticker + "/range/"
                      + std::to_string(multiplier) + "/" + timespan + "/" + start + "/" + end + "?"
                      + urlencode({{"adjusted", adjusted ? "true" : "false"},
                                   {"sort", "asc"},
                                   {"limit", std::to_string(limit)}});
    std::vector<kp::json::Value> out;
    std::unordered_set<long long> seen;
    while (!url.empty()) {
        const kp::json::Value page = agg_page(rest, url);
        const kp::json::Value* res = page.find("results");
        if (!res || !detail::truthy(*res)) break;
        for (const auto& b : res->arr()) {
            const long long t = detail::py_int(at(b, "t"));
            if (seen.insert(t).second) out.push_back(b);
        }
        const kp::json::Value* nxt = page.find("next_url");
        url = (nxt && nxt->is_string()) ? nxt->str() : std::string();
    }
    std::stable_sort(out.begin(), out.end(), [](const kp::json::Value& a, const kp::json::Value& b) {
        return detail::py_int(*a.find("t")) < detail::py_int(*b.find("t"));
    });
    return out;
}

std::vector<kp::json::Value> second_bars(const Rest& rest, const std::string& ticker,
                                         const std::string& start_date, const std::string& end_date,
                                         bool adjusted, long limit) {
    return aggregate_bars(rest, ticker, 1, "second", start_date, end_date, adjusted, limit);
}

}  // namespace kp::massive::history

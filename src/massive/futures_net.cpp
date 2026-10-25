#include "kp/massive/futures_net.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "detail.hpp"
#include "kp/time/et.hpp"

namespace kp::massive::futures {

namespace {

void check_error_body(const kp::json::Value& page, const char* what) {
    const kp::json::Value* st = page.is_object() ? page.find("status") : nullptr;
    if (st && st->is_string() && st->str() == "ERROR") {
        const kp::json::Value* e = page.find("error");
        const kp::json::Value* m = page.find("message");
        const kp::json::Value* pick = (e && detail::truthy(*e)) ? e : m;
        std::string s = !pick ? "None" : pick->is_string() ? pick->str() : detail::py_repr(*pick);
        throw MassiveHistoryError(std::string(what) + " returned 200 with an error body: "
                                  + detail::head(s, 200));
    }
}

std::vector<kp::json::Value> aggs_range(const Rest& rest, const std::string& ticker,
                                        const std::string& resolution, long long start_ns,
                                        long long end_ns, long limit) {
    const std::string url = rest.base() + "/futures/v1/aggs/" + ticker + "?"
        + urlencode({{"resolution", resolution}, {"order", "asc"}, {"sort", "window_start"},
                     {"window_start.gte", std::to_string(start_ns)},
                     {"window_start.lt", std::to_string(end_ns)},
                     {"limit", std::to_string(limit)}});
    const kp::json::Value page = rest.get_url(url, 60.0, &rest.futures_limiter());
    check_error_body(page, "futures aggs");
    std::vector<kp::json::Value> res;
    if (const auto* r = page.find("results"); r && detail::truthy(*r)) res = r->arr();
    if ((long)res.size() < limit) return res;
    if (end_ns - start_ns <= MIN_SPLIT_NS) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f", detail::py_true_div(end_ns - start_ns, 1000000000LL));
        throw MassiveHistoryError(ticker + ": " + std::to_string(res.size()) + " rows in a " + buf
                                  + "s window at the page limit and the window cannot be split further");
    }
    // Python floor division; both operands positive here.
    const long long mid = start_ns + (end_ns - start_ns) / 2;
    auto a = aggs_range(rest, ticker, resolution, start_ns, mid, limit);
    auto b = aggs_range(rest, ticker, resolution, mid, end_ns, limit);
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace

kp::json::Value get(const Rest& rest, const std::string& path, const Params& params) {
    std::vector<std::pair<std::string, std::string>> kv;
    for (const auto& [k, v] : params)
        if (v) kv.emplace_back(k, *v);
    const kp::json::Value page =
        rest.get_url(rest.base() + path + "?" + urlencode(kv), 60.0, &rest.futures_limiter());
    check_error_body(page, "futures endpoint");
    return page;
}

kp::json::Value quote_page(const Rest& rest, const std::string& contract) {
    return get(rest, "/futures/v1/quotes/" + contract, {{"limit", "1"}});
}

std::vector<kp::json::Value> aggs(const Rest& rest, const std::string& ticker,
                                  const std::string& resolution, long long start_ns,
                                  long long end_ns, long limit) {
    if (resolution != "1sec" && resolution != "1min" && resolution != "1second"
        && resolution != "1minute")
        throw std::invalid_argument("resolution '" + resolution
                                    + "' is rejected by the vendor with HTTP 400; use one of "
                                      "('1sec', '1min', '1second', '1minute')");
    const auto out = aggs_range(rest, ticker, resolution, start_ns, end_ns, limit);
    std::unordered_set<long long> seen;
    std::vector<kp::json::Value> uniq;
    for (const auto& b : out) {
        const kp::json::Value* ws = b.find("window_start");
        if (!ws || ws->is_null()) continue;
        if (seen.insert(detail::py_int(*ws)).second) uniq.push_back(b);
    }
    std::stable_sort(uniq.begin(), uniq.end(), [](const kp::json::Value& a, const kp::json::Value& b) {
        return detail::py_int(*a.find("window_start")) < detail::py_int(*b.find("window_start"));
    });
    if (!uniq.empty()) {   // _assert_window
        long long lo = detail::py_int(*uniq.front().find("window_start"));
        long long hi = detail::py_int(*uniq.back().find("window_start"));
        if (lo < start_ns || hi >= end_ns)
            throw MassiveHistoryError(
                ticker + ": vendor ignored the window filter -- asked [" + std::to_string(start_ns)
                + ", " + std::to_string(end_ns) + "), got [" + std::to_string(lo) + ", "
                + std::to_string(hi) + "]. See the date-filtering note in massive/futures.py before "
                  "'fixing' this.");
    }
    return uniq;
}

double session_volume(const Rest& rest, const std::string& ticker, int year, int month, int day) {
    const long long lo = kp::time::from_et(year, month, day, 9, 30).ns;
    const long long hi = kp::time::from_et(year, month, day, 16, 0).ns;
    // sum(b.get("volume") or 0 for b in bars): the int fast path, then the float path.
    long long isum = 0;
    bool is_float = false;
    detail::PyFloatSum fsum;
    for (const auto& b : aggs(rest, ticker, "1min", lo, hi)) {
        const kp::json::Value* v = b.find("volume");
        const bool t = v && detail::truthy(*v);
        if (!is_float && (!t || v->is_int() || v->is_bool())) {
            isum += t ? detail::py_int(*v) : 0;
            continue;
        }
        if (!is_float) {
            is_float = true;
            fsum.add((double)isum + detail::py_float(*v));   // int + float -> the first float term
            continue;
        }
        if (!t || v->is_int() || v->is_bool()) fsum.add_int(t ? (double)detail::py_int(*v) : 0.0);
        else fsum.add(detail::py_float(*v));
    }
    return is_float ? fsum.result() : (double)isum;
}

}  // namespace kp::massive::futures

#include "kp/massive/index_rest.hpp"

#include <algorithm>
#include <set>

#include "detail.hpp"
#include "kp/massive/history.hpp"

namespace kp::massive::index {

const std::map<std::string, std::string>& INDEX_TICKER() {
    static const std::map<std::string, std::string> m = {{"SPX", "I:SPX"}, {"NDX", "I:NDX"}};
    return m;
}

std::map<std::string, IndexQuote> snapshot(const Rest& rest,
                                           const std::vector<std::string>& underlyings) {
    // want = {u: INDEX_TICKER[u] for u in underlyings if u in INDEX_TICKER}, insertion-ordered.
    std::vector<std::pair<std::string, std::string>> want;
    for (const auto& u : underlyings) {
        const auto it = INDEX_TICKER().find(u);
        if (it == INDEX_TICKER().end()) continue;
        if (std::none_of(want.begin(), want.end(), [&](const auto& p) { return p.first == u; }))
            want.emplace_back(u, it->second);
    }
    if (want.empty()) return {};
    std::vector<std::string> tks;
    for (const auto& p : want) tks.push_back(p.second);
    std::sort(tks.begin(), tks.end());
    std::string joined;
    for (const auto& t : tks) joined += (joined.empty() ? "" : ",") + t;

    const kp::json::Value page = rest.get("/v3/snapshot/indices", {{"ticker.any_of", joined}});
    // by_ticker = {r.get("ticker"): r for r in results}: a later row for the same ticker wins.
    std::map<std::string, const kp::json::Value*> by_ticker;
    const kp::json::Value* res = page.is_object() ? page.find("results") : nullptr;
    if (res && detail::truthy(*res))
        for (const auto& r : res->arr()) {
            const kp::json::Value* t = r.find("ticker");
            if (t && t->is_string()) by_ticker[t->str()] = &r;
        }
    std::map<std::string, IndexQuote> out;
    for (const auto& [und, tk] : want) {
        const auto it = by_ticker.find(tk);
        if (it == by_ticker.end() || !detail::truthy(*it->second)) continue;
        const kp::json::Value& r = *it->second;
        const kp::json::Value* v = r.find("value");
        if (!v || v->is_null()) continue;
        IndexQuote q;
        q.underlying = und;
        q.ticker = tk;
        q.value = detail::py_float(*v);
        if (const auto* lu = r.find("last_updated"); lu && !lu->is_null()) q.ts_ns = detail::py_int(*lu);
        if (const auto* ms = r.find("market_status"); ms && ms->is_string()) q.market_status = ms->str();
        if (const auto* nm = r.find("name"); nm && nm->is_string()) q.name = nm->str();
        out.emplace(und, std::move(q));
    }
    return out;
}

std::vector<std::pair<long long, double>> closes_ms(const Rest& rest, const std::string& underlying,
                                                    const std::string& day) {
    const auto it = INDEX_TICKER().find(underlying);
    if (it == INDEX_TICKER().end()) throw std::out_of_range("'" + underlying + "'");
    std::vector<std::pair<long long, double>> out;
    for (const auto& b : history::second_bars(rest, it->second, day, day)) {
        const kp::json::Value* c = b.find("c");
        if (!c || c->is_null()) continue;
        const double cv = detail::py_float(*c);
        if (cv > 0) out.emplace_back(detail::py_int(*b.find("t")), cv);
    }
    return out;
}

}  // namespace kp::massive::index

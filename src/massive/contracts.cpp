#include "kp/massive/contracts.hpp"

#include <algorithm>
#include <set>

#include "detail.hpp"
#include "kp/massive/history.hpp"

namespace kp::massive::client {

using kp::json::Value;

std::vector<Value> option_contracts(const Rest& rest, const std::string& underlying,
                                    const std::optional<std::string>& expiration_date) {
    std::vector<Value> out;
    Value page = rest.rest("/v3/reference/options/contracts",
                           {{"underlying_ticker", underlying},
                            {"expiration_date", expiration_date},
                            {"limit", "1000"}});
    while (true) {
        if (const Value* r = page.find("results"); r && detail::truthy(*r))
            for (const auto& c : r->arr()) out.push_back(c);
        const Value* nxt = page.find("next_url");
        if (!nxt || !detail::truthy(*nxt)) return out;
        page = rest.rest_url(nxt->str());
    }
}

RootStrikes option_root_and_strikes(const Rest& rest, const std::string& expiry_date,
                                    const std::string& underlying) {
    const auto contracts = option_contracts(rest, underlying, expiry_date);
    RootStrikes out;
    out.root = history::pm_settled_root(contracts);
    std::set<double> strikes;
    for (const auto& c : contracts) {
        const std::string& t = c.find("ticker")->str();
        // c["ticker"][2:-15] == root (None never equals a str)
        const long n = (long)t.size(), lo = std::min(2L, n), hi = std::max(0L, n - 15);
        const std::string r = hi > lo ? t.substr((std::size_t)lo, (std::size_t)(hi - lo)) : "";
        if (!out.root || r != *out.root) continue;
        out.contracts.push_back(c);
        const Value* k = c.find("strike_price");
        if (!k) throw std::out_of_range("'strike_price'");
        strikes.insert(detail::py_float(*k));
    }
    out.strikes.assign(strikes.begin(), strikes.end());
    return out;
}

}  // namespace kp::massive::client

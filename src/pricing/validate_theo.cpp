#include "kp/pricing/validate_theo.hpp"

#include "kp/pricing/universe.hpp"

namespace kp::pricing::validate_theo {

namespace {

// Python truthiness of a float that may be None: None and 0.0 are false, NaN is true.
bool truthy(const OptD& v) { return v.has_value() && *v != 0.0; }

BoardFair daily_fair(const pricer::Pricer& p, const theo::Market& m) {
    auto [fair, d] = p.market_fair(m);
    BoardFair bf;
    bf.fair = fair;
    bf.daily = std::move(d);
    return bf;
}

BoardFair hourly_fair(const hourly::HourlyPricer& hp, const theo::Market& m) {
    auto [fair, d] = hp.market_fair(m);
    BoardFair bf;
    bf.hourly = true;
    bf.fair = fair;
    bf.hour = std::move(d);
    return bf;
}

}  // namespace

std::pair<pricer::Pricer, Fairs> price_markets(const std::vector<theo::Market>& markets,
                                               const kp::massive::OptionSnapshot& snapshot,
                                               const std::string& root, const std::string& expiry,
                                               const std::vector<double>& strikes, OptD spot) {
    pricer::Pricer p(universe::quotes_by_strike(snapshot, root, expiry, strikes), strikes, spot);
    Fairs fairs;
    for (const auto& m : markets) fairs.set(m.ticker, daily_fair(p, m));
    return {std::move(p), std::move(fairs)};
}

Resolved resolve_and_smooth(const kp::massive::OptionSnapshot& snapshot, const std::string& root,
                            const std::string& expiry, const std::vector<double>& strikes,
                            const std::optional<kp::massive::index::IndexQuote>& index_quote,
                            const SkewObserver* skew_state, std::optional<long long> index_at_ns) {
    const skew::Board quotes = universe::quotes_by_strike(snapshot, root, expiry, strikes);
    pricer::Pricer probe(quotes, strikes);                       // parity, unconditionally
    spot::SpotResolution res = spot::resolve(probe.spot(), index_quote, std::nullopt, index_at_ns);
    Resolved out{res.source == "index" ? pricer::Pricer(quotes, strikes, res.value) : probe, res,
                 std::nullopt};
    pricer::Pricer& pr = out.pricer;
    if (skew_state && *skew_state && truthy(pr.spot())) {
        const OptD w_atm = skew::atm_vol(quotes, strikes, pr.spot());
        if (truthy(w_atm)) {
            // Observe unconditionally -- the state prunes on its clock -- but only PRICE off a
            // real curve: a flat `too_few_smoothed_buckets` fit would throw the smile away.
            skew::SkewFit smoothed = (*skew_state)(quotes, strikes, pr.spot(), w_atm);
            if (smoothed.ok()) {
                pr.use_skew(smoothed);
                out.smoothed = std::move(smoothed);
            }
        }
    }
    return out;
}

Priced resolve_and_price(const std::vector<theo::Market>& markets,
                         const kp::massive::OptionSnapshot& snapshot, const std::string& root,
                         const std::string& expiry, const std::vector<double>& strikes,
                         const std::optional<kp::massive::index::IndexQuote>& index_quote,
                         const SkewObserver* skew_state, std::optional<long long> index_at_ns) {
    Resolved r = resolve_and_smooth(snapshot, root, expiry, strikes, index_quote, skew_state,
                                    index_at_ns);
    Fairs fairs;
    for (const auto& m : markets) fairs.set(m.ticker, daily_fair(r.pricer, m));
    return {std::move(r.pricer), std::move(fairs), std::move(r.res)};
}

PricedBoards resolve_and_price_boards(
    const std::vector<theo::Market>& daily_markets, const std::vector<theo::Market>& hourly_markets,
    const kp::massive::OptionSnapshot& snapshot, const std::string& root, const std::string& expiry,
    const std::vector<double>& strikes, std::optional<kp::time::Instant> now,
    const varcal::VarianceCalendar* calendar,
    const std::optional<kp::massive::index::IndexQuote>& index_quote,
    const SkewObserver* skew_state, std::optional<long long> index_at_ns) {
    Resolved r = resolve_and_smooth(snapshot, root, expiry, strikes, index_quote, skew_state,
                                    index_at_ns);
    PricedBoards out{std::move(r.pricer), std::nullopt, {}, std::move(r.res),
                     std::move(r.smoothed)};
    for (const auto& m : daily_markets) out.fairs.set(m.ticker, daily_fair(out.daily, m));
    if (!hourly_markets.empty() && calendar != nullptr && now) {
        out.hourly.emplace(universe::quotes_by_strike(snapshot, root, expiry, strikes), strikes,
                           *now, *calendar, std::nullopt, out.res.index);
        if (out.smoothed) out.hourly->use_skew(*out.smoothed);
        for (const auto& m : hourly_markets) out.fairs.set(m.ticker, hourly_fair(*out.hourly, m));
    }
    return out;
}

std::pair<hourly::HourlyPricer, Fairs> hourly_price_markets(
    const std::vector<theo::Market>& markets, const skew::Board& quotes,
    const std::vector<double>& strikes, const kp::time::Instant& now,
    const varcal::VarianceCalendar& calendar, OptD index_forward) {
    hourly::HourlyPricer hp(quotes, strikes, now, calendar, std::nullopt, index_forward);
    Fairs fairs;
    for (const auto& m : markets) fairs.set(m.ticker, hourly_fair(hp, m));
    return {std::move(hp), std::move(fairs)};
}

}  // namespace kp::pricing::validate_theo

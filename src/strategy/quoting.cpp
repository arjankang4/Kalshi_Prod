#include "kp/strategy/quoting.hpp"

#include <cmath>
#include <cstdio>

namespace kp::strategy::quoting {

namespace pc = kp::pycompat;
namespace cfg = kp::config;

double edge_dollars(const std::string& side, double theo, double price) {
    return side == "bid" ? (theo - price) : (price - theo);
}

long edge_micros(const std::string& side, double theo, double price) {
    const long t = pc::py_round_i(theo * MICROS);
    const long p = pc::py_round_i(price * MICROS);
    return side == "bid" ? (t - p) : (p - t);
}

bool tradeable(OptD price) {
    return price.has_value() && cfg::MIN_PRICE <= *price && *price <= cfg::MAX_PRICE;
}

Touches synth_touches(OptD best_bid, OptD best_ask) {
    OptD bid_c = best_bid, ask_c = best_ask;
    if (!best_bid && best_ask) {
        const double v = pc::floor_tick(*best_ask - ONE_SIDED_WIDTH * TICK);
        bid_c = (cfg::MIN_PRICE > v) ? cfg::MIN_PRICE : v;      // Python max()
    } else if (!best_ask && best_bid) {
        const double v = pc::ceil_tick(*best_bid + ONE_SIDED_WIDTH * TICK);
        ask_c = (cfg::MAX_PRICE < v) ? cfg::MAX_PRICE : v;      // Python min()
    }
    return {bid_c, ask_c};
}

Action decide(const std::string& side, OptD theo, OptD best_bid, OptD best_ask,
              OptD offset, std::optional<long> min_edge_micros, Tighten tighten) {
    if (!theo) return {"none", NONE, NONE, "no_theo"};
    const double off = offset.value_or(cfg::QUOTE_OFFSET);
    const long bar = min_edge_micros.value_or(EDGE_MIN_MICROS);
    const double th = *theo;
    const bool is_bid = (side == "bid");

    OptD target = is_bid ? pc::floor_tick(th - off) : pc::ceil_tick(th + off);

    // Wide-market cap: only meaningful when both touches exist.
    //
    // On a wide book the edge-implied target can sit far inside the NBBO, and posting there hands
    // the whole spread away -- nobody was competing at that price. So when we would BE the best
    // bid or offer we tighten only a FRACTION of the way in from the touch we are improving.
    //
    //   book 8 at 15, theo 12, edge 0.8  ->  naive target 11
    //                                        capped at 8 + (15-8)/3 = 10.33  ->  10
    //
    // A ONE-SIDED BOOK IS TREATED AS A ONE_SIDED_WIDTH-WIDE ONE. Before 2026-08-23 a missing side
    // meant no width, so the cap was skipped entirely and the target went in at full edge --
    // measured, decide("bid", 0.10, 0.04, None) posted 0.09, five cents inside the only touch,
    // bounded by nothing but [0.01, 0.99]. That is the opposite of what the cap exists for: the
    // less competition there is on our side, the LESS we should give away. With BOTH sides absent
    // there is nothing to anchor to and the target stands.
    const Touches tc = synth_touches(best_bid, best_ask);
    const OptD bid_c = tc.bid, ask_c = tc.ask;

    if (bid_c && ask_c) {
        const double width = *ask_c - *bid_c;
        // Three-state: default sentinel, an explicit fraction, or absent meaning cap-at-mid.
        const bool legacy_mid = !tighten.use_default && !tighten.frac;
        const double frac = tighten.use_default ? cfg::QUOTE_TIGHTEN_FRACTION
                                                : tighten.frac.value_or(0.0);
        if (width > 2 * TICK) {
            double cap;
            if (legacy_mid) cap = (*bid_c + *ask_c) / 2.0;
            else            cap = is_bid ? (*bid_c + width * frac) : (*ask_c - width * frac);
            // min/max, so the cap only ever pulls a target BACK toward the touch; a target
            // already at or behind the touch is untouched.
            double t = *target;
            t = is_bid ? ((cap < t) ? cap : t) : ((cap > t) ? cap : t);
            target = is_bid ? pc::floor_tick(t) : pc::ceil_tick(t);
        }
    }

    // `own` stays the REAL touch -- improve/penny/join are about a book we can actually join, and
    // a synthetic level is not one. `far` uses the imputed side so the cross guard binds.
    const OptD own = is_bid ? best_bid : best_ask;
    const OptD far = is_bid ? ask_c : bid_c;

    auto crosses = [&](double p) {
        if (!far) return false;
        return is_bid ? (p >= *far) : (p <= *far);
    };

    // 1. improve -- strictly better than the touch, positive edge, tradeable, does not cross
    if (!tradeable(target)) target = NONE;
    if (target && own && !crosses(*target)) {
        const bool improves = is_bid ? (*target > *own) : (*target < *own);
        const long em = edge_micros(side, th, *target);
        if (improves && em > 0)
            return {"improve", pc::py_round_nd(*target, 4), (double)em / MICROS, "target_inside"};
    } else if (target && !own && !crosses(*target)) {
        const long em = edge_micros(side, th, *target);
        if (em > 0)
            return {"improve", pc::py_round_nd(*target, 4), (double)em / MICROS, "empty_side"};
    }

    if (!own) return no_quote();

    // 2. penny -- one tick inside the touch, but only if it retains at least the bar
    const double pen = pc::py_round_nd(is_bid ? (*own + TICK) : (*own - TICK), 4);
    if (tradeable(pen) && !crosses(pen)) {
        const long em = edge_micros(side, th, pen);
        if (em >= bar)
            return {"penny", pc::py_round_nd(pen, 4), (double)em / MICROS, "penny_above_min_edge"};
    }

    // 3. join -- passive fallback. Any positive edge is enough: joining does not improve the
    //    market, so holding it to the penny bar would mean quoting almost nothing on a
    //    tight book.
    if (tradeable(own)) {
        const long em = edge_micros(side, th, *own);
        if (em > 0)
            return {"join", pc::py_round_nd(*own, 4), (double)em / MICROS, "join_touch"};
    }

    return no_quote();
}

bool should_pull(const std::string& side, OptD theo, OptD resting_price, bool alone_at_level) {
    if (!theo || !resting_price) return false;
    return alone_at_level && edge_micros(side, *theo, *resting_price) < -EDGE_MIN_MICROS;
}

bool should_improve(const std::string& side, OptD theo, OptD resting_price, const Action& action) {
    (void)theo;
    if (!action.price || !resting_price) return false;
    const bool better = side == "bid" ? (*action.price > *resting_price)
                                      : (*action.price < *resting_price);
    return better && action.edge.has_value()
           && pc::py_round_i(*action.edge * MICROS) >= EDGE_MIN_MICROS;
}

std::vector<int> tier_offsets(OptD scale_pos) {
    double u = scale_pos.value_or(0.0);
    u = (u > 0.0) ? u : 0.0;          // Python max(0.0, ...)
    u = (1.0 < u) ? 1.0 : u;          // Python min(1.0, ...)
    std::vector<int> out;
    out.reserve(2);
    for (const auto& r : TIER_OFFSET_RANGE) {
        const double lo = r[0], hi = r[1];
        out.push_back((int)pc::py_round_i(lo + (hi - lo) * u));
    }
    return out;
}

long rung_min_edge_micros(int rank, std::optional<long> min_edge_micros, OptD scale_pos) {
    const long bar = min_edge_micros.value_or(EDGE_MIN_MICROS);
    if (rank <= 0) return bar;
    const std::vector<int> offs = tier_offsets(scale_pos);
    const int n = (int)offs.size();
    int back;
    if (rank <= n) {
        back = offs[rank - 1];
    } else {
        // Ranks past the calibrated offsets keep stepping by the last one rather than flattening,
        // so a longer ladder never has two ranks wanting the same edge.
        const int step = offs[n - 1] - (n > 1 ? offs[n - 2] : 0);
        back = offs[n - 1] + (rank - n) * (step > 1 ? step : 1);   // Python max(1, ...)
    }
    return bar + (long)back * TICK_MICROS;
}

std::vector<Rung> decide_ladder(const std::string& side, OptD theo, OptD best_bid, OptD best_ask,
                                const std::vector<std::string>& counts, OptD offset,
                                std::optional<long> min_edge_micros, OptD scale_pos,
                                bool tiers_enabled, Tighten tighten) {
    const Action act = decide(side, theo, best_bid, best_ask, offset, min_edge_micros, tighten);

    std::vector<Rung> out;
    if (act.price) {
        out.push_back({0, *act.price, counts[0], act.edge, act.reason,
                       rung_min_edge_micros(0, min_edge_micros, scale_pos)});
    }
    // NOTE: no early return on a declined rung 0. That return is precisely the amplifier the
    // 2026-08-26 change removed -- it took rungs 1 and 2 down with a sub-tick theo wobble.
    if (!tiers_enabled || !theo) return out;

    const bool is_bid = (side == "bid");
    const double th = *theo;

    // The far touch for the cross guard, imputed exactly the way `decide` imputes it.
    const Touches tc = synth_touches(best_bid, best_ask);
    const OptD far = is_bid ? tc.ask : tc.bid;

    const std::vector<int> tiers = tier_offsets(scale_pos);
    const int nt = (int)tiers.size();
    for (std::size_t rank = 1; rank < counts.size(); ++rank) {
        const int r = (int)rank;
        const long need = rung_min_edge_micros(r, min_edge_micros, scale_pos);
        const bool on_touch = !out.empty() && out[0].index == 0;
        const int back = tiers[(r < nt ? r : nt) - 1];      // Python min(rank, len(tiers)) - 1
        double px;
        if (on_touch) {
            px = pc::py_round_nd(is_bid ? (out[0].price - back * TICK)
                                        : (out[0].price + back * TICK), 4);
        } else {
            px = is_bid ? pc::floor_tick(th - (double)need / MICROS)
                        : pc::ceil_tick(th + (double)need / MICROS);
        }
        // WHICHEVER IS FURTHER FROM THE THEO: the absolute floor, or one tick behind the rung in
        // front. Both halves are load-bearing.
        //
        // An order's identity on a side IS its price (see strategy/orders), so two rungs at one
        // price are one order and a lost rung -- measured on a 0.13/0.21 book with theo 0.182,
        // where rung 0 pennied to 0.20 and rank 1's floor rounded to 0.20 as well, quietly
        // shipping 40 contracts of a 60-contract ladder.
        //
        // And rung 0's price comes from `decide` -- QUOTE_OFFSET, the tighten cap, the touch --
        // not from this floor, so on a wide book it can land FURTHER out than rank 1's floor.
        // Same book, bid side: rung 0 capped to 0.15 while rank 1's floor was 0.16, i.e. the back
        // rung sitting INSIDE the front one. ladder_prices_ordered exists to catch exactly that.
        //
        // Stepping outward only ever increases the edge, so the `need` gate below still holds.
        if (!out.empty()) {
            const double prev = out.back().price;
            px = is_bid ? ((px < prev - TICK) ? px : prev - TICK)    // Python min()
                        : ((px > prev + TICK) ? px : prev + TICK);   // Python max()
        }
        px = pc::py_round_nd(px, 4);
        // Each rung is gated independently. A 5-cent-behind bid on a 3-cent market is negative
        // and must be DROPPED, not clamped -- clamping would silently stack rungs on MIN_PRICE.
        if (!tradeable(px)) continue;
        // Never cross, on the same imputed book rung 0 was priced against.
        if (far && (is_bid ? (px >= *far) : (px <= *far))) continue;
        const long em = edge_micros(side, th, px);
        // Behind a live rung 0 the bar is the pre-existing one -- any positive edge -- so this
        // path is byte-for-byte the old ladder and the A/B against the 2026-08-25 session stays
        // clean. The rank bar applies only to the theo-anchored fallback, where there is no rung
        // 0 to inherit a price from and the requirement is the only thing setting the distance.
        if (em < (on_touch ? 1 : need)) continue;
        std::string reason;
        if (on_touch) {
            reason = "tier" + std::to_string(r) + "_" + std::to_string(back) + "c_behind";
        } else {
            char buf[64];
            std::snprintf(buf, sizeof buf, "rank%d_min_edge_%.1fc", r, (double)need / 10000.0);
            reason = buf;
        }
        out.push_back({r, px, counts[rank], (double)em / MICROS, reason, need});
    }
    return out;
}

bool ladder_prices_ordered(const std::string& side, const std::vector<Rung>& rungs) {
    if (rungs.size() < 2) return true;
    for (std::size_t i = 0; i + 1 < rungs.size(); ++i) {
        const bool ok = side == "bid" ? (rungs[i].price > rungs[i + 1].price)
                                      : (rungs[i].price < rungs[i + 1].price);
        if (!ok) return false;
    }
    return true;
}

}  // namespace kp::strategy::quoting

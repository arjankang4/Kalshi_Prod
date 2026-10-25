// The quoting ladder: improve -> penny -> join, then the tiered rungs behind it.
//
// Port of Kalshi/strategy/quoting.py. Pure -- no I/O, no network, no clock --
// which is exactly what makes it the highest-value module to get right and the easiest to prove.
// The Python imports nothing but `math` and `config`, and so does this.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::strategy::quoting {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t NONE = std::nullopt;

inline constexpr double TICK = kp::pycompat::TICK;   // 0.01

// Minimum edge for pennying or joining the touch. The bar is INCLUSIVE for posting: an edge of
// exactly the bar clears it and we quote, because the threshold's job is to decide whether the
// edge is worth having. Pulling a resting quote is the mirror and stays STRICT.
inline constexpr double EDGE_MIN = 0.01;

inline constexpr long MICROS          = 1000000;
inline constexpr long EDGE_MIN_MICROS = 10000;       // EDGE_MIN, exactly, in micro-dollars

// Width imputed for the ABSENT side of a one-sided book, in ticks. A book with only a bid is
// treated as a ONE_SIDED_WIDTH-wide market rather than as a book with no width. Deliberately wide: the point
// is to let the tighten cap engage at all, not to pretend we know where the missing side would be.
inline constexpr int ONE_SIDED_WIDTH = 8;

inline constexpr long TICK_MICROS = 10000;           // one cent, in micro-dollars

struct Action {
    std::string kind;        // improve | penny | join | none
    OptD price;
    OptD edge;
    std::string reason;
};
inline Action no_quote() { return {"none", NONE, NONE, "no_positive_edge"}; }

// What we earn per contract if the theo is right. Positive is good for us.
double edge_dollars(const std::string& side, double theo, double price);

// Edge as an INTEGER, in micro-dollars.
//
// Every threshold comparison goes through this rather than through floats. Measured on the real
// boundary: theo 0.115 against a 0.11 quote gives 0.0050000000000000044 in float, so
// `edge > EDGE_MIN` is true by noise alone and the ladder pennies a market it should have joined.
// Integers make the bar mean exactly what it says -- the same reason storage/units.py
// refuses to hold a price as a float.
long edge_micros(const std::string& side, double theo, double price);

// Kalshi accepts [MIN_PRICE, MAX_PRICE] only. Not a formality: a worthless bracket has theo 0.00,
// so the bid target is floor(0.00 - 0.01) = -0.01, and on a book with no resting bids the
// empty-side branch will happily return that with "+0.01 of edge". Observed 15 times in the first
// dry run. A price the exchange cannot accept is not a quote.
bool tradeable(OptD price);

// The quoting ladder. `price` is absent when we should not quote.
//
// `min_edge_micros` is the bar rung 2 (penny) must clear; absent means EDGE_MIN_MICROS, the
// constant this ladder was written with, so a caller that passes nothing gets exactly the
// default behaviour. pricing/edge supplies a calibrated value per market instead.
//
// Note the asymmetry, which predates the edge manager and is kept: PENNY must clear the bar
// because it improves the market and takes on the obligation of being the best price, while JOIN
// only needs positive edge -- holding a join to the same bar would mean quoting almost nothing on
// a tight book.
//
// `tighten`: the sentinel is -1.0 meaning "use config::QUOTE_TIGHTEN_FRACTION", and an ABSENT
// value means the older cap-at-mid rule. Three states, so it cannot be a plain double.
struct Tighten {
    bool use_default = true;   // the -1.0 sentinel
    OptD frac;                 // when use_default is false: absent = legacy cap-at-mid
    static Tighten fraction(double f) { return {false, f}; }
    static Tighten cap_at_mid()       { return {false, NONE}; }
};

// (bid, ask) with an absent side imputed ONE_SIDED_WIDTH ticks away.
//
// Factored out of `decide` because `decide_ladder`'s back rungs need the SAME imputed far touch
// for their cross guard. Two copies would be two chances to disagree about where a missing side
// sits, and the rungs would then be guarded against a different book from the one rung 0 was
// priced against.
struct Touches { OptD bid, ask; };
Touches synth_touches(OptD best_bid, OptD best_ask);

Action decide(const std::string& side, OptD theo, OptD best_bid, OptD best_ask,
              OptD offset = NONE, std::optional<long> min_edge_micros = std::nullopt,
              Tighten tighten = Tighten{});

// Pull a resting quote that has gone wrong -- only when we are alone at our price level. If we
// are the whole level then our quote *is* that level, and leaving it there offers the market a
// price we no longer believe. With others alongside us, being wrong by less than the bar is not
// worth the queue position. STRICT, mirroring EDGE_MIN's inclusive posting bar.
bool should_pull(const std::string& side, OptD theo, OptD resting_price, bool alone_at_level);

// Amend an existing quote only for a strictly better price worth more than the bar. Amending
// forfeits queue position unless it decreases size (Kalshi's rule), so an improvement has to
// clear a real bar rather than chase every tick.
bool should_improve(const std::string& side, OptD theo, OptD resting_price, const Action& action);

// --- the tiered ladder ---------------------------------------------------------------------
//
// Sizes are FLAT: every rung carries the same count. Flat sizing makes the lot the unit of trade,
// which is what lets the inventory limits be expressed in lots. The rungs' DISTANCE behind the
// front is what scales with risk: a calm market rests near the low end of TIER_OFFSET_RANGE, one
// at the top of the distribution near the high end.
struct Rung {
    int index;
    double price;
    std::string count;         // fixed-point string, goes on the wire verbatim
    OptD edge;
    std::string reason;
    std::optional<long> min_edge;   // the rank's requirement in micro-dollars; absent = None
};

// (min_cents_behind, max_cents_behind) for rung 1 and rung 2.
inline constexpr int TIER_OFFSET_RANGE[2][2] = {{1, 3}, {3, 5}};

// [cents_behind_rung0] for rungs 1 and 2, on the EDGE curve's own scale.
//
// `scale_pos` is edge::Curve::scale_position(R) -- where R sits on the exponential-then-linear
// edge curve, NOT its percentile rank. Same scale as the edge itself, which is the point: a
// market that earns more edge also rests further back, by the same measure.
//
// Because the bulk of that curve is exponential, this compresses hard: most markets sit near the
// low end of each range and only the extreme tail reaches the high end.
//
// NOTE the rounding. Whenever lo + (hi - lo) * scale_pos lands exactly on an even-plus-half (at
// scale_pos == 0.75 the ranges evaluate round(2.5) and round(4.5)), Python's half-to-even rule
// and std::round disagree by a full cent. Uses py_round.
std::vector<int> tier_offsets(OptD scale_pos);

// The edge a rung at RANK must earn, in micro-dollars. Monotone increasing in rank.
//
// ABSOLUTE, NOT RELATIVE. This is the number that replaced "N cents behind the rung in front": an
// absolute requirement can be evaluated on one rung without reference to any other, which is
// exactly what lets rung 1 survive rung 0 being cancelled.
//
// BUILT ON THE CALIBRATED EDGE, NOT ON CONSTANTS, and the difference only shows in the tail: a
// flat per-rank bar would, on a high-risk market whose own required edge is large, demand LESS
// edge of rung 1 than of rung 0. That inverts the thing the ladder is for: a rung further from the
// theo fills only on a larger adverse move, so it must charge more. So each rank's bar is the
// market's own min_edge plus that rank's tier offset.
long rung_min_edge_micros(int rank, std::optional<long> min_edge_micros = std::nullopt,
                          OptD scale_pos = 0.0);

// The full per-side order ladder: rung 0 from `decide`, then the tiered rungs behind it.
//
// THE BACK RUNGS ARE ANCHORED TO THE THEO WHEN RUNG 0 DECLINES. `decide` gives up a whole side
// once the theo reaches the touch; if the back rungs were priced only behind rung 0, that would
// cancel every rung on the side -- including ones that still carry positive edge -- and every
// cancel forfeits queue position.
//
// ANCHOR TO RUNG 0 WHILE IT EXISTS, TO THE THEO ONLY WHEN IT DOES NOT, and that asymmetry is the
// design. Anchoring everything to the theo churns far more orders, because theo +- min_edge moves
// on every tick while rung 0's price comes from the TOUCH, which is sticky.
//
// With tiers_enabled=false and no min_edge_micros this returns exactly what `decide` returns,
// wrapped in one Rung. That identity is what test_quoting asserts before any calibrated number is
// allowed near it.
std::vector<Rung> decide_ladder(const std::string& side, OptD theo, OptD best_bid, OptD best_ask,
                                const std::vector<std::string>& counts,
                                OptD offset = NONE,
                                std::optional<long> min_edge_micros = std::nullopt,
                                OptD scale_pos = 0.0,
                                bool tiers_enabled = true,
                                Tighten tighten = Tighten{});

// True when rungs step monotonically AWAY from the touch. A safety assertion, not a filter.
bool ladder_prices_ordered(const std::string& side, const std::vector<Rung>& rungs);

}  // namespace kp::strategy::quoting

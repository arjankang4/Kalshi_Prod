// The put-call-parity forward and the at-the-money total vol, from one board snapshot.
//
// Port of Kalshi/pricing/atmvol.py.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/pricing/skew.hpp"      // Board, LegKey
#include "kp/pricing/theo_core.hpp"

namespace kp::pricing::atmvol {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;
using Board = skew::Board;

// Strikes averaged into the forward. The naive single-strike anchor is noisy at the tick level, so
// the forward is taken over this many near-money strikes.
inline constexpr int PARITY_STRIKES = 20;

// Strikes whose implied vol is medianed into the ATM number. One is enough in principle -- the
// anchor with the smallest |C-P| is by construction the most ATM -- but then the whole ladder's
// vol rides on one contract's bid/ask, and a single wide or stale quote misprices every rung of
// the board at once. A median over several tolerates a minority of bad ones.
inline constexpr int ATM_VOL_STRIKES = 4;

// Health gates, both relative to w itself.
//   - call and put at the same strike must imply nearly the same vol. They are solved against a
//     common F, so a large gap means the board is not internally consistent.
//   - the k near-money strikes must agree with each other, or the smile is steep enough here
//     that a single flat number is not describing the board.
inline constexpr double MAX_CP_VOL_SPREAD  = 0.2;
inline constexpr double MAX_VOL_DISPERSION = 0.15;

// Is this quote a genuine two-sided market?
//
// Stricter than theo::quote_value, deliberately, and the strictness is load-bearing. Massive
// omits bp/bs entirely when there is no bid, and quote_value correctly reads that as 0.00 --
// the right call for pricing a wing, fatal for anchoring parity. A deep-ITM call whose bid has
// gone prices to 0.00, which makes |C - P| smallest at *its* strike, which wins the anchor and
// implies S at that strike. Measured on real tape: spot 7387.5 on a day whose true spot was 7706.
bool two_sided(const theo::Quote& q, bool require_ask_size = true);

// One (strike, call, put) triple that survived the two-sided test. Ordered as the Python tuple
// (abs_cp, strike, implied_S, C, P) so the lexicographic sort below is transcribable.
struct Combo { double abs_cp, strike, implied_s, c, p; };

std::vector<Combo> combos(const Board& quotes, const std::vector<double>& strikes);

// Spacing of the strike grid near `centre`. Local rather than the board-wide minimum: SPX lists
// 5-wide near the money and up to 200-wide in the tails.
double grid_step(std::vector<double> strikes, double centre);

struct ParityDetail {
    std::string reason;
    OptD anchor_strike, naive_forward, anchor_cp;
    int n_parity = 0;
};

// Put-call-parity forward to 16:00, as a strike-distance-weighted average.
//
// Anchor on the smallest |C - P| -- the most ATM strike, where both legs carry the most time
// value and the least bid/ask noise per dollar -- then average the implied forwards of the 25
// nearest strikes around it.
//
// Weight is 1/sqrt(max(|K - F0| / step, 1/2)): nearer strikes count for more, falling off as the
// inverse square root of the distance in grid STEPS rather than points, so the same rule travels
// from SPX's 5-wide grid at 7,700 to NDX's 10-wide at 29,700 without retuning. The 1/2 floor
// keeps a strike sitting exactly on F0 from taking infinite weight.
//
// NOTE: the weight is `** 0.5`, i.e. std::pow, NOT std::sqrt -- they differ on ~0.086% of values.
std::pair<OptD, ParityDetail> parity_forward(const Board& quotes,
                                             const std::vector<double>& strikes);

struct VolDetail {
    std::string reason;
    int n_vol = 0;
    OptD cp_spread, dispersion, w_anchor;
    std::vector<double> strikes_used;
};

// Median implied total vol over the `k` most-ATM two-sided strikes.
//
// Each leg is inverted against the *common weighted forward*, not against its own strike's
// implied forward. That is what turns the call/put disagreement into a usable signal: solved
// against a shared F, a C and a P individually mispriced in opposite directions still recover
// nearly the same vol (checked with the legs pushed +-20%: 0.00345 against 0.00350 true), so a
// large cp_spread means the board is genuinely inconsistent rather than merely noisy.
std::pair<OptD, VolDetail> atm_total_vol(const Board& quotes, const std::vector<double>& strikes,
                                         double forward, int k = ATM_VOL_STRIKES);

struct AtmFit {
    OptD forward, w;
    std::string reason;
    OptD anchor_strike, naive_forward;
    int n_parity = 0, n_vol = 0;
    OptD cp_spread, dispersion, w_anchor;
    std::vector<double> strikes_used;
    std::string forward_source;
    OptD parity_forward_, index_minus_parity;
    bool ok() const { return forward.has_value() && w.has_value() && reason.empty(); }
};

// quotes + strike grid -> AtmFit.
//
// THE PARITY FORWARD IS PRIMARY AND THE INDEX IS ONLY A FALLBACK. That is the opposite of the
// daily board (pricing/spot), and the asymmetry was measured, not assumed. The parity forward is
// BY CONSTRUCTION the forward at which call- and put-implied vols agree, which is exactly what
// cp_spread measures and MAX_CP_VOL_SPREAD gates -- so cp_spread is really a forward-error
// detector, and imposing any externally-sourced level inflates it. Sweeping the forward around
// parity on a live SPXW strip:
//
//     offset   -2.0    -1.5    -1.0     0.0    +1.0    +1.5    +2.0
//     cp_spr  0.360   0.256   0.163   0.004   0.155   0.247   0.352
//     ok      False   False    True    True    True    True   False
//
// The gate tolerates about +-1.75 points, and two minutes of skew between the option snapshot
// and the index snapshot is worth roughly that -- so substituting the index here would spuriously
// reject healthy boards as cp_vol_spread. The index still earns its place: index_minus_parity is
// recorded on every fit as the health signal, and index_forward is used when parity fails
// outright, which is the case that previously had no answer at all.
AtmFit fit(const Board& quotes, const std::vector<double>& strikes, bool gate = true,
           OptD index_forward = std::nullopt);

}  // namespace kp::pricing::atmvol

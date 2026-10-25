// Maps Kalshi bracket boundaries -> the SPXW option tickers needed to price them.
//
// Port of the pure half of Kalshi/pricing/universe.py. `option_root_and_strikes`
// and `load_event_markets` are NOT here -- the first is a Massive REST call and the second reads a
// recon capture off disk; both belong to kp_net and the app respectively.
#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "kp/massive/snapshot.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/pricing/theo_core.hpp"

namespace kp::pricing::universe {

// Massive snapshot {sym: Q} -> {('C'|'P', strike): Q} for the pricers.
//
// Filters the collector's board (which spans BOTH strips on one socket) down to one root/expiry
// and the given strikes. A symbol absent from the snapshot, or present with an empty frame
// (Python `if q:` on an empty dict; `present == false` here), is simply not in the board.
skew::Board quotes_by_strike(const kp::massive::OptionSnapshot& snapshot, const std::string& root,
                             const std::string& expiry, const std::vector<double>& strikes);

using OptD = std::optional<double>;

// Massive caps a connection at 1000 subscribed tickers and permits ONE options connection per
// account, so the subscription is a budget to be managed, not a set-and-forget. Mirrors
// massive.client.QuoteCollector.MAX_TICKERS -- kept here so pricing/ need not see the collector.
inline constexpr int MAX_TICKERS = 1000;

// "O:SPXW260819C07700000" -- strike is 8 digits of strike*1000.
std::string occ_ticker(const std::string& root, const std::string& expiry_yymmdd,
                       char right, double strike);

// The strikes every boundary prices from. PINNED for the whole session: they already span the
// entire Kalshi board -- measured 2026-08-20, +-4.6% on SPX and +-4.8% on NDX -- so bracket
// pricing survives a large intraday move without re-centring. Only the parity band follows spot.
//
// Throws if a boundary has no bracketing strikes, exactly as the Python raises RuntimeError. This
// is one of the two places in the port where an exception is right: a boundary we cannot bracket
// is a market we cannot price at all, and returning an empty set would quietly quote nothing.
std::set<double> bracketing_strikes(const std::vector<theo::Market>& markets,
                                    const std::vector<double>& strikes);

// Listed strikes within +-band_pct of `centre`, for the put-call parity spot.
//
// Percent, not a strike count, because a fixed count means wildly different coverage per board:
// 50 strikes is +-3.3% on SPX's 5-wide near-money grid but only +-0.85% on NDX, whose grid is also
// 5-wide at the money but sits at ~29,400 instead of ~7,675. Percent travels.
std::set<double> parity_band(const std::vector<double>& strikes, OptD centre,
                             OptD band_pct = std::nullopt);

struct Desired {
    std::vector<double> strikes;      // sorted
    std::vector<std::string> tickers; // C then P per strike, strikes ascending
};

// Pinned bracketing strikes + a parity band around `spot`, as option tickers.
//
// Trims the band from the outside in if the total would breach the connection cap -- the pinned
// set is never sacrificed, because losing a bracketing strike silently unprices a market whereas a
// narrower band only weakens the spot estimate.
//
// ON THE TRIM'S TIE-BREAK. The Python sorts `band - pinned` by distance from centre alone, over a
// SET, so two equidistant strikes are ordered by CPython's set iteration -- and if the cut landed
// between them, which one survived would be hash order. This port breaks the tie on the strike.
//
// MEASURED, not assumed: equidistant strikes are COMMON (a symmetric grid around a centre on the
// grid produces a tie at every distance), but across the whole gen_universe sweep the cut never
// once lands between two tied strikes, so the SELECTED SET is identical either way. The generator
// re-checks that on every run by computing both selections and comparing them -- it compares
// selections, not merely whether a tie exists, because the tie on its own is harmless.
//
// It also cannot arise live: the trim fires only above 500 strikes and the live board lists ~143.
Desired desired_tickers(const std::vector<theo::Market>& markets,
                        const std::vector<double>& strikes,
                        const std::string& root, const std::string& expiry,
                        OptD spot, OptD band_pct = std::nullopt,
                        std::optional<int> cap = std::nullopt);

}  // namespace kp::pricing::universe

// Tuned constants from the reference Kalshi/config.py.
//
// Values here mirror the reference config.py (whose tuned values are illustrative in the public
// copy). Changing one here without changing it there breaks parity silently, so the golden
// vectors pin the ones that reach a price, and the `config_fresh` test checks every name this
// file shares with config.py.
//
// Secrets and paths are NOT here -- they are read at runtime from the reference tree (see
// kp/config_env.hpp). Kalshi_Prod deliberately has no var/ of its own: copying artifacts is how
// two trees diverge.
#pragma once
#include <array>
#include <string_view>
#include "kp/config_gen.hpp"   // the mechanical half: every other scalar in config.py

namespace kp::config {

// --- market identity --------------------------------------------------------------------
inline constexpr std::string_view SERIES_TICKER    = "KXINX";
inline constexpr std::string_view KALSHI_CATEGORY  = "Financials";  // "Finance" is not one
inline constexpr std::string_view OPTION_UNDERLYING = "SPX";        // tickers come back as SPXW
inline constexpr std::string_view STOCK_SYMBOL     = "SPY";

// --- quoting ----------------------------------------------------------------------------
inline constexpr double QUOTE_OFFSET = 0.02;   // bid = theo - offset, offer = theo + offset

// Half-width of the put-call parity band, as a FRACTION OF SPOT. Percent rather than a strike
// count, because a count means different things per board: the same number of strikes covers a
// much narrower range on NDX, which has the same 5-wide near-money grid at a far higher level.
inline constexpr double QUOTE_BAND_PCT = 0.02;

// Past this age the index level is not a spot any more. The quote cycle re-reads it every pass,
// so an age approaching this bound means the feed stalled rather than merely lagged.
inline constexpr double INDEX_MAX_STALE_S = 5.0;

// An ALARM, not a gate. Parity legitimately disagrees with the index by a few points when the
// board is thin, and refusing to quote on that would hand the session to the noise.
inline constexpr double INDEX_PARITY_ALARM_PCT = 0.001;

// How far into a wide market to tighten when our target would improve the touch: the fraction is
// of the FULL NBBO width, measured from the near touch (target = near + (far - near) * fraction,
// then rounded to the tick on the passive side).
inline constexpr double QUOTE_TIGHTEN_FRACTION = 0.5;

// Contract counts per rung, front first. The tier machinery works for longer ladders -- adding a
// rung is an edit here and in config.py. Fixed-point contract-count STRINGS -- they go on the wire
// verbatim, so "10" or "10.0" would be a different request body.
inline constexpr std::array<std::string_view, 1> QUOTE_TIER_COUNTS = {"10.00"};
inline constexpr std::string_view ORDER_COUNT = "5.00";

inline constexpr bool TIERS_ENABLED        = true;
inline constexpr bool EDGE_MANAGER_ENABLED = true;
inline constexpr double EDGE_RECOMPUTE_S   = 20.0;

// --- the session loss limit ------------------------------------------------------------
// Every other limit in this file bounds EXPOSURE -- contracts per side, contracts in total,
// collateral per market and across the book, net inventory in lots. None of them bounds LOSS. A
// model wrong in a consistent direction fills inside all of them, all day.
inline constexpr double DAILY_LOSS_LIMIT_DOLLARS = 500.0;
// The halt switch is a FILE, not a flag or a signal, because the operator using it is in a
// different shell from the process and possibly on a different day. `touch var/HALT` needs no
// IPC and no PID, survives a wedged loop, and is visible to anyone who lists the directory.
inline constexpr const char* HALT_FILE_REL = "var/HALT";

// --- the band --------------------------------------------------------------------------
// Which markets are near enough the money to be worth quoting in DEPTH. Measured in sigma, not
// strikes, because a strike count means very different things on the wide daily brackets and the
// narrow hourly rungs of the same underlying.
inline constexpr double BAND_SIGMA = 2.5;
// Hysteresis. Entering the band costs extra rungs and leaving it cancels them, which on a
// price-time exchange is queue position -- so an incumbent is held to a boundary this much wider
// before it is dropped.
inline constexpr double BAND_EXIT_MULT = 1.5;

// Contracts per lot, and the net inventory bound in lots, in-band and in the wing.
inline constexpr int LOT_SIZE = 10;
inline constexpr int POSITION_LIMIT_LOTS_BAND = 4;
inline constexpr int POSITION_LIMIT_LOTS_WING = 2;

// Never quote a market inside this many minutes of its own settlement. Not a nicety: once the
// next minute reaches settlement the contract resolves, so its theta is (payoff - price), not a
// small decay.
inline constexpr double MIN_MINUTES_TO_CLOSE = 5.0;

// REQUOTE_TICKS deliberately does NOT live here. It is a derived view of
// orders.FRONT_IMPROVE_TICKS / BACK_IMPROVE_TICKS upstream; this port keeps the same single
// source of truth and declares the two constants in kp/strategy/orders.hpp.

// --- the realized-vol lean ---------------------------------------------------------------
// The lean does not move the PRICE -- it moves the required EDGE (pricing.lean.edge_adjust), so
// the theo stays entirely the implied-vol digital and the worst case is quoting wider on one side
// and tighter on the other rather than putting a level on the wire the implied model never chose.
// side_offset / clamp_price remain in the reference for analysis and are OFF the live path.
//
// Consequence for this port: rvol/ cannot be stubbed. rvol/{live,features,model} feed
// strategy/leanchain on a LEAN_RECOMPUTE_S cadence and are the only numpy on the live path.
//
// LEAN_VRP_RATIO is None, and that is a real property rather than an oversight: uncentred, the
// lean harvests the variance risk premium, a deliberate short-vol posture.
inline constexpr double LEAN_VOL_SCALE  = 0.02;
inline constexpr double LEAN_RECOMPUTE_S = 3.0;
inline constexpr bool   LEAN_ENABLED    = true;
inline constexpr bool   LEAN_VRP_RATIO_IS_NONE = true;

// The smile-slope term in the digital: P(S>K) = N(d2) - F*phi(d1)*dw/dK. An A/B SWITCH, NOT A
// ROLLOUT GATE -- it defaults on in the reference. Turning it off reverts to the plain N(d2)
// digital, which omits the smile slope; pricing/validate_digital fails loudly with it false.
inline constexpr bool DIGITAL_SLOPE_TERM = true;

// --- volatility skew ----------------------------------------------------------------------
inline constexpr double SKEW_RECOMPUTE_S = 3.0;
inline constexpr double SKEW_WINDOW_S    = 60.0;
inline constexpr double SKEW_HALF_LIFE_S = 15.0;

// --- risk limits ---------------------------------------------------------------------------
// Floors under the worst-case resting exposure that rungs x sides x markets can reach. The
// per-side cap is sized as exactly one three-rung ladder at LOT_SIZE.
inline constexpr double MAX_RESTING_CONTRACTS_PER_MARKET_SIDE = 30;   // one 3-rung ladder at LOT_SIZE
inline constexpr double MAX_RESTING_CONTRACTS_TOTAL           = 4000;
inline constexpr double MAX_RESTING_ORDER_VALUE               = 2000.0;  // dollars
inline constexpr double MIN_PRICE = 0.01;
inline constexpr double MAX_PRICE = 0.99;

inline constexpr long MICROS_PER_DOLLAR = 1000000;
inline constexpr int  CC_PER_CONTRACT   = 100;

}  // namespace kp::config

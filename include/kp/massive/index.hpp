// The live index level, as a value.
//
// Port of the PURE half of Kalshi/massive/index.py. `snapshot()` -- the
// /v3/snapshot/indices REST call -- is not here: it belongs to kp_net, and splitting the type from
// the fetch is what lets pricing/spot be golden-tested with no network. The Python keeps both in
// one module because an import is free; a link edge is not.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace kp::massive::index {

using OptD = std::optional<double>;

// Beyond this the level is not a spot any more. The live quote cycle re-reads every few seconds,
// so anything approaching this bound means the feed has stalled rather than merely lagged.
inline constexpr double DEFAULT_MAX_STALE_S = 5.0;

inline constexpr long long NS = 1000000000LL;

// Epoch nanoseconds since the Unix epoch, from the system clock.
long long now_ns();

// One index level with its vendor timestamp. `ts_ns` is epoch NANOSECONDS.
//
// Note the unit trap the reference has hit twice: /v3/snapshot `last_updated` and /v3/quotes
// `sip_timestamp` are nanoseconds, while /v2/aggs `t` and the Massive WebSocket `t` are
// milliseconds. Same vendor, same concept, different unit.
struct IndexQuote {
    std::string underlying;
    std::string ticker;
    OptD value;
    std::optional<long long> ts_ns;
    std::optional<std::string> market_status;
    std::optional<std::string> name;

    // Seconds since the vendor stamped this level, or absent if it carries no timestamp.
    //
    // `at_ns` absent means "read the clock", matching the Python default. Every caller inside a
    // priced cycle should pass one instead: the Python reads the clock TWICE per resolve() -- once
    // in age_s and once inside is_fresh -- so two microseconds of drift can in principle land the
    // recorded age and the freshness verdict on opposite sides of the bound. Passing an instant
    // makes the whole resolution one observation, and makes it replayable.
    OptD age_s(std::optional<long long> at_ns = std::nullopt) const;

    bool is_fresh(double max_stale_s = DEFAULT_MAX_STALE_S,
                  std::optional<long long> at_ns = std::nullopt) const;
};

}  // namespace kp::massive::index

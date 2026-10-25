#include "kp/pricing/spot.hpp"

#include <cstdio>

#include "kp/config.hpp"

namespace kp::pricing::spot {

namespace ix = kp::massive::index;

namespace {

SpotResolution make(OptD value, std::string source, OptD index, OptD parity, OptD index_age_s,
                    std::optional<std::string> market_status, std::string reason) {
    SpotResolution r;
    r.value = value;
    r.source = std::move(source);
    r.index = index;
    r.parity = parity;
    r.index_age_s = index_age_s;
    r.market_status = std::move(market_status);
    r.reason = std::move(reason);
    // Python: `if index is not None and parity is not None and parity > 0`. Note it is `> 0`,
    // not truthiness, so a parity of exactly 0.0 leaves divergence absent rather than dividing.
    if (index && parity && *parity > 0.0) {
        r.divergence = *index - *parity;
        r.divergence_pct = *r.divergence / *parity;
        const double d = *r.divergence_pct;
        r.alarm = (d < 0.0 ? -d : d) > kp::config::INDEX_PARITY_ALARM_PCT;
    }
    return r;
}

}  // namespace

SpotResolution resolve(OptD parity_spot,
                       const std::optional<ix::IndexQuote>& index_quote,
                       OptD max_stale_s,
                       std::optional<long long> at_ns) {
    const double stale = max_stale_s ? *max_stale_s : kp::config::INDEX_MAX_STALE_S;

    OptD idx_val, age;
    std::optional<std::string> status;
    if (index_quote) {
        idx_val = index_quote->value;
        status = index_quote->market_status;
        age = index_quote->age_s(at_ns);
    }

    if (index_quote && index_quote->is_fresh(stale, at_ns))
        return make(idx_val, "index", idx_val, parity_spot, age, status, "");

    std::string reason;
    if (!index_quote) {
        reason = "no_index_quote";
    } else if (!age) {
        reason = "index_no_timestamp";
    } else if (*age < 0.0) {
        // A negative age means our clock and the vendor's disagree. Treating it as fresh would
        // hide a clock skew that also corrupts every other age check in the process.
        reason = "index_timestamp_in_future";
    } else {
        char buf[64];
        std::snprintf(buf, sizeof buf, "index_stale_%.1fs", *age);
        reason = buf;
    }

    if (parity_spot)
        return make(parity_spot, "parity", idx_val, parity_spot, age, status, reason);

    return make(MISSING, "", idx_val, MISSING, age, status, reason + "+no_parity");
}

}  // namespace kp::pricing::spot

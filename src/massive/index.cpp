#include "kp/massive/index.hpp"

#include <chrono>

namespace kp::massive::index {

long long now_ns() {
    // Python: int(datetime.now(timezone.utc).timestamp() * NS). float seconds scaled by 1e9, so
    // the low digits are float noise either way; system_clock is the same instant to any precision
    // that matters against a 10-second staleness bound.
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

OptD IndexQuote::age_s(std::optional<long long> at_ns) const {
    if (!ts_ns) return std::nullopt;
    const long long t = at_ns ? *at_ns : now_ns();
    return (double)(t - *ts_ns) / (double)NS;
}

bool IndexQuote::is_fresh(double max_stale_s, std::optional<long long> at_ns) const {
    const OptD age = age_s(at_ns);
    return age.has_value() && 0.0 <= *age && *age <= max_stale_s;
}

}  // namespace kp::massive::index

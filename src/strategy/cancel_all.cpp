#include "kp/strategy/cancel_all.hpp"

#include <chrono>
#include <cstdio>
#include <ostream>

#include "kp/storage/pyerr.hpp"
#include "kp/time/et.hpp"

namespace kp::strategy::cancel_all {

using kp::json::Value;

const std::vector<std::string> DAILY_SERIES = {"KXINX", "KXNASDAQ100", "KXINXDUD", "KXNASDAQDUD"};
const std::vector<std::string> HOURLY_SERIES = {"KXINXU", "KXNASDAQ100U"};
const std::vector<int> HOURS = {1000, 1100, 1200, 1300, 1400, 1500, 1600};

std::vector<std::string> today_events(int year, int month, int day, const std::vector<std::string>& daily,
                                      const std::vector<std::string>& hourly, const std::vector<int>& hours) {
    static const char* MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    char stamp[16];
    std::snprintf(stamp, sizeof stamp, "%02d%s%02d", year % 100, MON[month - 1], day);
    std::vector<std::string> out;
    for (const auto& s : daily) out.push_back(s + "-" + stamp + "H1600");
    for (const auto& s : hourly)
        for (int h : hours) out.push_back(s + "-" + stamp + "H" + std::to_string(h));
    return out;
}

std::vector<std::string> today_events() {
    kp::time::init();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto p = kp::time::to_et(kp::time::Instant{ns});
    return today_events(p.year, p.month, p.day);
}

std::vector<Value> resting_across(kp::kalshi::Client& k, const std::vector<std::string>& events) {
    std::vector<Value> out;
    for (std::size_t i = 0; i < events.size(); i += MAX_EVENTS_PER_QUERY) {
        std::string q;
        for (std::size_t j = i; j < std::min(events.size(), i + MAX_EVENTS_PER_QUERY); ++j) {
            if (j > i) q += ",";
            q += events[j];
        }
        const Value r = k.resting_orders({{"event_ticker", kp::kalshi::P(q)}});
        const Value* orders = r.find("orders");
        if (orders && orders->is_array()) for (const auto& o : orders->arr()) out.push_back(o);
    }
    return out;
}

long cancel_events(kp::kalshi::Client& k, const std::vector<std::string>& events, bool verbose,
                   std::ostream& out, std::ostream& err) {
    if (events.empty()) return 0;
    const auto orders = resting_across(k, events);
    if (verbose)
        out << orders.size() << " resting order(s) across " << events.size() << " event(s) in "
            << (events.size() + MAX_EVENTS_PER_QUERY - 1) / MAX_EVENTS_PER_QUERY << " quer(ies)\n";
    long failed = 0;
    for (const auto& o : orders) {
        const Value* oid_v = o.is_object() ? o.find("order_id") : nullptr;
        if (!oid_v) throw kp::storage::PyError("KeyError", "'order_id'");
        const std::string oid = kp::storage::py_str(*oid_v);
        try {
            k.cancel_order(oid);
            if (verbose) {
                // o.get("price_dollars", o.get("yes_price"))
                const Value* pd = o.find("price_dollars");
                const Value* yp = o.find("yes_price");
                const Value px = pd ? *pd : (yp ? *yp : Value());
                const Value* t = o.find("ticker");
                const Value* s = o.find("side");
                out << "  cancelled " << oid << "  " << (t ? kp::storage::py_str(*t) : "None") << " "
                    << (s ? kp::storage::py_str(*s) : "None") << " @ " << kp::storage::py_str(px) << "\n";
            }
        } catch (const kp::kalshi::KalshiError& e) {
            ++failed;
            err << "  ! " << oid << ": " << e.what() << "\n";
        }
    }
    const long left = (long)resting_across(k, events).size();
    if (verbose) {
        out << "remaining resting: " << left;
        if (failed) out << " (" << failed << " cancel error(s))";
        out << "\n";
    }
    return left;
}

}  // namespace kp::strategy::cancel_all

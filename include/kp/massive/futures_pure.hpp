// CME futures: the PURE half of massive/futures.py -- product codes, the quarterly contract cycle,
// the front-month selection rule, and the session-window arithmetic.
//
// Everything that opens a connection (`_get`, `aggs`, `session_volume`, `closes_ms`) belongs to the
// massive I/O port. `front_month` is here with its one network dependency -- the per-contract RTH
// volume -- INJECTED, because the selection rule is what has to match the reference exactly:
//
//   * candidates come from the CME calendar (third Friday of Mar/Jun/Sep/Dec), NOT from
//     /futures/v1/contracts, which was measured omitting ESM6/ESU6 on 2026-03-27;
//   * the incumbent is re-probed first when it is not among them;
//   * the winner is the STRICTLY highest volume (a tie keeps the earlier candidate);
//   * below MIN_FRONT_VOLUME nothing traded meaningfully and the answer is "none".
//
// The roll is the day volume crosses over, not a calendar rule -- measured 2024-09-16: ESU4
// 365,541 vs ESZ4 552,300. Callers must drop the return spanning a roll.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kp::massive::futures {

// Kalshi underlying -> CME product code.
std::string product_for(const std::string& underlying);   // PRODUCT.get(u, u)
std::optional<std::string> product(const std::string& underlying);   // PRODUCT.get(u)
// Local cache ticker per underlying (rvol/data layout).
std::optional<std::string> cache_ticker(const std::string& underlying);

// The only resolutions /futures/v1/aggs accepts; anything else is an HTTP 400.
inline constexpr std::array<const char*, 4> RESOLUTIONS = {"1sec", "1min", "1second", "1minute"};
bool valid_resolution(const std::string& r);

inline constexpr long PAGE_LIMIT = 50000;                        // the aggs page; never paginate
inline constexpr long long MIN_SPLIT_NS = 60LL * 1000000000LL;   // below this a full page is a fault
inline constexpr double FUTURES_RATE_PER_SEC = 10.0;
inline constexpr long long NS_PER_MS = 1000000;
inline constexpr long MIN_FRONT_VOLUME = 1000;

// CME Globex: 18:00 ET -> 17:00 ET next day, dark 17:00-18:00 ET. (hour, minute)
inline constexpr std::pair<int, int> HALT_START{17, 0};
inline constexpr std::pair<int, int> HALT_END{18, 0};

inline constexpr std::array<int, 4> QUARTERLY_MONTHS = {3, 6, 9, 12};
// MONTH_CODE: H=3 M=6 U=9 Z=12. 0 / '\0' when not a quarterly code / month.
int month_for_code(char code);
char code_for_month(int month);

// A calendar date (the reference's datetime.date).
struct Date {
    int year = 1970, month = 1, day = 1;
    bool operator<(const Date& o) const {
        return year != o.year ? year < o.year : month != o.month ? month < o.month : day < o.day;
    }
    bool operator==(const Date& o) const { return year == o.year && month == o.month && day == o.day; }
    bool operator>=(const Date& o) const { return !(*this < o); }
    std::string iso() const;   // "YYYY-MM-DD"
};
long days_from_civil(const Date& d);      // days since 1970-01-01
Date civil_from_days(long z);
int weekday(const Date& d);               // Monday = 0 ... Sunday = 6, as date.weekday()

// 'ESU4' -> ('ES', 9, 2024). The decade is anchored on `today_year` (the reference reads
// date.today().year; it is a parameter here). MISSING for a calendar spread ('ESH5-ESM5') or
// anything else that is not one outright. Python's `$` also matches before one trailing '\n'.
struct Parsed { std::string product; int month; int year; };
std::optional<Parsed> parse_ticker(const std::string& ticker, int today_year);

// The third Friday of a month -- CME's equity-index delivery date.
Date third_friday(int year, int month);

// The next `n` quarterly contracts delivering on or after `day`, nearest first: (ticker, delivery).
std::vector<std::pair<std::string, Date>> quarterly_candidates(const std::string& product_code,
                                                               const Date& day, int n = 3);

// RTH volume for one contract on one ET date -- futures.session_volume, which is a network call.
using SessionVolumeFn = std::function<double(const std::string& ticker, const Date& day)>;

// The most-traded outright on ET date `day`, or MISSING if nothing reached MIN_FRONT_VOLUME.
std::optional<std::string> front_month(const std::string& product_code, const Date& day,
                                       const SessionVolumeFn& session_volume,
                                       int n_candidates = 3,
                                       const std::optional<std::string>& incumbent = std::nullopt);

// int(datetime.combine(day, time(h, m), tzinfo=ET).timestamp() * 1e9) -- the aggs window bound.
long long et_ns(const Date& day, int hour, int minute);
// (09:30 ET, 16:00 ET) of `day`, the window session_volume sums over.
std::pair<long long, long long> rth_window_ns(const Date& day);

}  // namespace kp::massive::futures

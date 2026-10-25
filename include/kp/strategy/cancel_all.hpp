// strategy/cancel_all.py -- cancel every resting order on today's index events. The KILL PATH.
//
// Its rules, each a failure it has already had: (a) name EVERY event we could have quoted -- the
// four daily series at H1600 and every hour of both hourly series, 18 events; (b) CHUNK the
// MultipleEventTickerQuery at 10, never truncate; (c) verify by RE-READING, not by trusting the
// cancel responses. No write budget gates it: the client charges the bucket and the bucket may go
// negative -- an order we cannot cancel because our own bookkeeping said no is a real position.
//
// cancel_events SENDS CANCELS. It is only ever exercised against a fake transport in tests.
#pragma once
#include <iosfwd>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/kalshi/client.hpp"

namespace kp::strategy::cancel_all {

inline constexpr std::size_t MAX_EVENTS_PER_QUERY = 10;
extern const std::vector<std::string> DAILY_SERIES;    // KXINX KXNASDAQ100 KXINXDUD KXNASDAQDUD
extern const std::vector<std::string> HOURLY_SERIES;   // KXINXU KXNASDAQ100U
extern const std::vector<int> HOURS;                   // 1000 .. 1600

// f"{s}-{d:%y%b%d}".upper() + "H1600" for the daily series, then every (hourly series, hour).
std::vector<std::string> today_events(int year, int month, int day,
                                      const std::vector<std::string>& daily = DAILY_SERIES,
                                      const std::vector<std::string>& hourly = HOURLY_SERIES,
                                      const std::vector<int>& hours = HOURS);
// Today's ET date.
std::vector<std::string> today_events();

// Every resting order across `events`, one query per chunk of 10 (comma-joined event_ticker).
std::vector<kp::json::Value> resting_across(kp::kalshi::Client& k, const std::vector<std::string>& events);

// Cancel them all; returns how many are STILL resting on a re-read. Prints what the reference
// prints (stdout when verbose; a failed cancel to `err` always). Only KalshiError is absorbed per
// order, as in the reference -- a transport failure propagates.
long cancel_events(kp::kalshi::Client& k, const std::vector<std::string>& events, bool verbose,
                   std::ostream& out, std::ostream& err);

}  // namespace kp::strategy::cancel_all

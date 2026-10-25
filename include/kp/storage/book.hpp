// storage/book_replay.Book: one market's resting interest, rebuilt from snapshot + deltas.
//
// Both ladders are BID stacks in their own side's terms (a `no` level at p is a YES offer at 1-p),
// prices int64 micro-dollars, sizes int64 centi-contracts. The one rule that must survive porting:
// a delta that would drive a level NEGATIVE throws (PyError PrecisionError-free ValueError), never
// clamps -- a negative size is arithmetic proof of a missed frame, and a floored book is wrong while
// looking right.
//
// std::map is the ordered ladder `sorted(b.yes.items())` produces, so reading is already ascending.
// Mutation ORDER on a malformed snapshot is the reference's too (both sides cleared first, then
// filled level by level until the bad one throws), so a half-loaded book looks the same both ways.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "kp/json/value.hpp"
#include "kp/storage/pyerr.hpp"

namespace kp::storage {

class Book {
public:
    std::map<std::int64_t, std::int64_t> yes, no;
    std::optional<std::int64_t> snapshot_ns, snapshot_seq;
    long n_deltas = 0;

    // `msg` is the snapshot body: yes_dollars_fp / no_dollars_fp as [[price, size], ...]; an ABSENT
    // side is empty (WS omits the key rather than sending []).
    void load(const kp::json::Value& msg, std::optional<std::int64_t> recv_ns,
              std::optional<std::int64_t> seq);
    // `msg` is the delta body: side, price_dollars, delta_fp. Throws PyError ValueError on a negative
    // result, KeyError on a missing field, AttributeError on an unknown side.
    void apply(const kp::json::Value& msg);
    // (best_bid, best_ask) in YES micro-dollars.
    std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>> best() const;
};

}  // namespace kp::storage

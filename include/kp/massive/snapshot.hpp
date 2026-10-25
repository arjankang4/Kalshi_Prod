// The option board as the Massive quote collector holds it: {OCC symbol: latest Q frame}.
//
// The reference's `QuoteCollector.snapshot()` returns a copy of a dict of raw Q-frame dicts; the
// pricing path reads only bp/ap/bs/as off each, so the C++ keeps exactly those as a theo::Quote
// (present=true) plus the vendor timestamp. `universe::quotes_by_strike` turns this into the
// {(right, strike): Quote} skew::Board every pricer takes.
#pragma once
#include <optional>
#include <string>
#include <unordered_map>

#include "kp/pricing/theo_core.hpp"

namespace kp::massive {

struct OptionQuote {
    kp::pricing::theo::Quote q;         // bp, ap, bs, as as Massive sent them; present = true
    std::optional<long long> t_ms;      // the frame's `t` (vendor ms), if it carried one
};

using OptionSnapshot = std::unordered_map<std::string, OptionQuote>;

}  // namespace kp::massive

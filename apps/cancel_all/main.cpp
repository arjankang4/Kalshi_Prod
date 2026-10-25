// Cancel every resting order on today's index events -- strategy/cancel_all.main, in C++.
//
//     KP_ALLOW_LIVE=1 cancel_all [--kalshi-root DIR]
//
// THIS SENDS CANCELS; the reference has no dry mode. It therefore sits behind the same second
// switch as quote_loop --live: without KP_ALLOW_LIVE=1 it refuses and names the Python kill path,
// which has been exercised against the real exchange. The logic is kp::strategy::cancel_all
// (today_events -> chunked resting_across -> cancel -> VERIFY BY RE-READING), tested against a fake
// exchange only.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "kp/kalshi/client.hpp"
#include "kp/strategy/cancel_all.hpp"
#include "kp/time/et.hpp"

int main(int argc, char** argv) {
    std::string root = KP_KALSHI_ROOT_DEFAULT;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--kalshi-root" && i + 1 < argc) root = argv[++i];
        else { std::fprintf(stderr, "usage: cancel_all [--kalshi-root DIR]\n"); return 2; }
    }
    const char* allow = std::getenv("KP_ALLOW_LIVE");
    if (allow == nullptr || std::string(allow) != "1") {
        std::fprintf(stderr,
            "[cancel_all] *** REFUSING: this sends cancels and the C++ path has never sent one. Use\n"
            "[cancel_all] ***   cd %s && python -m strategy.cancel_all\n"
            "[cancel_all] *** or set KP_ALLOW_LIVE=1 to run this one deliberately. ***\n", root.c_str());
        return 2;
    }
    kp::time::init();
    kp::kalshi::Client k(kp::kalshi::Client::from_env(root));
    const auto events = kp::strategy::cancel_all::today_events();
    std::cout << "[cancel_all] " << events.size() << " candidate events: " << events.front() << " .. "
              << events.back() << std::endl;
    return kp::strategy::cancel_all::cancel_events(k, events, true, std::cout, std::cerr) == 0 ? 0 : 1;
}

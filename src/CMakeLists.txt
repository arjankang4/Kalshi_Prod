# kp_base: the compatibility layer everything else is built on. No I/O, no config.
#
# massive/index.cpp is the PURE half of the vendor index feed -- the IndexQuote value type and its
# staleness arithmetic, nothing that opens a socket. It lives here rather than in kp_net so that
# pricing/spot, which consumes it, stays network-free and golden-testable. The /v3/snapshot fetch
# will be a separate TU in kp_net.
add_library(kp_base STATIC
    pycompat/round.cpp
    pycompat/repr.cpp
    json/value.cpp
    json/dumps.cpp
    time/et.cpp
    massive/index.cpp
)
target_include_directories(kp_base PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(kp_base PUBLIC kp::fp PRIVATE kp::warnings)

# kp_pricing: quotes -> fair value. Pure arithmetic over stdlib + kp_base.
#
# It must NEVER link kp_net. That mirrors the Python invariant (pricing/ imports only stdlib and
# config) and is what lets the whole pricing stack be golden-tested with no network at all.
file(GLOB KP_PRICING_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pricing/*.cpp)
add_library(kp_pricing STATIC ${KP_PRICING_SRC})
target_link_libraries(kp_pricing PUBLIC kp_base PRIVATE kp::warnings)

# kp_net: everything that opens a socket -- TLS, REST, the hand-rolled WebSocket, the Kalshi and
# Massive clients -- plus the on-disk tapes. Pricing never links it (see above).
file(GLOB KP_NET_SRC CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/net/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/kalshi/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/massive/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp)
list(REMOVE_ITEM KP_NET_SRC ${CMAKE_CURRENT_SOURCE_DIR}/massive/index.cpp)   # pure; in kp_base
if(KP_NET_SRC)
    add_library(kp_net STATIC ${KP_NET_SRC})
    target_link_libraries(kp_net PUBLIC kp_base OpenSSL::SSL OpenSSL::Crypto CURL::libcurl
                          ZLIB::ZLIB Threads::Threads PRIVATE kp::warnings)
else()
    add_library(kp_net INTERFACE)
    target_link_libraries(kp_net INTERFACE kp_base)
endif()

# kp_rvol: the LIVE half of rvol/ (features, model, live) -- the realized-vol forecast. The fit and
# the dataset builders are offline and stay in Python; this reads their artifact.
file(GLOB KP_RVOL_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rvol/*.cpp)
if(KP_RVOL_SRC)
    add_library(kp_rvol STATIC ${KP_RVOL_SRC})
    # kp_net only for kp::kalshi::py_type_name in log lines; rvol opens no socket itself.
    target_link_libraries(kp_rvol PUBLIC kp_pricing kp_net kp_base PRIVATE kp::warnings)
else()
    add_library(kp_rvol INTERFACE)
    target_link_libraries(kp_rvol INTERFACE kp_pricing)
endif()

# kp_strategy: quoting rules, the order manager, budgets and guards. The decision halves are pure;
# the pieces that SEND (orders::apply, fastcancel) reach kp_net through the client interface.
file(GLOB KP_STRATEGY_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/strategy/*.cpp)
add_library(kp_strategy STATIC ${KP_STRATEGY_SRC})
target_link_libraries(kp_strategy PUBLIC kp_rvol kp_pricing kp_net kp_base PRIVATE kp::warnings)

# kp_all: one name for tests and apps to link.
add_library(kp_all INTERFACE)
target_link_libraries(kp_all INTERFACE kp_strategy kp_rvol kp_net kp_pricing kp_base)

# kp_app: the quote loop's own logic (strategy/run_04_quote_loop.py) -- pure helpers and the
# cycle, separate from apps/ so the cycle can be driven by record/replay without a main().
file(GLOB KP_APP_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp)
add_library(kp_app STATIC ${KP_APP_SRC})
target_link_libraries(kp_app PUBLIC kp_all PRIVATE kp::warnings)

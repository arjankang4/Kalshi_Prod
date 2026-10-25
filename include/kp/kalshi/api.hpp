// The seam between the order logic (kp_strategy) and the exchange client (kp_net).
//
// strategy/orders.apply and the fast guard SEND through this interface and nothing else, so the
// whole order manager can be replayed against a fake exchange in tests and under record/replay,
// and so the dry path provably never reaches a socket. kalshi::Client (src/kalshi/client.cpp)
// is the one real implementation.
//
// Semantics are the reference's, method for method (kalshi/client.py):
//   batch_create_orders  POST   /portfolio/events/orders/batched, body {"orders": [...]};
//                        returns resp["orders"] (or [] ), the RAW per-order result list -- partial
//                        failure is normal and each element carries its own `error`.
//   batch_cancel_orders  DELETE /portfolio/events/orders/batched, body {"orders":[{"order_id":..}]};
//                        returns resp["orders"] (or []); `reduced_by` is the honest confirmation.
// Both return an empty array for empty input WITHOUT a request, both charge the write bucket
// per item before sending (10 per create, 2 per cancel), and both throw KalshiError on non-2xx.
// A write is NEVER retried, on any status -- a retried create is a double-sent order.
#pragma once
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/kalshi/error.hpp"

namespace kp::kalshi {

class Api {
public:
    virtual ~Api() = default;
    virtual kp::json::Value batch_create_orders(const std::vector<kp::json::Value>& orders) = 0;
    virtual kp::json::Value batch_cancel_orders(const std::vector<std::string>& order_ids) = 0;
};

}  // namespace kp::kalshi

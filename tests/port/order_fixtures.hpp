// Shared by the order-path suites (test_orders, test_budget, test_fastcancel): hex/JSON decoding
// of the vector fields, Order and RestingBook <-> the reference's dict shapes, and a replaying
// fake of kp::kalshi::Api. Header-only and test-only.
#pragma once
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/kalshi/api.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/kalshi/error.hpp"
#include "kp/strategy/orders.hpp"
#include "kp/strategy/quoting.hpp"

namespace kp::test::ofx {

using kp::json::Value;
namespace ko = kp::strategy::orders;

inline std::string unhex(const std::string& s) {
    if (s == "-") return "";
    std::string out;
    out.reserve(s.size() / 2);
    auto nib = [](char c) { return (c <= '9') ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back((char)((nib(s[i]) << 4) | nib(s[i + 1])));
    return out;
}
inline Value jhex(const std::string& s) { return kp::json::parse(unhex(s)); }
inline std::string js(const Value& v) { return kp::json::dumps(v); }

inline std::optional<double> onum(const Value* v) {
    return (v == nullptr || v->is_null()) ? std::nullopt : std::optional<double>(v->num());
}
inline std::optional<double> onum(const Value& v) { return onum(&v); }

inline ko::Order order_from(const Value& d) {
    ko::Order o;
    if (const Value* v = d.find("ticker")) o.ticker = v->str();
    if (const Value* v = d.find("side")) o.side = v->str();
    o.price = onum(d.find("price"));
    if (const Value* v = d.find("count"); v && !v->is_null()) o.count = v->str();
    if (const Value* v = d.find("order_id")) {
        if (v->is_null()) o.order_id_null = true;
        else o.order_id = v->str();
    }
    if (const Value* v = d.find("client_order_id"); v && !v->is_null())
        o.client_order_id = v->str();
    if (const Value* v = d.find("dry_run"); v && !v->is_null()) o.dry_run = v->boolean();
    if (const Value* v = d.find("reason"); v && !v->is_null()) o.reason = v->str();
    if (const Value* v = d.find("pair"); v && !v->is_null()) o.pair = v->str();
    // Key order is data: was client_order_id written after reason/pair?
    int pos_coid = -1, pos_reason = -1, i = 0;
    for (const auto& m : d.obj()) {
        if ((m.first == "client_order_id" || m.first == "dry_run") && pos_coid < 0) pos_coid = i;
        if ((m.first == "reason" || m.first == "pair") && pos_reason < 0) pos_reason = i;
        ++i;
    }
    o.coid_after_reason = pos_coid >= 0 && pos_reason >= 0 && pos_coid > pos_reason;
    return o;
}

inline std::vector<ko::Order> orders_from(const Value& arr) {
    std::vector<ko::Order> out;
    for (const auto& d : arr.arr()) out.push_back(order_from(d));
    return out;
}

inline Value orders_json(const std::vector<ko::Order>& rows) {
    Value a = Value::make_array({});
    for (const auto& o : rows) a.push(o.to_json());
    return a;
}

inline std::vector<kp::strategy::quoting::Rung> rungs_from(const Value& arr) {
    std::vector<kp::strategy::quoting::Rung> out;
    int i = 0;
    for (const auto& r : arr.arr())
        out.push_back({i++, r.arr()[0].num(), r.arr()[1].str(), std::nullopt, "t", std::nullopt});
    return out;
}

inline Value on(std::optional<double> v) { return v ? Value::make_number(*v) : Value(); }

inline void load_book(ko::RestingBook& b, const Value& js) {
    for (const auto& d : js.arr()) {
        ko::MarketState& st = b.state(d.find("ticker")->str());
        st.bid = orders_from(*d.find("bid"));
        st.ask = orders_from(*d.find("ask"));
        st.theo = onum(d.find("theo"));
        st.best_bid = onum(d.find("best_bid"));
        st.best_ask = onum(d.find("best_ask"));
        st.zombie_bid = onum(d.find("zombie_bid"));
        st.zombie_ask = onum(d.find("zombie_ask"));
        st.zombie_bid_touch = onum(d.find("zombie_bid_touch"));
        st.zombie_ask_touch = onum(d.find("zombie_ask_touch"));
        st.fill_bid = onum(d.find("fill_bid"));
        st.fill_ask = onum(d.find("fill_ask"));
        st.position = d.find("position")->num();
    }
}

inline Value book_json(const ko::RestingBook& b) {
    Value out = Value::make_array({});
    for (const auto& t : b.tickers()) {
        const ko::MarketState& st = *b.find(t);
        Value d;
        d.set("ticker", Value::make_string(t));
        d.set("bid", orders_json(st.bid));
        d.set("ask", orders_json(st.ask));
        d.set("theo", on(st.theo));
        d.set("best_bid", on(st.best_bid));
        d.set("best_ask", on(st.best_ask));
        d.set("zombie_bid", on(st.zombie_bid));
        d.set("zombie_ask", on(st.zombie_ask));
        d.set("zombie_bid_touch", on(st.zombie_bid_touch));
        d.set("zombie_ask_touch", on(st.zombie_ask_touch));
        d.set("fill_bid", on(st.fill_bid));
        d.set("fill_ask", on(st.fill_ask));
        d.set("position", Value::make_number(st.position));
        out.push(d);
    }
    return out;
}

// Replays the calls a scripted Python fake answered: checks each request against the one the
// reference sent, charges the bucket exactly as the real transport would (debit before the send,
// penalise after a 429), and answers with the identical response or exception.
class ReplayApi : public kp::kalshi::Api {
public:
    explicit ReplayApi(const Value& calls, kp::kalshi::TokenBucket* bucket = nullptr)
        : calls_(calls.is_array() ? calls.arr() : std::vector<Value>{}), bucket_(bucket) {}

    Value batch_cancel_orders(const std::vector<std::string>& ids) override {
        const Value& c = next("cancel");
        Value req = Value::make_array({});
        for (const auto& i : ids) req.push(Value::make_string(i));
        if (js(req) != js(*c.find("req"))) mismatch("cancel ids " + js(req));
        return respond(c, (double)(kp::kalshi::CANCEL_TOKEN_COST * (long)ids.size()));
    }
    Value batch_create_orders(const std::vector<Value>& orders) override {
        const Value& c = next("create");
        const std::string req = js(Value::make_array(orders));
        if (req != c.find("req")->str()) mismatch("create bodies " + req);
        return respond(c, (double)(kp::kalshi::ORDER_TOKEN_COST * (long)orders.size()));
    }
    // Every recorded call was made, in order, with the recorded request -- and no other.
    bool ok() const { return ok_ && i_ == calls_.size(); }
    std::size_t made() const { return i_; }
    const std::string& why() const { return why_; }

private:
    const Value& next(const char* kind) {
        static const Value none;
        if (i_ >= calls_.size()) {
            mismatch(std::string("unexpected extra ") + kind + " call");
            ++i_;
            throw std::logic_error("ReplayApi: unexpected call");
        }
        const Value& c = calls_[i_++];
        if (c.find("kind")->str() != kind) mismatch(std::string("expected ") + kind);
        return c;
    }
    void mismatch(const std::string& w) {
        if (ok_) why_ = w;
        ok_ = false;
    }
    Value respond(const Value& c, double cost) {
        if (bucket_) bucket_->debit(cost);
        const Value& r = *c.find("resp");
        if (const Value* ok = r.find("ok")) return *ok;
        if (const Value* ke = r.find("kalshi")) {
            const int status = (int)ke->arr()[0].integer();
            if (status == 429 && bucket_) bucket_->penalise(cost);
            throw kp::kalshi::KalshiError(status, ke->arr()[1].str());
        }
        const Value& ex = *r.find("exc");
        const std::string name = ex.arr()[0].str(), msg = ex.arr()[1].str();
        if (name == "Exception") throw std::runtime_error(msg);
        throw kp::kalshi::PyNamedError(name, msg);
    }
    std::vector<Value> calls_;
    kp::kalshi::TokenBucket* bucket_;
    std::size_t i_ = 0;
    bool ok_ = true;
    std::string why_;
};

// An Api that fails the suite if it is ever reached -- the dry path.
class ForbiddenApi : public kp::kalshi::Api {
public:
    Value batch_cancel_orders(const std::vector<std::string>&) override { ++touched; return {}; }
    Value batch_create_orders(const std::vector<Value>&) override { ++touched; return {}; }
    int touched = 0;
};

struct FakeClock {
    double t = 0.0, step = 0.0;
    double operator()() { const double v = t; t += step; return v; }
};

inline ko::IdSource counter_ids(const std::string& prefix) {
    auto n = std::make_shared<long>(0);
    return [prefix, n] { return prefix + "-" + std::to_string(++*n); };
}

}  // namespace kp::test::ofx

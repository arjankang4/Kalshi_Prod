// kalshi/client.py's pure half against the reference: fp, _max_batch and its float floor division,
// order_body as the BYTES json.dumps sends, TokenBucket scripts on a fake clock, write_budget's
// dict, and Python float() over the strings the order path parses.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/kalshi/bucket.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace kk = kp::kalshi;

namespace {

std::string unhex(const std::string& s) {
    if (s == "-") return "";
    std::string out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back((char)std::stoi(s.substr(i, 2), nullptr, 16));
    return out;
}

struct FakeClock {
    double t = 0.0, step = 0.0;
    double operator()() { const double v = t; t += step; return v; }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("bucket");
    T.expect({"fp", "maxb", "floordiv", "body", "bnew", "bop_avail", "bop_afford", "bop_debit",
              "bop_penalise", "bop_resize", "bop_detail", "bop_jump", "bop_end", "limits",
              "pyfloat"});

    std::map<long, std::shared_ptr<FakeClock>> clocks;
    std::map<long, std::unique_ptr<kk::TokenBucket>> buckets;

    for (const auto& line : read(dir, "bucket")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "fp") {
            T.hit(fn, kk::fp(hx(w[1]), std::stoi(w[2])) == unhex(w[3]), line);
        } else if (fn == "maxb") {
            T.hit(fn, kk::max_batch(ohx(w[1]), ohx(w[2])) == std::stol(w[3]), line);
        } else if (fn == "floordiv") {
            T.hit(fn, same(kk::py_floordiv(hx(w[1]), hx(w[2])), w[3]), line);
        } else if (fn == "body") {
            const std::string tif = unhex(w[6]), stp = unhex(w[8]);
            const auto b = kk::order_body(unhex(w[1]), w[2], unhex(w[3]), unhex(w[4]), unhex(w[5]),
                                          tif.empty() ? "good_till_canceled" : tif,
                                          w[7] == "-" ? true : w[7] == "1",
                                          stp.empty() ? "maker" : stp);
            kp::json::Value wrap;
            wrap.set("orders", kp::json::Value::make_array({b}));
            T.hit(fn, kp::json::dumps(b) == unhex(w[9]) && kp::json::dumps(wrap) == unhex(w[10]),
                  line);
        } else if (fn == "bnew") {
            const long sid = std::stol(w[1]);
            auto c = std::make_shared<FakeClock>();
            c->t = hx(w[4]);
            c->step = hx(w[5]);
            clocks[sid] = c;
            buckets[sid] = std::make_unique<kk::TokenBucket>(hx(w[2]), hx(w[3]),
                                                             [c] { return (*c)(); });
            T.hit(fn, true, line);
        } else if (fn == "bop") {
            const long sid = std::stol(w[1]);
            auto& b = *buckets.at(sid);
            const std::string& op = w[2];
            bool ok = false;
            if (op == "avail") ok = same(b.available(), w[3]);
            else if (op == "afford") ok = (b.can_afford(hx(w[3])) ? "1" : "0") == w[4];
            else if (op == "debit") ok = same(b.debit(hx(w[3])), w[4]);
            else if (op == "penalise") ok = same(b.penalise(hx(w[3])), w[4]);
            else if (op == "resize") {
                b.resize(ohx(w[3]), ohx(w[4]));
                ok = same(b.capacity(), w[5]) && same(b.refill_rate(), w[6]);
            } else if (op == "detail") ok = kp::json::dumps(b.detail().to_json()) == unhex(w[3]);
            else if (op == "jump") { clocks.at(sid)->t += hx(w[3]); ok = true; }
            else if (op == "end") ok = same(b.spent(), w[3]) && b.denied() == std::stol(w[4]);
            T.hit("bop_" + op, ok, line);
        } else if (fn == "limits") {
            const auto lim = kp::json::parse(unhex(w[1]));
            bool ok;
            try {
                const std::string got = kp::json::dumps(kk::write_budget_from_limits(lim));
                ok = w[2] != "ERR" && got == unhex(w[2]);
            } catch (const std::exception&) {
                ok = w[2] == "ERR";
            }
            T.hit(fn, ok, line);
        } else if (fn == "pyfloat") {
            bool ok;
            try {
                const double got = kk::py_float_str(unhex(w[1]));
                ok = w[2] != "ERR" && same(got, w[2]);
            } catch (const std::invalid_argument&) {
                ok = w[2] == "ERR";
            }
            T.hit(fn, ok, line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }

    // The protocol constants other modules and the networked client read.
    kp::test::check("ORDER_TOKEN_COST is 10", kk::ORDER_TOKEN_COST == 10);
    kp::test::check("CANCEL_TOKEN_COST is 2", kk::CANCEL_TOKEN_COST == 2);
    kp::test::check("DEFAULT_TOKEN_COST is 10", kk::DEFAULT_TOKEN_COST == 10);
    kp::test::check("a batch is 30 on the measured tier, not 90", kk::max_batch(900.0, 300.0) == 30);
    kp::test::check("writes are POST/DELETE/PUT/PATCH",
                    kk::is_write_method("post") && kk::is_write_method("DELETE")
                        && !kk::is_write_method("GET"));
    kp::test::check("a named error keeps its Python class name",
                    kk::py_type_name(kk::PyNamedError("TimeoutError", "x")) == "TimeoutError");
    return T.report();
}

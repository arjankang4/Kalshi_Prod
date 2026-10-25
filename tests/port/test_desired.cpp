// run_04.build_desired and quoting.top_of_book against the reference: 1,500 randomized boards
// (every book shape, quotable/refused/partial edge records, lean rows, band shapes, positions at
// and past the inventory limit, fill backoff ages) plus 3,000 standalone book reads. Desired rungs
// and the full per-market notes are compared as json.dumps text, i.e. key order included.
#include <string>
#include <vector>

#include "kp/app/desired.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace json = kp::json;
namespace app = kp::app;

namespace {
std::vector<std::string> tabs(const std::string& s) {
    std::vector<std::string> out;
    std::size_t p0 = 0;
    for (std::size_t p = 0; p <= s.size(); ++p)
        if (p == s.size() || s[p] == '\t') { out.push_back(s.substr(p0, p - p0)); p0 = p + 1; }
    return out;
}
std::optional<double> onum(const json::Value& o, const char* k) {
    const json::Value* v = o.find(k);
    return (v && v->is_number()) ? std::optional<double>(v->num()) : std::nullopt;
}
std::optional<long> oint(const json::Value& o, const char* k) {
    const json::Value* v = o.find(k);
    return (v && v->is_number()) ? std::optional<long>((long)v->integer()) : std::nullopt;
}
std::optional<std::string> ostr(const json::Value& o, const char* k) {
    const json::Value* v = o.find(k);
    return (v && v->is_string()) ? std::optional<std::string>(v->str()) : std::nullopt;
}
kp::strategy::edgemgr::EdgeRecord rec_of(const json::Value& o) {
    kp::strategy::edgemgr::EdgeRecord r;
    r.edge_micros = oint(o, "edge_micros");
    r.floor_micros = oint(o, "floor_micros");
    r.bonus_micros = oint(o, "bonus_micros");
    r.edge = onum(o, "edge");
    r.risk_u = onum(o, "risk_u").value_or(0.0);
    r.scale_pos = onum(o, "scale_pos").value_or(0.0);
    r.R = onum(o, "R");
    r.delta = onum(o, "delta");
    r.vega = onum(o, "vega");
    r.theta = onum(o, "theta");
    const json::Value* q = o.find("quotable");
    r.quotable = q && q->is_bool() && q->boolean();
    r.source = ostr(o, "source").value_or("");
    r.reason = ostr(o, "reason").value_or("");
    return r;
}
json::Value opt(std::optional<double> v) { return v ? json::Value::make_number(*v) : json::Value(); }
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("desired");
    T.expect({"desired", "tob"});
    for (const auto& line : read(dir, "desired")) {
        const auto f = tabs(line);
        if (f[0] == "tob") {
            const json::Value e = json::parse(f[1]);
            const auto t = app::top_of_book(&e);
            json::Value got;
            for (auto v : {t.best_bid, t.best_ask, t.bid_size, t.ask_size}) got.push(opt(v));
            if (got.is_null()) got = json::Value::make_array({});
            T.hit("tob", json::dumps(got) == f[2], line + "\n   got " + json::dumps(got));
            continue;
        }
        const json::Value in = json::parse(f[1]);
        std::vector<kp::pricing::theo::Market> markets;
        for (const auto& m : in.find("markets")->arr()) markets.push_back(kp::pricing::theo::market_from_json(m));
        app::Fairs fairs;
        for (const auto& [tk, d] : in.find("fairs")->obj()) {
            app::FairDetail fd;
            fd.fair = onum(d, "fair");
            fd.reason = ostr(d, "reason");
            fd.fair_vol = onum(d, "fair_vol");
            fd.delta_adj = onum(d, "delta_adj");
            fd.f_move = onum(d, "f_move");
            fd.delta_adj_reason = ostr(d, "delta_adj_reason");
            fairs[tk] = fd;
        }
        app::Books books;
        for (const auto& [tk, b] : in.find("books")->obj()) books[tk] = b;
        std::map<std::string, kp::strategy::edgemgr::EdgeRecord> recs;
        for (const auto& [tk, r] : in.find("em")->obj()) recs[tk] = rec_of(r);
        const app::EdgeLookup em = [&](const std::string& tk) {
            auto it = recs.find(tk);
            if (it != recs.end()) return it->second;
            kp::strategy::edgemgr::EdgeRecord r;
            r.edge_micros = 10000; r.floor_micros = 10000; r.bonus_micros = 0; r.edge = 0.01;
            r.quotable = false; r.source = "no_R"; r.reason = "unknown_ticker";
            return r;
        };
        app::LeanRows lean;
        for (const auto& [tk, r] : in.find("lean")->obj()) {
            kp::strategy::leanchain::Row row;
            row.p_ref = onum(r, "p_ref");
            row.p_rv = onum(r, "p_rv");
            lean[tk] = row;
        }
        app::Bands bands;
        const json::Value* bj = in.find("bands");
        const bool has_bands = bj && !bj->is_null();
        if (has_bands)
            for (const auto& [tk, b] : bj->obj()) {
                kp::strategy::band::Classified c;
                c.inband = b.find("inband")->boolean();
                for (const auto& s : b.find("counts")->arr()) c.counts.push_back(s.str());
                c.position_limit = b.find("position_limit")->num();
                c.detail.has_detail = b.find("_detail") != nullptr;
                c.detail.sigmas = onum(b, "sigmas");
                bands[tk] = c;
            }
        app::PositionMap pos;
        for (const auto& [tk, v] : in.find("positions")->obj()) pos[tk] = v.num();
        kp::strategy::orders::RestingBook book;
        for (const auto& [tk, sd] : in.find("fills")->obj())
            for (const auto& [side, t] : sd.obj()) book.state(tk).mark_fill(side, t.num());
        const auto res = app::build_desired(markets, fairs, &books, em, &lean,
                                            in.find("tiers")->boolean(), has_bands ? &bands : nullptr,
                                            &pos, book, in.find("mono")->num(),
                                            kp::time::Instant{in.find("now_ns")->integer()});
        json::Value des = json::Value::make_array({});
        for (const auto& [key, rungs] : res.desired) {
            json::Value rs = json::Value::make_array({});
            for (const auto& r : rungs) {
                json::Value rr;
                rr.push(json::Value::make_int(r.index));
                rr.push(json::Value::make_number(r.price));
                rr.push(json::Value::make_string(r.count));
                rr.push(opt(r.edge));
                rr.push(json::Value::make_string(r.reason));
                rr.push(r.min_edge ? json::Value::make_int(*r.min_edge) : json::Value());
                rs.push(rr);
            }
            json::Value row;
            row.push(json::Value::make_string(key.first));
            row.push(json::Value::make_string(key.second));
            row.push(rs);
            des.push(row);
        }
        json::Value notes = json::Value::make_object({});
        for (const auto& [tk, n] : res.notes) notes.set(tk, n);
        const std::string gd = json::dumps(des), gn = json::dumps(notes);
        T.hit("desired", gd == f[2] && gn == f[3],
              line.substr(0, 200) + "\n   got desired " + gd.substr(0, 300) + "\n   got notes " + gn.substr(0, 600)
              + "\n  want notes " + f[3].substr(0, 600));
    }
    return T.report();
}

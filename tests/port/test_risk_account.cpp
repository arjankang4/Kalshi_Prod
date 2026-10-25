// risklimit's account mark against the reference: 400 sessions of marks over random snapshots,
// books and fairs, missing snapshots (the stale halt), the loss-limit halt, and a mid-session
// RESTART that must read the persisted opening equity back -- plus the persisted file's bytes.
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "kp/app/cycle.hpp"
#include "kp/strategy/risklimit.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace json = kp::json;
namespace rl = kp::strategy::risklimit;
using OptD = std::optional<double>;

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("risk_account");
    T.expect({"step", "file"});
    const kp::time::Instant NOW{1787754605123456000LL};    // 2026-08-26T14:30:05.123456Z
    const auto base = std::filesystem::temp_directory_path() / ("riskacct-cxx-" + std::to_string(::getpid()));
    long cur_sess = -1;
    std::unique_ptr<rl::SessionRisk> r;
    std::string tmp;
    OptD limit;
    bool restarted = false;
    const auto make = [&]() {
        r = std::make_unique<rl::SessionRisk>(limit ? limit : std::nullopt, tmp + "/NOHALT");
        if (!limit) r = std::make_unique<rl::SessionRisk>(std::nullopt, tmp + "/NOHALT");
        r->set_state_dir(tmp);
        r->set_wall_clock([&] { return NOW; });
    };
    for (const auto& line : read(dir, "risk_account")) {
        const auto tab = line.find('\t');
        const std::string tag = line.substr(0, tab);
        const json::Value rec = json::parse(line.substr(tab + 1));
        const long sess = (long)rec.find("sess")->integer();
        if (tag == "file") {
            std::ifstream in(tmp + "/" + rec.find("name")->str());
            std::stringstream ss;
            ss << in.rdbuf();
            T.hit(tag, ss.str() == rec.find("text")->str(), line + "\n   got " + ss.str());
            continue;
        }
        if (sess != cur_sess) {
            cur_sess = sess;
            tmp = (base / std::to_string(sess)).string();
            std::filesystem::create_directories(tmp);
            limit = rec.find("limit")->is_null() ? OptD() : OptD(rec.find("limit")->num());
            restarted = false;
            make();
        }
        if (rec.find("restart")->is_bool() && rec.find("restart")->boolean() && !restarted) {
            restarted = true;
            make();
        }
        std::optional<rl::AccountSnap> snap;
        if (const json::Value* s = rec.find("snap"); s && !s->is_null()) {
            rl::AccountSnap a;
            a.t = s->arr()[0].num();
            a.balance = s->arr()[1].num();
            for (const auto& [tk, q] : s->arr()[2].obj()) a.positions.emplace_back(tk, q.num());
            snap = a;
        }
        rl::Tops tops;
        for (const auto& [tk, v] : rec.find("books")->obj()) {
            const auto& a = v.arr();
            tops[tk] = {a[0].is_null() ? OptD() : OptD(a[0].num()), a[1].is_null() ? OptD() : OptD(a[1].num())};
        }
        std::map<std::string, OptD> fairs;
        for (const auto& [tk, v] : rec.find("fairs")->obj()) fairs[tk] = v.is_null() ? OptD() : OptD(v.num());
        const OptD pnl = r->mark_account(snap, tops, &fairs, rec.find("mono")->num());
        const std::string why = r->halt_reason();
        const json::Value* wh = rec.find("halt");
        const auto jnum = [](OptD v) { return v ? json::dumps(json::Value::make_number(*v)) : std::string("null"); };
        const bool pnl_ok = jnum(pnl) == json::dumps(*rec.find("pnl"));
        const bool halt_ok = wh->is_null() ? why.empty() : why == wh->str();
        const bool stale_ok = jnum(r->stale_s()) == json::dumps(*rec.find("stale_s"));
        const std::string gs = json::dumps(kp::app::to_json(r->summary()));
        const std::string ws = json::dumps(*rec.find("summary"));
        T.hit(tag, pnl_ok && halt_ok && stale_ok && gs == ws,
              line.substr(0, 300) + "\n   got pnl=" + jnum(pnl) + " halt=" + why + " stale=" + jnum(r->stale_s())
                  + "\n   got summary " + gs + "\n  want summary " + ws);
    }
    if (!std::getenv("KEEP")) std::filesystem::remove_all(base);
    return T.report();
}

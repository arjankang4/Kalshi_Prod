// The JSON reader must recover every double in every real model artifact BIT-EXACTLY.
//
// This is not a formality. Python writes doubles as shortest round-trip decimal; if the parser
// is off by one ulp on any value in var/model/variance_calendar/SPX.json, then varcal.phi is off,
// so var_left is off, so theta is off, so R is off, so the required edge is off -- and no unit
// vector anywhere else would tell you why. Hence strtod, and hence this test.
#include "../harness.hpp"
#include "kp/json/value.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace kp::test;

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    const std::string ref = (argc > 2) ? argv[2] : KP_KALSHI_ROOT_DEFAULT;

    section("every double in every real artifact round-trips");
    {
        std::ifstream in(dir + "/json_roundtrip.vec");
        if (!in) { check("open json_roundtrip.vec", false, dir); return report("test_json"); }
        std::string line;
        std::getline(in, line);               // header
        long n = 0, bad = 0;
        std::string worst;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string tag, text, hexs;
            ss >> tag >> text >> hexs;
            if (tag != "num") continue;
            ++n;
            // Parse it exactly as it appears inside a document, not via strtod directly, so the
            // number scanner itself is under test.
            const kp::json::Value v = kp::json::parse("[" + text + "]");
            const double got = v.arr().at(0).num();
            const double want = std::strtod(hexs.c_str(), nullptr);
            if (!same_bits(got, want)) {
                ++bad;
                if (worst.empty()) {
                    char b[200];
                    std::snprintf(b, sizeof b, "%s got=%a want=%a", text.c_str(), got, want);
                    worst = b;
                }
            }
        }
        char b[300];
        std::snprintf(b, sizeof b, "%ld doubles from the real artifacts", n);
        check(b, bad == 0, worst);
    }

    section("the artifacts parse as documents");
    for (const char* rel : {"/var/model/variance_calendar/SPX.json",
                            "/var/model/variance_calendar/NDX.json",
                            "/var/model/edge/SPX.json",
                            "/var/model/edge/NDX.json"}) {
        const std::string path = ref + rel;
        std::ifstream probe(path);
        if (!probe) { check(std::string("skip (absent) ") + rel, true); continue; }
        try {
            const kp::json::Value v = kp::json::parse_file(path);
            check(std::string("parsed ") + rel, v.is_object());
        } catch (const std::exception& e) {
            check(std::string("parsed ") + rel, false, e.what());
        }
    }

    section("shape and ordering");
    {
        // Object members keep INSERTION ORDER, as a Python dict -- a std::map would reorder, and
        // dict order reaches the wire elsewhere in this stack.
        const kp::json::Value v = kp::json::parse(R"({"z":1,"a":2,"m":3})");
        const auto& o = v.obj();
        check("object members keep insertion order",
              o.size() == 3 && o[0].first == "z" && o[1].first == "a" && o[2].first == "m");
        // Absent and null are different, because Python distinguishes them.
        const kp::json::Value w = kp::json::parse(R"({"present":null})");
        check("null is present but null", w.find("present") != nullptr && w.find("present")->is_null());
        check("absent is nullptr", w.find("missing") == nullptr);
        check("opt_num treats null as absent", !w.opt_num("present").has_value());
    }
    {
        const kp::json::Value v = kp::json::parse(R"({"s":"a\"b\\c\né😀"})");
        check("escapes and surrogate pairs decode",
              v.find("s") != nullptr && v.find("s")->str() == "a\"b\\c\n\xc3\xa9\xf0\x9f\x98\x80");
    }
    return report("test_json");
}

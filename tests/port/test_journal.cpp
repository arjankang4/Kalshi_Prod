// strategy/journal against the reference: the bytes of the file the C++ Journal leaves on disk,
// line by line and whole, including the reopen-appends path; plus isoformat / str(datetime).
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "kp/json/value.hpp"
#include "kp/strategy/journal.hpp"
#include "kp/time/et.hpp"
#include "tapes_io.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::tapes;
namespace jr = kp::strategy::journal;
namespace json = kp::json;

namespace {

// Rebuild the record: {"__f__": ...} -> a non-finite float; {"__dt__": ns, "tz": ...} ->
// str(datetime), which is what `default=str` writes.
json::Value decode(const json::Value& v) {
    if (v.is_object()) {
        if (const auto* f = v.find("__f__")) {
            const std::string& s = f->str();
            return json::Value::make_number(s == "nan" ? std::nan("") : s == "inf" ? HUGE_VAL : -HUGE_VAL);
        }
        if (const auto* d = v.find("__dt__")) {
            const kp::time::Instant t{d->integer()};
            return json::Value::make_string(v.find("tz")->str() == "utc" ? jr::py_str_datetime_utc(t)
                                                                         : jr::py_str_datetime_et(t));
        }
        json::Value o = json::Value::make_object({});
        for (const auto& [k, x] : v.obj()) o.set(k, decode(x));
        return o;
    }
    if (v.is_array()) {
        json::Value a = json::Value::make_array({});
        for (const auto& x : v.arr()) a.push(decode(x));
        return a;
    }
    return v;
}

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("journal");
    T.expect({"row", "reopen", "file", "iso"});

    char tmpl[] = "/tmp/kp_journal_XXXXXX";
    const char* td = ::mkdtemp(tmpl);
    if (!td) { std::printf("FATAL: mkdtemp\n"); return 1; }
    const std::string root = td;
    const std::string stamp = "20260821T143000Z";
    auto j = jr::open_journal(root, stamp, "quotes.jsonl");
    std::string expect_file;
    long n_since_open = 0;

    for (const auto& line : read(dir, "journal")) {
        const auto parts = tabs(line);
        const auto w = words(parts[0]);
        const std::string& fn = w[0];
        if (fn == "row") {
            const auto before = std::filesystem::exists(j->path()) ? std::filesystem::file_size(j->path()) : 0;
            j->write(decode(json::parse(parts[1])));
            ++n_since_open;
            // what landed on disk for THIS record (flushed here only to read it back; the
            // Journal's own fsync cadence is untouched)
            std::fflush(nullptr);
            j->flush();
            const std::string all = slurp(j->path());
            const std::string got = all.substr(before);
            expect_file += parts[2] + "\n";
            T.hit(fn, got == parts[2] + "\n" && j->n() == n_since_open, line);
        } else if (fn == "reopen") {
            j->close();
            j = jr::open_journal(root, stamp, "quotes.jsonl");
            n_since_open = 0;
            T.hit(fn, j->n() == 0, line);
        } else if (fn == "file") {
            j->close();
            const std::string p = root + "/" + w[1];
            const std::string data = slurp(p);
            T.hit(fn, j->path() == p && std::to_string(data.size()) == w[2] && data == expect_file,
                  line);
        } else if (fn == "iso") {
            const kp::time::Instant t{std::stoll(w[1])};
            T.hit(fn, jr::isoformat_et(t) == w[2] && jr::isoformat_utc(t) == w[3]
                      && jr::py_str_datetime_et(t) == parts[1] && jr::py_str_datetime_utc(t) == parts[2],
                  line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return T.report();
}

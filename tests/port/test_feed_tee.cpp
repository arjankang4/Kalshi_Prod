// storage::FeedTee/Segment against the reference, FILE BYTES and all (see gen_feed_tee.py): every
// segment and every manifest the Python writer leaves on disk must hash identically here.
#include <openssl/evp.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include "kp/storage/feed_tee.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace fs = std::filesystem;
using kp::json::Value;

namespace {

struct FakeClock : kp::storage::Clock {
    std::int64_t ns = 0;
    double mono = 0.0;
    std::int64_t time_ns() override { return ns; }
    double time() override { return (double)ns / 1e9; }
    double monotonic() override { return mono; }
};

std::string sha256_file(const fs::path& p, std::size_t& size) {
    std::ifstream in(p, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size = b.size();
    unsigned char d[32];
    unsigned n = 0;
    EVP_Digest(b.data(), b.size(), d, &n, EVP_sha256(), nullptr);
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (unsigned i = 0; i < n; ++i) { s.push_back(hx[d[i] >> 4]); s.push_back(hx[d[i] & 15]); }
    return s;
}

std::string raw_of(const std::string& w) {
    if (w.rfind("hex:", 0) != 0) return w;
    std::string out;
    for (std::size_t i = 4; i + 1 < w.size(); i += 2) out.push_back((char)std::stoi(w.substr(i, 2), nullptr, 16));
    return out;
}

// The rest of the line after the first `n` words.
std::string tail(const std::string& line, int n) {
    std::size_t p = 0;
    for (int i = 0; i < n; ++i) {
        p = line.find(' ', p);
        if (p == std::string::npos) return "";
        ++p;
    }
    return line.substr(p);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("feed_tee");
    T.expect({"scenario", "open", "file", "detail", "files_listed"});
    const char* td = std::getenv("TMPDIR");
    FakeClock clock;
    std::unique_ptr<kp::storage::FeedTee> tee;
    fs::path tmp;
    std::vector<std::pair<std::string, std::string>> want_files;   // relpath -> "size sha"
    std::string scenario;

    auto finish_files = [&]() {
        std::vector<std::string> got;
        for (const auto& e : fs::recursive_directory_iterator(tmp))
            if (e.is_regular_file()) got.push_back(fs::relative(e.path(), tmp).string());
        std::sort(got.begin(), got.end());
        std::vector<std::string> want;
        for (const auto& [p, v] : want_files) want.push_back(p);
        T.hit("files_listed", got == want, scenario + ": " + std::to_string(got.size()) + " files vs " +
                                               std::to_string(want.size()));
    };

    for (const auto& line : read(dir, "feed_tee")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "scenario") {
            scenario = w[1];
            tmp = fs::path(td ? td : "/tmp") / ("kp-tee-cpp-" + std::to_string(::getpid()) + "-" + scenario);
            fs::remove_all(tmp);
            fs::create_directories(tmp);
            kp::storage::FeedTee::Options o;
            o.ws_dir = tmp.string();
            o.session_id = w[2];
            o.queue_max = std::stol(w[3]);
            o.limits.roll_bytes = std::stoll(w[4]);
            o.limits.roll_seconds = std::stod(w[5]);
            o.dt = w[6];
            o.channel = w[7];
            o.subscribe_state = kp::json::parse(tail(line, 8));
            o.clock = &clock;
            tee = std::make_unique<kp::storage::FeedTee>(o);
            want_files.clear();
            T.hit(fn, true, line);
        } else if (fn == "clock") {
            clock.ns = std::stoll(w[1]);
            clock.mono = hx(w[2]);
        } else if (fn == "open") {
            T.hit(fn, tee->open_first(), line);
        } else if (fn == "offer") {
            tee->offer(std::stoll(w[1]), kp::json::parse(raw_of(w[2])));
        } else if (fn == "drain") {
            tee->drain_once(0.0);
        } else if (fn == "stop") {
            tee->stop();
        } else if (fn == "file") {
            want_files.emplace_back(w[1], w[2] + " " + w[3]);
            std::size_t size = 0;
            const fs::path p = tmp / w[1];
            const std::string sha = fs::exists(p) ? sha256_file(p, size) : "missing";
            const bool ok = std::to_string(size) == w[2] && sha == w[3];
            if (!ok && p.string().find(".meta.json") != std::string::npos && fs::exists(p)) {
                std::ifstream in(p);
                std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::printf("--- C++ %s ---\n%s\n", w[1].c_str(), b.c_str());
            }
            T.hit(fn, ok, scenario + " " + line + " GOT " + std::to_string(size) + " " + sha);
        } else if (fn == "detail") {
            finish_files();
            const std::string got = kp::json::dumps(tee->detail(), kp::json::COMPACT);
            T.hit(fn, got == tail(line, 1), scenario + " " + line + "\n   GOT " + got);
            tee.reset();
            fs::remove_all(tmp);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    return T.report();
}

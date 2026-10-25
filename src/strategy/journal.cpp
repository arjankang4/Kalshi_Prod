#include "kp/strategy/journal.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>

#include "kp/massive/futures_pure.hpp"

namespace kp::strategy::journal {

std::string journal_dir(const std::string& root) {
    return (std::filesystem::path(root) / "var" / "journal").string();
}

std::string dumps_row(const kp::json::Value& rec) { return kp::json::dumps(rec, kp::json::COMPACT); }

Journal::Journal(std::string path) : path_(std::move(path)) {
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    fh_ = std::fopen(path_.c_str(), "a");
    if (!fh_) throw std::runtime_error("open " + path_ + ": " + std::strerror(errno));
}

Journal::~Journal() {
    try { close(); } catch (...) {}
}

void Journal::write(const kp::json::Value& rec) {
    if (!fh_) throw std::runtime_error("I/O operation on closed file.");
    const std::string line = dumps_row(rec) + "\n";
    if (std::fwrite(line.data(), 1, line.size(), fh_) != line.size())
        throw std::runtime_error("write " + path_ + ": " + std::strerror(errno));
    n_ += 1;
    // Batched so it is not an fsync per line; a killed process loses at most the last batch.
    if (n_ % FSYNC_EVERY == 0) flush();
}

void Journal::flush() {
    if (!fh_) return;
    std::fflush(fh_);
    ::fsync(::fileno(fh_));
}

void Journal::close() {
    if (!fh_) return;
    flush();
    std::fclose(fh_);
    fh_ = nullptr;
}

std::unique_ptr<Journal> open_journal(const std::string& root, const std::string& stamp,
                                      const std::string& name) {
    const auto p = std::filesystem::path(journal_dir(root)) / ("run=" + stamp) / name;
    return std::make_unique<Journal>(p.string());
}

namespace {

std::string iso(long long secs_local, long micros, long offset_s, char sep) {
    const long long day = secs_local >= 0 ? secs_local / 86400 : -((-secs_local + 86399) / 86400);
    const long long sod = secs_local - day * 86400;
    const auto d = kp::massive::futures::civil_from_days((long)day);
    char b[64];
    int n = std::snprintf(b, sizeof b, "%04d-%02d-%02d%c%02d:%02d:%02d", d.year, d.month, d.day,
                          sep, (int)(sod / 3600), (int)(sod % 3600 / 60), (int)(sod % 60));
    std::string s(b, (std::size_t)n);
    if (micros) {
        std::snprintf(b, sizeof b, ".%06ld", micros);
        s += b;
    }
    const char sign = offset_s < 0 ? '-' : '+';
    const long a = offset_s < 0 ? -offset_s : offset_s;
    std::snprintf(b, sizeof b, "%c%02ld:%02ld", sign, a / 3600, a % 3600 / 60);
    s += b;
    return s;
}

void split_ns(const kp::time::Instant& t, long long& secs, long& micros) {
    secs = t.ns >= 0 ? t.ns / 1000000000LL : -((-t.ns + 999999999LL) / 1000000000LL);
    micros = (long)((t.ns - secs * 1000000000LL) / 1000);
}

}  // namespace

std::string isoformat_utc(const kp::time::Instant& t, char sep) {
    long long secs; long micros;
    split_ns(t, secs, micros);
    return iso(secs, micros, 0, sep);
}

std::string isoformat_et(const kp::time::Instant& t, char sep) {
    long long secs; long micros;
    split_ns(t, secs, micros);
    const auto p = kp::time::to_et(t);
    const long long local = (long long)kp::massive::futures::days_from_civil(
                                kp::massive::futures::Date{p.year, p.month, p.day}) * 86400
                            + p.hour * 3600 + p.minute * 60 + p.second;
    return iso(local, micros, (long)(local - secs), sep);
}

}  // namespace kp::strategy::journal

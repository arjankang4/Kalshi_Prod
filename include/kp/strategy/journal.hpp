// Append-only JSONL record of quoting decisions (and, via a second file, the risk tape).
//
// Port of strategy/journal.py: `Journal` and `open_journal`. One record per line, byte-identical
// to the reference's
//
//     json.dumps(rec, separators=(",", ":"), default=str) + "\n"
//
// which kp::json::dumps(rec, COMPACT) already reproduces (key order, int vs float, float repr,
// NaN/Infinity, ensure_ascii escapes).
//
// `default=str`. A kp::json::Value cannot hold a non-JSON type, so the conversion the reference
// does at dump time happens where the C++ caller builds the record. The only non-JSON values that
// can reach a journal on the live path are datetimes, and `str(datetime)` is isoformat with a
// SPACE separator -- py_str_datetime_* below produce exactly that text. (run_04's quote rows and
// the risk tape carry none today; the helpers exist so a future field does not guess.)
//
// THE ROOT IS A PARAMETER. The reference writes under config.ROOT/var/journal; here the caller
// passes the root, so tests write into a temp dir and never into the live tree.
//
// Durability: fsync every FSYNC_EVERY records and on close, as the reference. Not thread-safe;
// run_04 writes each journal from the cycle thread only.
#pragma once
#include <cstdio>
#include <memory>
#include <string>

#include "kp/json/value.hpp"
#include "kp/time/et.hpp"

namespace kp::strategy::journal {

inline constexpr long FSYNC_EVERY = 50;

// <root>/var/journal
std::string journal_dir(const std::string& root);

// One record's line, without the trailing newline.
std::string dumps_row(const kp::json::Value& rec);

class Journal {
public:
    // Creates the parent directories and opens `path` for APPEND (a restart continues the file).
    // Throws std::runtime_error if the file cannot be opened, as open() raises in the reference.
    explicit Journal(std::string path);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    // Throws std::runtime_error on a failed write (the reference raises OSError).
    void write(const kp::json::Value& rec);
    void flush();          // flush + fsync
    void close();          // flush + close; idempotent

    long n() const { return n_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::FILE* fh_ = nullptr;
    long n_ = 0;
};

// Journal(<root>/var/journal/run=<stamp>/<name>)
std::unique_ptr<Journal> open_journal(const std::string& root, const std::string& stamp,
                                      const std::string& name = "decisions.jsonl");

// datetime.isoformat() of an aware datetime: "YYYY-MM-DDTHH:MM:SS[.ffffff]+HH:MM", microseconds
// only when non-zero (sub-microsecond ns are truncated -- a Python datetime has none). `sep` ' '
// gives str(datetime), i.e. what `default=str` writes.
std::string isoformat_et(const kp::time::Instant& t, char sep = 'T');
std::string isoformat_utc(const kp::time::Instant& t, char sep = 'T');
inline std::string py_str_datetime_et(const kp::time::Instant& t) { return isoformat_et(t, ' '); }
inline std::string py_str_datetime_utc(const kp::time::Instant& t) { return isoformat_utc(t, ' '); }

}  // namespace kp::strategy::journal

// storage/segments.py (the local writer half) -- gzip rolling capture segments + manifest sidecar.
//
// BYTE-IDENTICAL TO THE REFERENCE, which is stronger than "a valid gzip of the same lines", and the
// difference is observable: Segment.should_roll compares the RAW FILE POSITION with the roll size,
// and that position only moves when zlib emits output. Python's GzipFile (3.12) puts a 32 KiB
// io.BufferedWriter in front of the compressor, so WHEN deflate is called -- and therefore when bytes
// come out, and therefore which frame lands in which segment -- depends on that buffer's exact
// flush rule. GzipWriter reproduces it (see gzip_writer.cpp), plus the header Python writes for a
// GzipFile over a named file: FNAME = the basename without ".gz", MTIME as given (0 here, as
// segments.py passes), XFL 0 for level 6, OS 255.
//
// Clocks are injectable so a golden test can drive time exactly as the reference saw it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>

#include <zlib.h>

#include "kp/json/value.hpp"

namespace kp::storage {

// time.time_ns / time.time / time.monotonic. The default is the real clock; `time()` is derived
// from `time_ns()` exactly as CPython's time.time() is.
struct Clock {
    virtual ~Clock() = default;
    virtual std::int64_t time_ns();
    virtual double time();
    virtual double monotonic();
    static Clock& real();
};

// gzip.GzipFile(fileobj=open(path, "wb"), mode="wb", compresslevel, mtime), including its 32 KiB
// write buffer. `tell()` is the raw file's position (header + compressed bytes emitted so far).
class GzipWriter {
public:
    GzipWriter(const std::string& path, int compresslevel = 6, std::uint32_t mtime = 0);
    ~GzipWriter();
    GzipWriter(const GzipWriter&) = delete;
    GzipWriter& operator=(const GzipWriter&) = delete;
    void write(const std::string& data);
    void flush(int zlib_mode = Z_SYNC_FLUSH);      // GzipFile.flush + the raw file's flush
    void close();
    std::int64_t tell() const { return raw_pos_; }

    static constexpr std::size_t WRITE_BUFFER = 4 * 8192;   // gzip._WRITE_BUFFER_SIZE

private:
    void compress(const char* p, std::size_t n, int flush);
    void raw(const std::string& s);
    std::FILE* f_ = nullptr;
    z_stream z_{};
    std::string buf_;
    std::uint32_t crc_ = 0;
    std::uint64_t size_ = 0;
    std::int64_t raw_pos_ = 0;
    bool closed_ = false;
};

// SEGMENT_ROLL_BYTES / SEGMENT_ROLL_SECONDS default to config's; SYNC_SECONDS is segments.py's.
struct SegmentLimits {
    std::int64_t roll_bytes = 64LL * 1024 * 1024;
    double roll_seconds = 15 * 60;
    double sync_seconds = 20;
};

class Segment {
public:
    // Path: <ws_dir>/dt=<dt>/channel=<channel>/seg-<session[:8]>-<seq_no:06d>.jsonl.gz
    Segment(const std::string& ws_dir, const std::string& dt, const std::string& channel,
            const std::string& session_id, long seq_no, const kp::json::Value& subscribe_state,
            Clock& clock, const SegmentLimits& limits = {});
    void write(const kp::json::Value& obj);                  // json.dumps(obj, separators=(",",":"))
    bool sync(std::optional<double> now = std::nullopt);
    void note_frame(std::int64_t recv_ns, const kp::json::Value* sid, const kp::json::Value* seq,
                    const kp::json::Value* ftype);
    bool should_roll(std::optional<double> now = std::nullopt);
    const kp::json::Value& close();                          // writes <path>.meta.json

    std::string path;
    std::string dt, channel, session_id;
    long seq_no;
    double opened_at;
    long lines = 0;
    std::int64_t plain_bytes = 0;
    long dropped = 0;
    std::optional<std::int64_t> first_recv_ns, last_recv_ns;
    kp::json::Value sids;                                    // an object, insertion-ordered
    kp::json::Value subscribe_state;
    bool closed = false;
    double last_sync;
    kp::json::Value meta;

private:
    Clock& clock_;
    SegmentLimits lim_;
    GzipWriter gz_;
};

// The digest and length of a gzip file's DECOMPRESSED stream (s3util.sha256_gz_plaintext).
std::pair<std::string, std::int64_t> sha256_gz_plaintext(const std::string& path);

}  // namespace kp::storage

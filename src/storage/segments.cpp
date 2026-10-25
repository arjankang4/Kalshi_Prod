#include "kp/storage/segments.hpp"

#include <openssl/evp.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "kp/storage/pyerr.hpp"

namespace kp::storage {

namespace fs = std::filesystem;
using kp::json::Value;

// --- clock ------------------------------------------------------------------------------------

std::int64_t Clock::time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
double Clock::time() {
    // CPython _PyTime_AsSecondsDouble: an exact second converts the integer; otherwise ns / 1e9.
    const std::int64_t t = time_ns();
    if (t % 1000000000 == 0) return (double)(t / 1000000000);
    return (double)t / 1e9;
}
double Clock::monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
Clock& Clock::real() {
    static Clock c;
    return c;
}

namespace {

// An OSError the way CPython words it: "[Errno 13] Permission denied: '/x'".
PyError os_error(int err, const std::string& path) {
    const char* name = err == EACCES || err == EPERM ? "PermissionError"
                     : err == ENOENT ? "FileNotFoundError"
                     : err == EEXIST ? "FileExistsError"
                     : err == EISDIR ? "IsADirectoryError"
                     : err == ENOTDIR ? "NotADirectoryError" : "OSError";
    return PyError(name, "[Errno " + std::to_string(err) + "] " + std::strerror(err) + ": " + py_repr_str(path));
}

void put32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 0xFF));
}

std::string seg_path(const std::string& ws_dir, const std::string& dt, const std::string& channel,
                     const std::string& session_id, long seq_no) {
    const fs::path dir = fs::path(ws_dir) / ("dt=" + dt) / ("channel=" + channel);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw os_error(ec.value(), dir.string());
    char n[32];
    std::snprintf(n, sizeof n, "%06ld", seq_no);
    return (dir / ("seg-" + session_id.substr(0, 8) + "-" + n + ".jsonl.gz")).string();
}

}  // namespace

// --- GzipWriter -------------------------------------------------------------------------------

GzipWriter::GzipWriter(const std::string& path, int compresslevel, std::uint32_t mtime) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) throw os_error(errno, path);
    if (deflateInit2(&z_, compresslevel, Z_DEFLATED, -MAX_WBITS, 8, 0) != Z_OK) {
        std::fclose(f_);
        throw PyError("zlib.error", "deflateInit2 failed");
    }
    crc_ = (std::uint32_t)crc32(0L, Z_NULL, 0);
    // _write_gzip_header: FNAME is os.path.basename(fileobj.name), minus a trailing ".gz".
    std::string fname = fs::path(path).filename().string();
    if (fname.size() >= 3 && fname.compare(fname.size() - 3, 3, ".gz") == 0) fname.resize(fname.size() - 3);
    std::string h = "\x1f\x8b\x08";
    h.push_back(fname.empty() ? '\0' : '\x08');
    put32(h, mtime);
    h.push_back(compresslevel == 9 ? '\x02' : compresslevel == 1 ? '\x04' : '\0');
    h.push_back('\xff');
    if (!fname.empty()) { h += fname; h.push_back('\0'); }
    raw(h);
}

GzipWriter::~GzipWriter() {
    if (!closed_) {
        deflateEnd(&z_);
        if (f_) std::fclose(f_);
    }
}

void GzipWriter::raw(const std::string& s) {
    if (s.empty()) return;
    if (std::fwrite(s.data(), 1, s.size(), f_) != s.size()) throw os_error(errno, "<segment>");
    raw_pos_ += (std::int64_t)s.size();
}

void GzipWriter::compress(const char* p, std::size_t n, int flush) {
    if (flush == Z_NO_FLUSH && n == 0) return;               // _write_raw skips empty data
    if (n) {
        crc_ = (std::uint32_t)crc32(crc_, (const Bytef*)p, (uInt)n);
        size_ += n;
    }
    std::string out;
    unsigned char chunk[65536];
    z_.next_in = (Bytef*)p;
    z_.avail_in = (uInt)n;
    do {
        z_.next_out = chunk;
        z_.avail_out = sizeof chunk;
        const int rc = deflate(&z_, flush);
        if (rc == Z_STREAM_ERROR) throw PyError("zlib.error", "deflate failed");
        out.append((const char*)chunk, sizeof chunk - z_.avail_out);
    } while (z_.avail_out == 0 || z_.avail_in != 0);
    raw(out);
}

// io.BufferedWriter.write over _WriteBufferStream: data that FITS the free space is buffered
// (exactly-full included); otherwise the buffer is flushed first, then data larger than the whole
// buffer goes straight through and anything smaller becomes the new buffer.
void GzipWriter::write(const std::string& data) {
    if (data.size() <= WRITE_BUFFER - buf_.size()) {
        buf_ += data;
        return;
    }
    if (!buf_.empty()) {
        compress(buf_.data(), buf_.size(), Z_NO_FLUSH);
        buf_.clear();
    }
    if (data.size() > WRITE_BUFFER) compress(data.data(), data.size(), Z_NO_FLUSH);
    else buf_ = data;
}

void GzipWriter::flush(int zlib_mode) {
    if (!buf_.empty()) { compress(buf_.data(), buf_.size(), Z_NO_FLUSH); buf_.clear(); }
    if (zlib_mode != Z_NO_FLUSH) compress(nullptr, 0, zlib_mode);
    std::fflush(f_);
}

void GzipWriter::close() {
    if (closed_) return;
    if (!buf_.empty()) { compress(buf_.data(), buf_.size(), Z_NO_FLUSH); buf_.clear(); }
    compress(nullptr, 0, Z_FINISH);
    std::string t;
    put32(t, crc_);
    put32(t, (std::uint32_t)(size_ & 0xffffffffu));
    raw(t);
    deflateEnd(&z_);
    closed_ = true;
    if (std::fclose(f_) != 0) { f_ = nullptr; throw os_error(errno, "<segment>"); }
    f_ = nullptr;
}

// --- sha256 of the plaintext ------------------------------------------------------------------

std::pair<std::string, std::int64_t> sha256_gz_plaintext(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw os_error(errno, path);
    std::string gz((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
    std::int64_t n = 0;
    // gzip.open reads every member of a multi-member file; inflate with gzip framing, member by member.
    std::size_t off = 0;
    unsigned char out[65536];
    while (off < gz.size()) {
        z_stream z{};
        if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) break;
        z.next_in = (Bytef*)gz.data() + off;
        z.avail_in = (uInt)(gz.size() - off);
        int rc;
        do {
            z.next_out = out;
            z.avail_out = sizeof out;
            rc = inflate(&z, Z_NO_FLUSH);
            const std::size_t got = sizeof out - z.avail_out;
            EVP_DigestUpdate(md, out, got);
            n += (std::int64_t)got;
            if (rc != Z_OK && rc != Z_STREAM_END) {
                inflateEnd(&z);
                EVP_MD_CTX_free(md);
                throw PyError("BadGzipFile", "corrupt gzip stream in " + path);
            }
        } while (rc != Z_STREAM_END && (z.avail_in > 0 || z.avail_out == 0));
        off = gz.size() - z.avail_in;
        inflateEnd(&z);
        if (rc != Z_STREAM_END) {
            EVP_MD_CTX_free(md);
            throw PyError("EOFError", "Compressed file ended before the end-of-stream marker was reached");
        }
    }
    unsigned char dg[32];
    unsigned int dl = 0;
    EVP_DigestFinal_ex(md, dg, &dl);
    EVP_MD_CTX_free(md);
    static const char* hx = "0123456789abcdef";
    std::string hex;
    for (unsigned i = 0; i < dl; ++i) { hex.push_back(hx[dg[i] >> 4]); hex.push_back(hx[dg[i] & 15]); }
    return {hex, n};
}

// --- Segment ----------------------------------------------------------------------------------

Segment::Segment(const std::string& ws_dir, const std::string& dt_, const std::string& channel_,
                 const std::string& session_id_, long seq_no_, const Value& subscribe_state_,
                 Clock& clock, const SegmentLimits& limits)
    : path(seg_path(ws_dir, dt_, channel_, session_id_, seq_no_)),
      dt(dt_), channel(channel_), session_id(session_id_), seq_no(seq_no_),
      opened_at(0.0), subscribe_state(subscribe_state_), last_sync(0.0),
      clock_(clock), lim_(limits), gz_(path, 6, 0) {
    opened_at = clock_.time();
    sids = Value::make_object({});
    last_sync = opened_at;
}

void Segment::write(const Value& obj) {
    std::string line = kp::json::dumps(obj, kp::json::COMPACT);
    line.push_back('\n');
    gz_.write(line);
    ++lines;
    plain_bytes += (std::int64_t)line.size();
}

bool Segment::sync(std::optional<double> now) {
    // `now = now or time.time()`: a zero is falsy, exactly as None is.
    const double t = (now && *now != 0.0) ? *now : clock_.time();
    if (closed || t - last_sync < lim_.sync_seconds) return false;
    gz_.flush(Z_SYNC_FLUSH);
    last_sync = t;
    return true;
}

namespace {
bool num_lt(const Value& a, const Value& b) {
    if (!a.is_number() || !b.is_number())
        throw PyError("TypeError", "'<' not supported between instances of '" + py_type_name(a) + "' and '" +
                                       py_type_name(b) + "'");
    if (a.is_int() && b.is_int()) return a.integer() < b.integer();
    return a.num() < b.num();
}
}  // namespace

void Segment::note_frame(std::int64_t recv_ns, const Value* sid, const Value* seq, const Value* ftype) {
    if (!first_recv_ns) first_recv_ns = recv_ns;
    last_recv_ns = recv_ns;
    if (!sid || sid->is_null()) return;
    const std::string key = session_id + ":" + py_str(*sid);
    Value* s = sids.find_mut(key);
    const Value seqv = seq ? *seq : Value();
    if (!s) {
        Value rec = Value::make_object({});
        rec.set("first_seq", seqv);
        rec.set("last_seq", seqv);
        rec.set("n_delta", Value::make_int(0));
        rec.set("n_snapshot", Value::make_int(0));
        rec.set("n_control", Value::make_int(0));
        sids.set(key, rec);
        s = sids.find_mut(key);
    }
    if (!seqv.is_null()) {
        const Value& f = *s->find("first_seq");
        if (f.is_null() || num_lt(seqv, f)) s->set("first_seq", seqv);
        const Value& l = *s->find("last_seq");
        if (l.is_null() || num_lt(l, seqv)) s->set("last_seq", seqv);
    }
    const std::string t = ftype && ftype->is_string() ? ftype->str() : std::string();
    const char* counter = t == "orderbook_delta" ? "n_delta" : t == "orderbook_snapshot" ? "n_snapshot" : "n_control";
    s->set(counter, Value::make_int(s->find(counter)->integer() + 1));
}

bool Segment::should_roll(std::optional<double> now) {
    const double t = (now && *now != 0.0) ? *now : clock_.time();
    if (gz_.tell() >= lim_.roll_bytes) return true;
    if (t - opened_at < lim_.roll_seconds) return false;
    return lines > 1;
}

const Value& Segment::close() {
    if (closed) return meta;
    gz_.close();
    closed = true;
    const auto [sha, n] = sha256_gz_plaintext(path);
    std::error_code ec;
    const auto csize = fs::file_size(path, ec);
    auto oi = [](const std::optional<std::int64_t>& v) { return v ? Value::make_int(*v) : Value(); };
    char segid[64];
    std::snprintf(segid, sizeof segid, "%s-%06ld", session_id.c_str(), seq_no);
    meta = Value::make_object({});
    meta.set("segment_id", Value::make_string(segid));
    meta.set("capture_session_id", Value::make_string(session_id));
    meta.set("capture_date_et", Value::make_string(dt));
    meta.set("channel", Value::make_string(channel));
    meta.set("line_count", Value::make_int(lines));
    meta.set("uncompressed_bytes", Value::make_int(n));
    meta.set("compressed_bytes", Value::make_int((long long)csize));
    meta.set("plaintext_sha256", Value::make_string(sha));
    meta.set("first_recv_ns", oi(first_recv_ns));
    meta.set("last_recv_ns", oi(last_recv_ns));
    meta.set("sids", sids);
    meta.set("dropped_frames", Value::make_int(dropped));
    meta.set("subscribe_state", subscribe_state);
    meta.set("opened_at", Value::make_number(opened_at));
    meta.set("closed_at", Value::make_number(clock_.time()));
    meta.set("epochs_opened", Value::make_array({}));
    meta.set("epochs_closed", Value::make_array({}));
    if (n != plain_bytes) {
        Value mm = Value::make_object({});
        mm.set("counted", Value::make_int(plain_bytes));
        mm.set("on_disk", Value::make_int(n));
        meta.set("byte_count_mismatch", mm);
    }
    const std::string mpath = path + ".meta.json";
    std::FILE* f = std::fopen(mpath.c_str(), "wb");
    if (!f) throw os_error(errno, mpath);
    const std::string text = kp::json::dumps(meta, kp::json::DumpStyle{", ", ": ", 1});
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return meta;
}

}  // namespace kp::storage

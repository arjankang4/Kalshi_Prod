#include "kp/storage/feed_tee.hpp"

#include <openssl/rand.h>

#include <chrono>
#include <cstdio>

#include "kp/storage/pyerr.hpp"
#include "kp/time/et.hpp"

namespace kp::storage {

using kp::json::Value;

std::string new_session_id() {
    unsigned char b[4];
    RAND_bytes(b, sizeof b);
    char out[9];
    std::snprintf(out, sizeof out, "%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
    return out;
}

namespace {

std::string today_et(Clock& c) {
    kp::time::init();
    const auto p = kp::time::to_et(kp::time::Instant{c.time_ns()});
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", p.year, p.month, p.day);
    return b;
}

// f"{prefix}: {type(e).__name__}: {str(e)[:100]}"
std::string err_text(const char* prefix, const std::exception& e) {
    const auto* pe = dynamic_cast<const PyError*>(&e);
    const std::string type = pe ? pe->type : "RuntimeError";
    return std::string(prefix) + ": " + type + ": " + py_slice(e.what(), 100);
}

}  // namespace

FeedTee::FeedTee(Options o) : o_(std::move(o)) {
    clock_ = o_.clock ? o_.clock : &Clock::real();
    dt = o_.dt.empty() ? today_et(*clock_) : o_.dt;
    channel = o_.channel.empty() ? "quoter" : o_.channel;
    session_id = o_.session_id.empty() ? new_session_id() : o_.session_id;
    subscribe_state = o_.subscribe_state.is_null() ? Value::make_object({}) : o_.subscribe_state;
}

FeedTee::~FeedTee() {
    stop_ = true;
    qcv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool FeedTee::open_first() {
    try {
        open_segment();
    } catch (const std::exception& e) {
        error = err_text("open", e);
        return false;
    }
    return true;
}

FeedTee& FeedTee::start() {
    if (!open_first()) return *this;
    running_ = true;
    th_ = std::thread([this] { run(); });
    return *this;
}

bool FeedTee::ok() const { return running_ && !error; }

FeedTee& FeedTee::stop() {
    stop_ = true;
    qcv_.notify_all();
    if (th_.joinable()) th_.join();        // the reference bounds this at 15 s; the loop exits
    close_segment();                       // once the queue is empty, which is bounded by it
    return *this;
}

bool FeedTee::offer(std::int64_t recv_ns, Value frame) {
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (o_.queue_max > 0 && (long)q_.size() >= o_.queue_max) {
            ++dropped;
            return false;
        }
        q_.emplace_back(recv_ns, std::move(frame));
    }
    qcv_.notify_one();
    return true;
}

std::size_t FeedTee::queued() const {
    std::lock_guard<std::mutex> lk(qmu_);
    return q_.size();
}

void FeedTee::run() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            if (stop_ && q_.empty()) break;
        }
        if (!drain_once(0.5)) break;
    }
    running_ = false;
}

bool FeedTee::drain_once(double timeout_s) {
    std::optional<std::pair<std::int64_t, Value>> item;
    {
        std::unique_lock<std::mutex> lk(qmu_);
        if (q_.empty() && timeout_s > 0)
            qcv_.wait_for(lk, std::chrono::duration<double>(timeout_s), [&] { return !q_.empty() || stop_; });
        if (!q_.empty()) {
            item.emplace(std::move(q_.front()));
            q_.pop_front();
        }
    }
    if (!item) {
        maybe_roll();
        return true;
    }
    try {
        write(item->first, item->second);
    } catch (const std::exception& e) {
        // A write failure must not take the session down and must not spin: record, stop writing.
        error = err_text("write", e);
        return false;
    }
    maybe_roll();
    return true;
}

void FeedTee::write(std::int64_t recv_ns, const Value& frame) {
    if (!segment_) return;
    if (dropped.load() && (clock_->monotonic() - last_drop_marker_) > DROP_MARKER_MIN_INTERVAL_S) {
        Value m = Value::make_object({});
        m.set("v", Value::make_int(1));
        m.set("kind", Value::make_string("drop_marker"));
        m.set("session", Value::make_string(session_id));
        m.set("recv_ns", Value::make_int(clock_->time_ns()));
        m.set("dropped_total", Value::make_int(dropped.load()));
        segment_->write(m);
        segment_->dropped = dropped.load();
        last_drop_marker_ = clock_->monotonic();
    }
    Value rec = Value::make_object({});
    rec.set("v", Value::make_int(1));
    rec.set("session", Value::make_string(session_id));
    rec.set("recv_ns", Value::make_int(recv_ns));
    rec.set("frame", frame);
    segment_->write(rec);
    ++frames;
    const Value* sid = frame.is_object() ? frame.find("sid") : nullptr;
    const Value* seq = frame.is_object() ? frame.find("seq") : nullptr;
    const Value* typ = frame.is_object() ? frame.find("type") : nullptr;
    segment_->note_frame(recv_ns, sid, seq, typ);
    segment_->sync();
}

void FeedTee::maybe_roll() {
    if (segment_ && segment_->should_roll()) {
        close_segment();
        open_segment();
    }
}

void FeedTee::open_segment() {
    segment_ = std::make_unique<Segment>(o_.ws_dir, dt, channel, session_id, seq_no, subscribe_state,
                                         *clock_, o_.limits);
    Value h = Value::make_object({});
    h.set("v", Value::make_int(1));
    h.set("session", Value::make_string(session_id));
    h.set("recv_ns", Value::make_int(clock_->time_ns()));
    h.set("kind", Value::make_string("segment_header"));
    h.set("sub", subscribe_state);
    h.set("dt", Value::make_string(dt));
    h.set("writer", Value::make_string("run_04_quote_loop"));
    segment_->write(h);
    ++seq_no;
}

std::optional<Value> FeedTee::close_segment() {
    if (!segment_) return std::nullopt;
    std::optional<Value> meta;
    try {
        segment_->dropped = dropped.load();
        meta = segment_->close();
        ++segments_closed;
        const Value* cb = meta->find("compressed_bytes");
        bytes_written += cb && cb->is_number() ? cb->integer() : 0;
    } catch (const std::exception& e) {
        if (!error) error = err_text("close", e);
        meta.reset();
    }
    segment_.reset();
    return meta;
}

Value FeedTee::detail() const {
    Value d = Value::make_object({});
    d.set("ok", Value::make_bool(ok()));
    d.set("dt", Value::make_string(dt));
    d.set("channel", Value::make_string(channel));
    d.set("session", Value::make_string(session_id));
    d.set("frames", Value::make_int(frames));
    d.set("dropped", Value::make_int(dropped.load()));
    d.set("queued", Value::make_int((long long)queued()));
    d.set("segments", Value::make_int(segments_closed));
    d.set("gz_bytes", Value::make_int(bytes_written));
    d.set("error", error ? Value::make_string(*error) : Value());
    return d;
}

}  // namespace kp::storage

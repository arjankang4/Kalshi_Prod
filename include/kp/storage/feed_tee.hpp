// storage/feed_tee.py -- tee the quote loop's own socket frames into rolling gzip segments.
//
// NEVER BLOCK THE PUMP: offer() is a bounded non-blocking put; when the queue is full the frame is
// DROPPED and counted, and the writer marks the tape with a `drop_marker` record (at most one per
// DROP_MARKER_MIN_INTERVAL_S). A writer failure records `error` and stops writing; it never throws
// into the caller. The S3 upload half stays in Python and picks these files up from disk.
//
// The writer thread's loop body is `drain_once`, public so a golden test can step the exact same
// code synchronously and compare the bytes with the reference's.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "kp/json/value.hpp"
#include "kp/storage/segments.hpp"

namespace kp::storage {

inline constexpr long TEE_QUEUE_MAX = 10000;
inline constexpr double DROP_MARKER_MIN_INTERVAL_S = 5.0;

class FeedTee {
public:
    struct Options {
        std::string ws_dir;                    // config.WS_DIR, i.e. <KALSHI_ROOT>/data/ws
        std::string dt;                        // "" -> today's ET date
        std::string channel = "quoter";        // config.QUOTE_TEE_CHANNEL
        kp::json::Value subscribe_state;       // null -> {}
        long queue_max = TEE_QUEUE_MAX;        // <= 0 means unbounded, as queue.Queue
        std::string session_id;                // "" -> uuid4().hex[:8]
        SegmentLimits limits;
        Clock* clock = nullptr;                // null -> the real clock
    };

    explicit FeedTee(Options o);
    ~FeedTee();
    FeedTee(const FeedTee&) = delete;
    FeedTee& operator=(const FeedTee&) = delete;

    FeedTee& start();                          // open the first segment, start the writer. Never throws.
    FeedTee& stop();                           // drain what is queued, close the segment + manifest
    bool ok() const;
    // The hot side. Never blocks, never throws; false if the frame was dropped.
    bool offer(std::int64_t recv_ns, kp::json::Value frame);
    kp::json::Value detail() const;

    // --- the writer loop body and the synchronous open, for tests ---
    bool open_first();                          // start() without the thread; false on error
    // One iteration of `_drain`: take one item (waiting up to timeout_s), write it, maybe roll.
    // Returns false when the loop should exit (a write error).
    bool drain_once(double timeout_s);
    std::size_t queued() const;

    std::string dt, channel, session_id;
    kp::json::Value subscribe_state;
    long seq_no = 0;
    long frames = 0;
    std::atomic<long> dropped{0};
    long segments_closed = 0;
    std::int64_t bytes_written = 0;
    std::optional<std::string> error;

private:
    void write(std::int64_t recv_ns, const kp::json::Value& frame);
    void maybe_roll();
    void open_segment();
    std::optional<kp::json::Value> close_segment();
    void run();

    Options o_;
    Clock* clock_;
    std::unique_ptr<Segment> segment_;
    double last_drop_marker_ = 0.0;
    mutable std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::pair<std::int64_t, kp::json::Value>> q_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread th_;
};

// uuid.uuid4().hex[:8]
std::string new_session_id();

}  // namespace kp::storage

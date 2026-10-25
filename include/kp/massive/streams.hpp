// massive/streams.py -- live futures and index over the Massive WebSockets.
//
// Two feeds on one shared base (`Stream`: connect, auth, subscribe, pump, reconnect-with-backoff).
// The per-frame handling is state and is golden-tested against the Python by replaying identical
// frames with time.time_ns/time.monotonic scripted; the pump thread is a thin loop around it.
//
//   FuturesFeed  top-of-book MID per underlying off `Q.<contract>` (ES for SPX, NQ for NDX).
//                Notifies (counts an update, feeds the tee, extends the move history) only on a
//                MID CHANGE -- 95.8% of futures frames repeat the mid (measured 2026-08-26).
//   IndexFeed    index level per underlying off `V.I:SPX` / `V.I:NDX`, as IndexQuote.
//
// UNITS: the WebSocket `t` is MILLISECONDS; everything is converted to nanoseconds at the frame
// so IndexQuote and pricing/spot are unchanged.
//
// Failing soft is the contract: start() never throws (a failure lands in last_error() and ok()
// stays false), and a broken socket reconnects with 2 s -> 30 s exponential backoff, RESUBSCRIBING
// -- a reconnect that forgot the subscription would look healthy and deliver nothing.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/index.hpp"
#include "kp/massive/transport.hpp"

namespace kp::massive::streams {

inline constexpr long long NS = 1000000000LL;
// Mid changes retained per underlying for move_scale: ~a minute at 10-20 distinct mids a second.
inline constexpr std::size_t HIST_MAX = 1200;
// Below this many mid changes move_scale refuses (nullopt) rather than guess.
inline constexpr std::size_t MIN_SCALE_TICKS = 20;
// getattr(config, "FUTURES_MAX_STALE_S", 2.0) -- config.py does not define it.
inline constexpr double FUTURES_MAX_QUOTE_AGE_S = 2.0;

// `storage.feed_tee.FeedTee.offer(recv_ns, frame)`, or anything with that shape. Called on the
// pump thread for every frame that CHANGED something, outside the feed's lock. The feed neither
// starts nor stops it.
using Tee = std::function<void(long long recv_ns, const kp::json::Value& msg)>;

struct StreamHealth {
    std::string name;
    bool ok = false;
    long frames = 0, updates = 0, reconnects = 0, errors = 0;
    std::optional<std::string> last_error;
    std::optional<double> silent_s;      // round(now - last_frame_t, 3); nullopt before any frame
};

class Stream {
public:
    static constexpr double RECONNECT_BACKOFF_S = 2.0;
    static constexpr double MAX_BACKOFF_S = 30.0;

    virtual ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Connect (15 s), auth, subscribe, spawn the pump. Never throws; returns *this.
    Stream& start(double timeout = 15.0);
    Stream& stop();

    // Pump running AND a socket up. False during a reconnect's backoff.
    bool ok() const;
    StreamHealth health(std::optional<double> now = std::nullopt) const;
    std::optional<std::string> last_error() const;
    const std::string& name() const { return name_; }
    const std::vector<std::string>& subs() const { return subs_; }

    // Test seams. run_pump() is the pump loop on the calling thread (threaded=false); it returns
    // on stop (a test pauses it with request_stop() from inside the transport and resumes with
    // clear_stop()), or true where the reference's pump thread would have DIED on an uncaught
    // exception. ingest() is one received frame; `mono_now` absent reads the clock only once the
    // frame has parsed, which is when the reference reads it.
    bool run_pump();
    bool ingest(const std::string& raw, long long recv_ns, std::optional<double> mono_now = std::nullopt);
    void request_stop() { stop_ = true; }
    void clear_stop() { stop_ = false; }

protected:
    Stream(std::string url, std::vector<std::string> subs, std::string name, Tee tee,
           std::string api_key, WsFactory ws, Clock clock, bool threaded);
    // One non-status message. Returns true iff it changed something. Throws std::exception on a
    // malformed field (-> errors / last_error "handle: ..."). Called with mu_ HELD.
    virtual bool on_message(const kp::json::Value& m, long long recv_ns) = 0;
    void set_subs(std::vector<std::string> s) { subs_ = std::move(s); }

    mutable std::mutex mu_;
    Clock clock_;

private:
    void connect();
    void close_ws();
    void wait_stop(double s);    // _stop.wait(s): returns early on stop()

    std::mutex stop_mu_;
    std::condition_variable stop_cv_;

    std::string url_;
    std::vector<std::string> subs_;
    std::string name_;
    Tee tee_;
    std::string key_;
    WsFactory ws_factory_;
    bool threaded_;

    std::unique_ptr<WsTransport> ws_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> have_ws_{false};

    long frames_ = 0, updates_ = 0, reconnects_ = 0, errors_ = 0;
    std::optional<std::string> last_error_;
    std::optional<double> last_frame_t_;
    std::optional<double> connected_at_;
};

// (mid, age_s) or (nullopt, age_s|nullopt) -- deltaadj.FuturesTicker.mid's contract exactly.
struct MidAge {
    std::optional<double> mid;
    std::optional<double> age_s;
};

class FuturesFeed : public Stream {
public:
    // Maps an underlying to its front-month contract, e.g. "SPX" -> "ESZ6", or nullopt if nothing
    // traded; may throw (recorded in resolve_error). The live binding is
    //   [&](u, day) { return futures_pure::front_month(PRODUCT.get(u, u), day, ...session_volume...); }
    // `day` is the ET date "YYYY-MM-DD".
    using Resolver = std::function<std::optional<std::string>(const std::string& underlying,
                                                              const std::string& day)>;

    // `contracts` pins a contract per underlying (the resolver is not consulted for those).
    FuturesFeed(std::vector<std::string> underlyings, std::string api_key, WsFactory ws,
                const std::map<std::string, std::string>& contracts = {},
                const Resolver& resolve = nullptr, const std::string& day = "", Tee tee = nullptr,
                Clock clock = Clock::system(), std::string url = "wss://socket.massive.com/futures",
                bool threaded = true);
    ~FuturesFeed() override;

    // (mid, age) on the VENDOR stamp. A negative age (clock skew) or one past max_age_s
    // (default FUTURES_MAX_QUOTE_AGE_S) is refused: (nullopt, age). `now_ns` absent reads the clock.
    MidAge mid(const std::string& underlying, std::optional<double> max_age_s = std::nullopt,
               std::optional<long long> now_ns = std::nullopt) const;

    // Expected adverse futures move over `horizon_s`, in index points:
    //   sigmas * sqrt(sum((dmid)**2) / span_s * horizon_s)
    // over the retained MID CHANGES on the vendor's clock. nullopt under min_ticks or on a zero
    // span; 0.0 if the realized variance is not positive. Bit-identical to the reference, which
    // takes Python 3.12's compensated sum() and libm pow() for the squares.
    std::optional<double> move_scale(const std::string& underlying, double horizon_s,
                                     double sigmas = 1.0, std::size_t min_ticks = MIN_SCALE_TICKS) const;

    // underlying -> contract, in `underlyings` order (a Python dict).
    const std::vector<std::pair<std::string, std::string>>& contract() const { return contract_; }
    const std::map<std::string, std::string>& resolve_error() const { return resolve_error_; }
    long ticks(const std::string& underlying) const;
    const std::vector<std::string>& underlyings() const { return underlyings_; }

protected:
    bool on_message(const kp::json::Value& m, long long recv_ns) override;

private:
    std::vector<std::string> underlyings_;
    std::vector<std::pair<std::string, std::string>> contract_;
    std::map<std::string, std::string> by_sym_;
    std::map<std::string, std::string> resolve_error_;
    std::map<std::string, std::pair<double, long long>> mid_;   // u -> (mid, ts_ns)
    std::map<std::string, long> ticks_;
    std::map<std::string, std::deque<std::pair<long long, double>>> hist_;
};

class IndexFeed : public Stream {
public:
    IndexFeed(std::vector<std::string> underlyings, std::string api_key, WsFactory ws,
              Tee tee = nullptr, Clock clock = Clock::system(),
              std::string url = "wss://socket.massive.com/indices", bool threaded = true);
    ~IndexFeed() override;

    // {underlying: IndexQuote} for those we have heard from -- absent, never empty. An empty
    // `underlyings` means all of this feed's. Same shape as index::snapshot (REST), so
    // run_04's _index_quotes and pricing/spot take either.
    std::map<std::string, kp::massive::index::IndexQuote>
    snapshot(const std::vector<std::string>& underlyings = {}) const;

    const std::vector<std::string>& underlyings() const { return underlyings_; }

protected:
    bool on_message(const kp::json::Value& m, long long recv_ns) override;

private:
    std::vector<std::string> underlyings_;
    std::map<std::string, std::string> by_tk_;     // "I:SPX" -> "SPX"
    std::map<std::string, kp::massive::index::IndexQuote> q_;
};

}  // namespace kp::massive::streams

// massive/client.py -- the options-Quotes WebSocket collector. (The contracts reference and
// pricing/universe.option_root_and_strikes are in kp/massive/contracts.hpp.)
//
// QuoteCollector is split the way the reference's behaviour is: the FRAME HANDLING and the
// SUBSCRIPTION DIFFING are plain state (golden-tested by replaying identical frame sequences into
// the Python with `websocket` and `time` monkeypatched), and the pump thread is a thin loop that
// feeds received frames to it. Tests run the same loop synchronously (`threaded=false`).
#pragma once
#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/snapshot.hpp"
#include "kp/massive/transport.hpp"

namespace kp::massive::client {

// Keep the tail only: enough to diagnose a subscribe failure, bounded against a session's worth of
// per-ticker acks (measured ~220 acks from one 210-ticker subscribe).
inline constexpr std::size_t STATUS_LOG_MAX = 2000;

struct CollectorHealth {
    bool alive = false;
    std::optional<std::string> pump_error;
    std::optional<double> quote_age_s;   // since the last QUOTE frame; nullopt = none yet (warm-up)
    std::size_t n_symbols = 0;
    long reconnects = 0;
    long frames = 0;
};

// Subscribes to Q.<ticker> for a list of option tickers and keeps the latest quote per symbol.
//
// Massive caps a connection at 1000 tickers and permits ONE options connection per account, so the
// subscription is a budget. Lifecycle: construct -> start() -> [snapshot()/health()/recenter()]* ->
// stop(). If health().alive goes false, reconnect() -- which CLEARS the cache, because every quote
// is of unknown age after an unknown gap and a stale quote is priced as a current one.
class QuoteCollector {
public:
    static constexpr std::size_t MAX_TICKERS = 1000;
    static constexpr double RECENTER_MIN_INTERVAL = 3.0;

    // `raw_path`: optional JSONL tee of every raw frame (off in run_04). `threaded=false` makes
    // start()/reconnect() NOT spawn the pump: the caller drives run_pump() itself (tests).
    QuoteCollector(std::vector<std::string> tickers, std::string api_key, WsFactory ws,
                   Clock clock = Clock::system(), std::string raw_path = "",
                   std::string url = "wss://socket.massive.com/options", bool threaded = true);
    ~QuoteCollector();
    QuoteCollector(const QuoteCollector&) = delete;
    QuoteCollector& operator=(const QuoteCollector&) = delete;

    // Connect, auth, subscribe (ONE frame, comma-joined). A MassiveError naming `max_connections`
    // (the server reaps a closed connection after a few seconds) is retried `attempts` times,
    // `backoff` s apart; anything else throws at once.
    QuoteCollector& start(int attempts = 6, double backoff = 5.0);
    // Stop the pump, close the socket and the raw tee. Idempotent.
    QuoteCollector& stop();
    // Rebuild the socket and resubscribe to sorted(subscribed()). Clears the cache. false (with
    // pump_error "reconnect failed: ...") on failure; never throws.
    bool reconnect(int attempts = 6, double backoff = 5.0);

    bool alive() const;
    // `now` is a monotonic instant; nullopt reads the clock.
    CollectorHealth health(std::optional<double> now = std::nullopt) const;

    // {OCC symbol: latest quote}, a copy.
    kp::massive::OptionSnapshot snapshot() const;
    // Block until >= min_symbols distinct symbols have quoted, or `timeout` s pass.
    bool wait_for_coverage(std::size_t min_symbols, double timeout = 20.0, double poll = 0.25) const;

    // Move the subscription to `want`: unsubscribe first (so it never transiently exceeds the cap,
    // and the dropped symbols' cached quotes are evicted), then subscribe. Returns
    // {added, removed}, or nullopt if called within RECENTER_MIN_INTERVAL of the last re-centre
    // (refused, not queued). Throws MassiveError if `want` holds more than MAX_TICKERS distinct
    // tickers. Newly added symbols arrive BLIND: no snapshot on subscribe.
    std::optional<std::pair<std::size_t, std::size_t>> recenter(const std::vector<std::string>& want,
                                                                bool force = false);
    // Direct diffing, as in the reference: each returns how many tickers it sent. subscribe throws
    // MassiveError past the cap.
    std::size_t subscribe(const std::vector<std::string>& tickers);
    std::size_t unsubscribe(const std::vector<std::string>& tickers);

    std::set<std::string> subscribed() const { return subscribed_; }
    const std::vector<std::string>& tickers() const { return tickers_; }
    long reconnects() const { return reconnects_; }
    long recenter_count() const { return recenter_count_; }
    long frame_count() const;
    long msg_count() const;
    std::size_t status_log_size() const;
    long status_dropped() const;
    std::optional<double> last_msg_t() const;
    std::optional<std::string> pump_error() const;

    // The pump loop, run on the calling thread until stop, a transport error (-> pump_error), or a
    // frame the reference's pump would have died on (returns true: "died"). Public for tests
    // (threaded=false), which pause it with request_stop() from inside the transport and resume
    // with clear_stop() + run_pump() -- exactly what the generator does to the Python's _stop.
    bool run_pump();
    void request_stop() { stop_ = true; }
    void clear_stop() { stop_ = false; }
    // One received frame through the pump's handling. Returns false where the reference's pump
    // thread would have died on an uncaught exception (a non-list frame, a non-dict element, an
    // incomparable `q`): such a death leaves pump_error UNSET, exactly as it does in Python.
    bool ingest(const std::string& raw);

private:
    struct Entry {
        kp::massive::OptionQuote quote;
        kp::json::Value q;        // the frame's `q` (absent -> 0), for the sequence rule
    };

    void connect_and_auth();
    void close_socket();
    void close_raw();
    void spawn();
    void join_pump();
    std::size_t send(const char* action, const std::vector<std::string>& tickers);

    std::vector<std::string> tickers_;
    std::string key_;
    WsFactory ws_factory_;
    Clock clock_;
    std::string raw_path_;
    std::string url_;
    bool threaded_;

    std::unique_ptr<WsTransport> ws_;
    std::FILE* raw_fh_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;          // latest_, counters, status log, last_msg_t_, pump_error_
    std::mutex send_mu_;             // sends from the loop thread vs. nothing else on this socket
    std::unordered_map<std::string, Entry> latest_;
    long frame_count_ = 0, msg_count_ = 0, status_dropped_ = 0;
    std::deque<kp::json::Value> status_log_;
    std::optional<double> last_msg_t_;
    std::optional<std::string> pump_error_;
    std::optional<double> pump_exited_at_;   // monotonic instant the pump died (cleared on reconnect)

    std::set<std::string> subscribed_;
    double last_recenter_ = 0.0;
    long recenter_count_ = 0;
    long reconnects_ = 0;
};

}  // namespace kp::massive::client

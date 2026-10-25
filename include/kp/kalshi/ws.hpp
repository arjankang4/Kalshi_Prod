// kalshi/ws.py -> kp::kalshi::MarketFeed: one authenticated socket, one book per market, our fills
// and our positions. The reason it exists is BUDGET: Kalshi meters REST reads and commands, never
// frames, so a subscribed book costs a handful of commands per session instead of 10 tokens a poll.
//
// VALIDITY IS THE POINT. A book is quotable only if a snapshot has been seen for it AND no sequence
// gap has occurred since; `seq` is per SUBSCRIPTION, so one gap invalidates every book until each is
// re-snapshotted (get_snapshot, chunked at 100, paced at 0.25 s + any error-27 back-off). entry()
// returns nullopt for an invalid book, never a stale or half-applied one.
//
// THE STATE MACHINE IS SEPARATE FROM THE SOCKET, so it is golden-tested with no socket at all:
//   - on_raw(text)  is the pump's per-frame body (stamp, parse, count, tee, handle, catch);
//   - run_pump(recv) is the pump loop over any frame source;
//   - the sender (set_sender) and the clock are injectable; start() wires both to a real
//     net::WebSocket and a thread, stop() unwinds them.
//
// books() returns {ticker: {"yes": [[price, size], ...], "no": [...]}} as kp::json::Value -- the
// exact shape `quoting.top_of_book` / `quoting.ladders` parse (levels ASCENDING, best LAST, prices
// "%.4f" dollars, sizes "%.2f" contracts), which is also a subset of what REST /markets/orderbooks
// returns, so one parser serves both book sources.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/net/http.hpp"
#include "kp/net/ws.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/storage/book.hpp"

namespace kp::kalshi {

inline const std::vector<std::string> WS_CHANNELS = {"orderbook_delta", "fill", "market_positions"};
inline const std::vector<std::string> MARKET_SCOPED_CHANNELS = {"orderbook_delta"};
inline constexpr int ERR_MARKET_LIMIT = 26, ERR_TOO_MANY_COMMANDS = 27;
inline constexpr double COMMAND_MIN_INTERVAL_S = 0.25;
inline constexpr double COMMAND_BACKOFF_S = 2.0;
inline constexpr double COMMAND_BACKOFF_MAX_S = 30.0;
inline constexpr std::size_t MAX_SNAPSHOT_REQUEST = 100;
inline constexpr std::size_t MAX_SUBSCRIBE_MARKETS = 500;

// time.monotonic / time.sleep / time.time_ns for the feed, injectable.
struct FeedClock {
    virtual ~FeedClock() = default;
    virtual double monotonic();
    virtual void sleep(double s);
    virtual std::int64_t time_ns();
    static FeedClock& real();
};

class MarketFeed {
public:
    using SendFn = std::function<void(const std::string&)>;        // throws on failure
    using TeeFn = std::function<void(std::int64_t recv_ns, const kp::json::Value& frame)>;
    using RecvFn = std::function<std::optional<std::string>()>;    // nullopt = timeout; throws = dead

    explicit MarketFeed(std::vector<std::string> tickers,
                        std::vector<std::string> channels = WS_CHANNELS,
                        TeeFn tee = nullptr, FeedClock* clock = nullptr);
    ~MarketFeed();
    MarketFeed(const MarketFeed&) = delete;
    MarketFeed& operator=(const MarketFeed&) = delete;

    // --- lifecycle (the socket) -----------------------------------------------------------------
    // Connect (no Origin header; `auth` builds the signed headers per attempt), start the pump,
    // THEN subscribe (a chunked subscribe waits for the first ack, which the pump records).
    MarketFeed& start(const std::string& ws_url, const std::function<kp::net::Headers()>& auth,
                      int attempts = 4, double backoff = 3.0);
    MarketFeed& stop();
    bool alive() const;

    // --- the state machine ----------------------------------------------------------------------
    void set_sender(SendFn send) { send_ = std::move(send); }
    void on_raw(const std::string& raw);                // one received frame, as the pump sees it
    void run_pump(const RecvFn& recv);                   // until request_stop() or recv throws
    void request_stop() { stop_ = true; }
    void handle(const kp::json::Value& msg);             // _handle; throws what the reference raises

    // Commands. subscribe returns the first command id (nullopt if none was sent).
    std::optional<long> subscribe(const std::vector<std::string>& tickers, std::size_t chunk = 0,
                                  double sid_timeout = 10.0);
    int request_snapshot(const std::vector<std::string>& tickers);

    // --- what the quote loop reads --------------------------------------------------------------
    std::optional<kp::json::Value> entry(const std::string& ticker) const;
    kp::json::Value books(const std::optional<std::vector<std::string>>& tickers = std::nullopt) const;
    std::vector<kp::json::Value> drain_fills();
    std::vector<kp::json::Value> pending_fills() const;  // a peek, for tests
    kp::json::Value positions() const;                   // {ticker: float}
    double position(const std::string& ticker) const;    // 0.0 when never held
    // Only fills gaps: a ticker the socket already reported wins. Returns len(positions).
    long seed_positions(const kp::json::Value& positions);
    kp::json::Value health(std::optional<double> now = std::nullopt) const;
    double wait_for_coverage(double fraction = 0.8, double timeout = 20.0, double poll = 0.25);

    // --- introspection (the reference's attributes) --------------------------------------------
    const std::vector<std::string>& tickers() const { return tickers_; }
    std::optional<std::string> pump_error() const;
    kp::json::Value command_errors() const;
    kp::json::Value sids() const;                        // [[channel, sid], ...] in arrival order
    kp::json::Value valid_map() const;                   // [[ticker, bool], ...] in _valid order
    std::vector<std::string> book_tickers() const;       // list(self._books)
    long cmd_id() const { return cmd_id_; }
    double next_cmd_t() const { return next_cmd_t_; }
    double cmd_backoff() const { return cmd_backoff_.load(); }

private:
    std::optional<long> send(kp::json::Value payload);
    void on_error(const kp::json::Value& msg);
    void on_snapshot(const kp::json::Value& body, const kp::json::Value& seq);
    void on_delta(const kp::json::Value& body);
    void set_position(const kp::json::Value* ticker, const kp::json::Value* value_fp);
    kp::json::Value await_sid(const std::string& channel, double timeout);
    kp::json::Value sid_of(const std::string& channel) const;
    void invalidate_all();

    std::vector<std::string> tickers_;
    std::vector<std::string> channels_;
    TeeFn tee_;
    FeedClock* clock_;
    SendFn send_;

    mutable std::mutex mu_;                               // books/valid/fills/positions/counters
    kp::pycompat::ODict<std::string, kp::storage::Book> books_;
    kp::pycompat::ODict<std::string, bool> valid_;
    std::vector<kp::json::Value> fills_;
    kp::pycompat::ODict<std::string, double> positions_;
    std::vector<std::pair<kp::json::Value, kp::json::Value>> sids_;   // channel -> sid, dict-ordered

    std::recursive_mutex cmd_mu_;
    long cmd_id_ = 0;
    double next_cmd_t_ = 0.0;
    std::atomic<double> cmd_backoff_{0.0};     // read by health() without the command lock

    kp::json::Value seq_;                                 // None until the first seq-bearing frame
    long gaps_ = 0, frames_ = 0, reconnects_ = 0, commands_ = 0;
    kp::json::Value command_errors_ = kp::json::Value::make_array({});
    std::optional<std::string> pump_error_;
    std::optional<double> last_frame_t_, started_t_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> pumping_{false};
    std::unique_ptr<kp::net::WebSocket> ws_;
    std::thread thread_;
};

// _fp / _cc: int micro-dollars -> "%.4f" dollars; int centi-contracts -> "%.2f" contracts.
std::string fp_micros(std::int64_t micros);
std::string cc_centi(std::int64_t centi);

}  // namespace kp::kalshi

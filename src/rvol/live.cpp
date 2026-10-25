#include "kp/rvol/live.hpp"
#include "kp/kalshi/bucket.hpp"

#include <cstdio>
#include <stdexcept>

#include "kp/config.hpp"
#include "kp/rvol/dataset.hpp"
#include "kp/rvol/features.hpp"

namespace kp::rvol::live {

Source source_of(const std::string& underlying) {
    if (underlying == "SPX") return {"I:SPX", "SPY", "ES"};
    if (underlying == "NDX") return {"I:NDX", "QQQ", "NQ"};
    throw std::out_of_range("KeyError: '" + underlying + "'");
}

IndexBuffer::IndexBuffer(std::string underlying, double hours)
    : underlying_(std::move(underlying)), hours_(hours) {}

bool IndexBuffer::backfill(const kp::time::Instant& now, const Fetch& fetch) {
    const kp::time::EtParts et = kp::time::to_et(now);
    char day[16];
    std::snprintf(day, sizeof day, "%04d-%02d-%02d", et.year, et.month, et.day);
    const Source src = source_of(underlying_);            // outside the try, as in the reference
    try {
        if (!fetch.closes_ms) throw std::runtime_error("no closes_ms fetch wired");
        const auto rows = fetch.closes_ms(underlying_, day);
        {
            std::lock_guard<std::mutex> lk(mu_);
            t_.clear();
            p_.clear();
            for (const auto& r : rows) {
                t_.push_back(r.first);
                p_.push_back(r.second);
            }
        }
        if (!fetch.second_bars) throw std::runtime_error("no second_bars fetch wired");
        const auto bars = fetch.second_bars(src.proxy, day, day);
        std::vector<std::int64_t> pt;
        std::vector<double> pp;
        for (const auto& b : bars) {
            // `b.get("c") and b["c"] > 0`: present, non-zero, and positive (NaN fails the last).
            if (b.c && *b.c != 0.0 && *b.c > 0) {
                pt.push_back(b.t);
                pp.push_back(*b.c);
            }
        }
        proxy_t_ = std::move(pt);
        proxy_p_ = std::move(pp);
        backfilled_ = true;
    } catch (const std::exception& e) {
        std::printf("[rvol.live] backfill failed (%s: %s) -- lean stays inert until the buffer "
                    "fills\n", kp::kalshi::py_type_name(e).c_str(),
                    kp::kalshi::utf8_prefix(e.what(), 80).c_str());
        std::fflush(stdout);
        backfilled_ = false;
    }
    return backfilled_;
}

bool IndexBuffer::append(const kp::massive::index::IndexQuote* quote) {
    if (quote == nullptr || !quote->ts_ns) return false;
    // `quote.ts_ns // 1_000_000`: floor division.
    std::int64_t t_ms = *quote->ts_ns / 1000000;
    if (*quote->ts_ns % 1000000 < 0) --t_ms;
    std::lock_guard<std::mutex> lk(mu_);
    if (!t_.empty() && t_ms <= t_.back()) return false;
    // The reference appends the timestamp and THEN raises TypeError on float(None), leaving the
    // two lists one element apart for the rest of the session. That state is not reproduced: the
    // quote is refused before anything is written. Unreachable live -- massive.index.snapshot
    // never builds a quote without a value.
    if (!quote->value) throw std::invalid_argument("TypeError: float() argument must be a real number, not 'NoneType'");
    t_.push_back(t_ms);
    p_.push_back(*quote->value);
    const std::int64_t cutoff = t_ms - (std::int64_t)(hours_ * 3600 * 1000);
    std::size_t drop = 0;
    while (drop < t_.size() && t_[drop] < cutoff) ++drop;
    if (drop) {
        t_.erase(t_.begin(), t_.begin() + (std::ptrdiff_t)drop);
        p_.erase(p_.begin(), p_.begin() + (std::ptrdiff_t)drop);
    }
    return true;
}

std::pair<std::vector<std::int64_t>, std::vector<double>> IndexBuffer::arrays() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {t_, p_};
}

double IndexBuffer::span_s() const {
    std::lock_guard<std::mutex> lk(mu_);
    return t_.size() > 1 ? (double)(t_.back() - t_.front()) / 1000.0 : 0.0;
}

LiveRealizedVol::LiveRealizedVol(std::string underlying,
                                 std::shared_ptr<const kp::pricing::varcal::VarianceCalendar> calendar,
                                 std::shared_ptr<IndexBuffer> buffer, std::string root)
    : underlying_(std::move(underlying)), root_(std::move(root)), cal_(std::move(calendar)),
      buf_(std::move(buffer)) {
    if (!cal_) {
        cal_ = std::make_shared<const kp::pricing::varcal::VarianceCalendar>(
            kp::pricing::varcal::VarianceCalendar::load(underlying_, "default", root_));
    }
    if (!buf_) buf_ = std::make_shared<IndexBuffer>(underlying_);
}

std::optional<model::Features> LiveRealizedVol::features(const kp::time::Instant& now,
                                                         double mono_s, bool force) {
    const double t = mono_s;
    if (!force && cache_ && (t - cache_t_) < kp::config::LEAN_RECOMPUTE_S) return cache_;
    const auto [ts, px] = buf_->arrays();
    if (ts.size() < 2) return std::nullopt;
    static const std::vector<std::int64_t> no_t;
    static const std::vector<double> no_p;
    const auto& pt = buf_->proxy_t() ? *buf_->proxy_t() : no_t;
    const auto& pp = buf_->proxy_p() ? *buf_->proxy_p() : no_p;
    model::Features out;
    for (const auto& lb : dataset::LOOKBACKS) {
        // The per-lookback scale is the whole point -- see dataset.hpp.
        const auto w = features::window_sigma(*cal_, ts, px, pt, pp,
                                               features::wall_minus_minutes(now, lb.minutes), now,
                                               lb.scale_s, nullptr, root_);
        const std::string n(lb.name);
        out.set("f_" + n, w.sigma);
        out.set("pf_" + n, w.proxy_frac);
        out.set("cov_" + n, w.coverage);
    }
    cache_ = out;
    cache_t_ = t;
    return out;
}

}  // namespace kp::rvol::live

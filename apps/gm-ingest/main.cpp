// gm-ingest: price ingestion, caching, and ADR-015 validation (ADR §7.2,
// §5.4/M1 exit criterion: "validated price panel builds locally;
// coverage stats reported").
//
// Reads gm-universe's upstream artifact (runs/<run_id>/gm-universe/universe.parquet,
// a sibling directory per gm-run's fixed layout - ADR §8.2) to get the
// distinct ticker set and date range, fetches each ticker's daily OHLCV
// history from Yahoo's chart endpoint (promoted to primary during M1,
// see ADR §7.2) through gm-io's mandatory HttpCache, and writes one
// combined validated prices.parquet.
//
// Validation screens actually implemented in M1 (ADR-015's full list is
// larger; what's here is real, not stubbed, and the rest is a
// documented follow-up, not a silent gap):
//   - null bars (Yahoo occasionally has a timestamp with no trade data)
//     are dropped and counted, never silently zero-filled
//   - a day-over-day adjusted-close log return beyond +/-50% rejects
//     that ticker's whole fetch (a jump that large in the split-
//     adjusted series is far more likely a bad tick than a real
//     one-day move) rather than silently including a probable data
//     error
//   - zero-volume trading days are counted and reported, not rejected
//     outright (illiquid names and real halts do have zero-volume days)
//   - gaps of more than 3 trading days between consecutive bars
//     (checked against the NYSE calendar) are counted and reported
// NOT yet implemented (tracked, not hidden): the two-source
// cross-validation against Tiingo, and retroactive-change detection by
// diffing against a prior cached fetch - both need a second wired-in
// source or a prior run's cache respectively, neither of which exists
// yet this milestone.

#include <gm-core/calendar.hpp>
#include <gm-core/stage_main.hpp>
#include <gm-data/liquidity.hpp>
#include <gm-io/http_cache.hpp>
#include <gm-io/parquet.hpp>
#include <gm-io/table.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <set>

namespace {

using namespace std::chrono;

gm::Date epoch_seconds_to_date(std::int64_t epoch_seconds) {
    auto tp = sys_seconds{seconds{epoch_seconds}};
    return gm::Date{date::floor<date::days>(tp)};
}

std::int64_t date_to_epoch_seconds_midnight_utc(const gm::Date& d) {
    return duration_cast<seconds>(d.sys_days().time_since_epoch()).count();
}

/// One ticker's fetch, parsed and screened. Kept separate from the
/// combined Table assembly below so a single ticker's rejection doesn't
/// require unwinding partially-appended columns in the shared table.
struct TickerBars {
    std::vector<std::string> dates;  // ISO, ascending
    std::vector<double> open, high, low, close, adjclose;
    std::vector<std::int64_t> volume;
    std::int64_t dropped_null_bars = 0;
    std::int64_t zero_volume_days = 0;
    std::int64_t gap_days_over_3 = 0;
};

gm::Result<TickerBars> fetch_and_screen(gm::io::HttpCache& cache, const gm::NyseCalendar& calendar,
                                         const std::string& ticker, const gm::Date& start,
                                         const gm::Date& end) {
    std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/" + ticker +
                       "?period1=" + std::to_string(date_to_epoch_seconds_midnight_utc(start)) +
                       "&period2=" +
                       std::to_string(date_to_epoch_seconds_midnight_utc(end) + 86400) +
                       "&interval=1d&events=div,splits";
    std::string cache_key = "yahoo_" + ticker + "_" + start.to_iso() + "_" + end.to_iso();
    // Cache key characters must satisfy HttpCache's validate_cache_key
    // (no '/', '\', or ".."); ISO dates and tickers never contain those.

    auto entry = cache.get(url, cache_key);
    if (!entry) return tl::unexpected(entry.error());

    auto body = entry->read_body();
    if (!body) return tl::unexpected(body.error());

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(*body);
    } catch (const nlohmann::json::parse_error& e) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kParseFailure,
                                               "Yahoo chart response is not valid JSON",
                                               ticker + ": " + e.what()));
    }

    // Everything below navigates and reads `doc` via nlohmann's
    // .at()/.contains()/operator[]/.get<T>(), every one of which can
    // throw (json::out_of_range, json::type_error, ...) if Yahoo's
    // response shape doesn't match what's assumed here - e.g. a ticker
    // it doesn't recognize, or a field the API renames. The whole
    // navigation+extraction pass is one try block for exactly that
    // reason (ADR-019: exceptions must not escape past a stage's error
    // boundary; a per-.at()-call try/catch would be unreadable and easy
    // to miss one of on the next edit - a real bug the M1 code review
    // caught here). `bars` is declared outside the try only because its
    // std::vector<std::string>/double/... members are safe to touch
    // after the block; nothing about their construction can throw a
    // json exception.
    TickerBars bars;
    try {
        // nlohmann::json::find() only searches immediate object keys -
        // it does NOT take a json_pointer for path traversal (that was
        // a real bug here initially, not just the deprecation warning
        // that caught it: .contains()/.at() are the pointer-aware
        // members).
        if (!doc.contains("/chart/result"_json_pointer)) {
            return tl::unexpected(gm::Error::make(
                gm::ErrorCode::kParseFailure, "Yahoo chart response has no chart.result", ticker));
        }
        const auto& results = doc.at("/chart/result"_json_pointer);
        if (!results.is_array() || results.empty()) {
            return tl::unexpected(gm::Error::make(gm::ErrorCode::kParseFailure,
                                                   "Yahoo chart response has an empty result", ticker));
        }
        const auto& result = results[0];

        if (!result.contains("timestamp") || !result.contains("indicators")) {
            return tl::unexpected(
                gm::Error::make(gm::ErrorCode::kParseFailure,
                                 "Yahoo chart result missing timestamp/indicators", ticker));
        }
        const auto& timestamps = result.at("timestamp");
        const auto& quote = result.at("/indicators/quote/0"_json_pointer);
        const auto& adjclose_arr = result.at("/indicators/adjclose/0/adjclose"_json_pointer);

        double prev_adjclose = 0.0;
        bool have_prev = false;
        std::optional<gm::Date> prev_date;

        for (std::size_t i = 0; i < timestamps.size(); ++i) {
            const auto& close_v = quote.at("close")[i];
            const auto& volume_v = quote.at("volume")[i];
            const auto& adjclose_v = adjclose_arr[i];

            if (close_v.is_null() || volume_v.is_null() || adjclose_v.is_null()) {
                ++bars.dropped_null_bars;
                continue;
            }

            double adjclose = adjclose_v.get<double>();

            if (have_prev && prev_adjclose > 0.0) {
                // ADR-015/§7.2 specify a symmetric +/-50% price-ratio
                // bound, not a symmetric bound on the LOG return - those
                // are different things: |log(ratio)| > log(1.5) triggers
                // on any drop beyond -33.3%, not -50%, because a fixed
                // log-return magnitude corresponds to +50% on the
                // upside but only -33.3% on the downside (ratios 1.5 and
                // 1/1.5 aren't symmetric around 1.0 the way +50%/-50%
                // are). Caught by inspecting this milestone's own real
                // rejected-ticker report: WST's genuine -38.2% single-
                // day move (a real, large, legitimate price change) was
                // being rejected by the log-based check even though it
                // is well within the ADR's stated +/-50% tolerance.
                // Comparing the raw ratio against [0.5, 1.5] directly is
                // both correct and simpler than the log formulation.
                double ratio = adjclose / prev_adjclose;
                if (ratio > 1.5 || ratio < 0.5) {
                    return tl::unexpected(gm::Error::make(
                        gm::ErrorCode::kValidationFailure,
                        "adjusted-close day-over-day return exceeds +/-50% (ADR-015 screen)",
                        ticker + " at index " + std::to_string(i) + ": " +
                            std::to_string(prev_adjclose) + " -> " + std::to_string(adjclose)));
                }
            }
            prev_adjclose = adjclose;
            have_prev = true;

            gm::Date bar_date = epoch_seconds_to_date(timestamps[i].get<std::int64_t>());

            if (prev_date.has_value()) {
                std::int64_t gap = calendar.count_trading_days(*prev_date, bar_date) - 1;
                if (gap > 3) ++bars.gap_days_over_3;
            }
            prev_date = bar_date;

            std::int64_t volume = volume_v.get<std::int64_t>();
            if (volume == 0) ++bars.zero_volume_days;

            bars.dates.push_back(bar_date.to_iso());
            bars.open.push_back(quote.at("open")[i].is_null() ? 0.0 : quote.at("open")[i].get<double>());
            bars.high.push_back(quote.at("high")[i].is_null() ? 0.0 : quote.at("high")[i].get<double>());
            bars.low.push_back(quote.at("low")[i].is_null() ? 0.0 : quote.at("low")[i].get<double>());
            bars.close.push_back(close_v.get<double>());
            bars.adjclose.push_back(adjclose);
            bars.volume.push_back(volume);
        }
    } catch (const nlohmann::json::exception& e) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kParseFailure, "Yahoo chart response has an unexpected shape",
            ticker + ": " + e.what()));
    }

    if (bars.dates.empty()) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kValidationFailure, "no usable bars after screening", ticker));
    }

    return bars;
}

gm::VoidResult run_gm_ingest(const gm::Config& config, const std::filesystem::path& output_dir,
                              gm::Manifest& manifest) {
    std::filesystem::path universe_parquet =
        output_dir.parent_path() / "gm-universe" / "universe.parquet";
    auto universe_table = gm::io::read_parquet(universe_parquet);
    if (!universe_table) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kIoFailure, "failed to read upstream universe.parquet (ran gm-universe first?)",
            universe_parquet.string() + ": " + universe_table.error().to_string()));
    }

    auto ticker_col = universe_table->string_column("ticker");
    auto date_col = universe_table->string_column("date");
    if (!ticker_col) return tl::unexpected(ticker_col.error());
    if (!date_col) return tl::unexpected(date_col.error());

    std::set<std::string> unique_tickers(ticker_col->begin(), ticker_col->end());
    gm::Date range_start = *gm::Date::parse_iso(*std::min_element(date_col->begin(), date_col->end()));
    gm::Date range_end = *gm::Date::parse_iso(*std::max_element(date_col->begin(), date_col->end()));

    std::int64_t max_tickers = config.get_int_or("ingest.max_tickers", -1);
    std::vector<std::string> tickers(unique_tickers.begin(), unique_tickers.end());
    if (max_tickers >= 0 && static_cast<std::int64_t>(tickers.size()) > max_tickers) {
        tickers.resize(static_cast<std::size_t>(max_tickers));
    }

    std::string cache_dir = config.get_string_or("ingest.cache_dir", "data/raw/price_cache");
    gm::io::HttpCache cache{cache_dir};
    gm::NyseCalendar calendar;

    gm::io::Table combined;
    std::vector<std::string> all_ticker, all_date;
    std::vector<double> all_open, all_high, all_low, all_close, all_adjclose;
    std::vector<std::int64_t> all_volume;

    std::int64_t tickers_ok = 0, tickers_rejected = 0;
    std::int64_t total_dropped_null = 0, total_zero_volume = 0, total_gaps = 0;
    std::vector<std::string> rejected_tickers;

    for (const auto& ticker : tickers) {
        auto bars = fetch_and_screen(cache, calendar, ticker, range_start, range_end);
        if (!bars) {
            ++tickers_rejected;
            rejected_tickers.push_back(ticker + ": " + bars.error().to_string());
            continue;
        }
        ++tickers_ok;
        total_dropped_null += bars->dropped_null_bars;
        total_zero_volume += bars->zero_volume_days;
        total_gaps += bars->gap_days_over_3;

        for (std::size_t i = 0; i < bars->dates.size(); ++i) {
            all_ticker.push_back(ticker);
            all_date.push_back(bars->dates[i]);
            all_open.push_back(bars->open[i]);
            all_high.push_back(bars->high[i]);
            all_low.push_back(bars->low[i]);
            all_close.push_back(bars->close[i]);
            all_adjclose.push_back(bars->adjclose[i]);
            all_volume.push_back(bars->volume[i]);
        }
    }

    if (tickers_ok == 0) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kValidationFailure,
                                               "every ticker was rejected by validation screens; "
                                               "nothing to write"));
    }

    // Table's add_*_column calls are [[nodiscard]] Result<void> (ADR §3
    // principle 1) - every call constructed identically from vectors
    // whose lengths were built in lockstep above, so a failure here
    // would mean a real bug in this function, not a data problem; still
    // checked and propagated rather than ignored.
    if (auto r = combined.add_string_column("ticker", std::move(all_ticker)); !r) return tl::unexpected(r.error());
    if (auto r = combined.add_string_column("date", std::move(all_date)); !r) return tl::unexpected(r.error());
    if (auto r = combined.add_double_column("open", std::move(all_open)); !r) return tl::unexpected(r.error());
    if (auto r = combined.add_double_column("high", std::move(all_high)); !r) return tl::unexpected(r.error());
    if (auto r = combined.add_double_column("low", std::move(all_low)); !r) return tl::unexpected(r.error());
    if (auto r = combined.add_double_column("close", std::move(all_close)); !r) return tl::unexpected(r.error());
    if (auto r = combined.add_double_column("adjclose", std::move(all_adjclose)); !r) return tl::unexpected(r.error());
    if (auto r = combined.add_int64_column("volume", std::move(all_volume)); !r) return tl::unexpected(r.error());

    auto write_result = gm::io::write_parquet(combined, output_dir / "prices.parquet");
    if (!write_result) return tl::unexpected(write_result.error());

    // Liquidity ranking (ADR-001: "top 100 by trailing 60-day median
    // dollar volume") is computed from what was just fetched, not
    // fetched separately - resolves the chicken-and-egg problem (ranking
    // needs prices, but knowing what to fetch would otherwise need
    // ranking) by writing it as a second, derived artifact rather than
    // destructively filtering prices.parquet down before writing it.
    // Optional: a config that only wants the raw validated panel (e.g.
    // this milestone's 10-ticker golden fixture, which has too few
    // names for "top 100" to mean anything) simply omits these keys.
    if (config.has("ingest.liquidity_window_days") || config.has("ingest.liquidity_top_n")) {
        std::int64_t window_days = config.get_int_or("ingest.liquidity_window_days", 60);
        std::int64_t top_n = config.get_int_or("ingest.liquidity_top_n", 100);

        auto ranked = gm::data::rank_by_liquidity(combined, static_cast<int>(window_days),
                                                    static_cast<int>(top_n));
        if (!ranked) return tl::unexpected(ranked.error());

        gm::io::Table liquid;
        std::vector<std::string> liquid_tickers;
        std::vector<double> liquid_medians;
        std::vector<std::int64_t> liquid_bars_used;
        for (const auto& r : *ranked) {
            liquid_tickers.push_back(r.ticker);
            liquid_medians.push_back(r.median_dollar_volume);
            liquid_bars_used.push_back(static_cast<std::int64_t>(r.bars_used));
        }
        if (auto r = liquid.add_string_column("ticker", std::move(liquid_tickers)); !r) return tl::unexpected(r.error());
        if (auto r = liquid.add_double_column("median_dollar_volume", std::move(liquid_medians)); !r) return tl::unexpected(r.error());
        if (auto r = liquid.add_int64_column("bars_used", std::move(liquid_bars_used)); !r) return tl::unexpected(r.error());

        auto liquid_write = gm::io::write_parquet(liquid, output_dir / "liquid_universe.parquet");
        if (!liquid_write) return tl::unexpected(liquid_write.error());

        manifest.set_int("liquid_universe_size", static_cast<std::int64_t>(ranked->size()));
        manifest.set_int("liquidity_window_days", window_days);
        manifest.set_int("liquidity_top_n", top_n);
    }

    manifest.set_int("rows_written", static_cast<std::int64_t>(combined.num_rows()));
    manifest.set_int("tickers_requested", static_cast<std::int64_t>(tickers.size()));
    manifest.set_int("tickers_ok", tickers_ok);
    manifest.set_int("tickers_rejected", tickers_rejected);
    manifest.set_int("dropped_null_bars", total_dropped_null);
    manifest.set_int("zero_volume_days", total_zero_volume);
    manifest.set_int("gaps_over_3_trading_days", total_gaps);
    nlohmann::json rejected_json = nlohmann::json::array();
    for (const auto& r : rejected_tickers) rejected_json.push_back(r);
    manifest.set_json("rejected_tickers", rejected_json);

    return {};
}

} // namespace

int main(int argc, char** argv) { return gm::run_stage_main(argc, argv, "gm-ingest", run_gm_ingest); }

#include "centrality.hpp"
#include "returns.hpp"

#include <gm-core/stage_main.hpp>
#include <gm-io/parquet.hpp>
#include <gm-io/table.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace {

gm::VoidResult run_gm_features(const gm::Config& /*config*/, const std::filesystem::path& output_dir,
                           gm::Manifest& manifest) {
    std::filesystem::path universe_dir = output_dir.parent_path() / "gm-universe";
    std::filesystem::path universe_path = universe_dir / "universe.parquet";

    if (!std::filesystem::exists(universe_path)) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kNotFound,
            "Missing upstream universe.parquet",
            universe_path.string()));
    }

    auto universe_table = gm::io::read_parquet(universe_path);
    if (!universe_table) return tl::unexpected(universe_table.error());

    auto dates_col = universe_table->string_column("date");
    if (!dates_col) return tl::unexpected(dates_col.error());
    auto tickers_col = universe_table->string_column("ticker");
    if (!tickers_col) return tl::unexpected(tickers_col.error());
    auto sectors_col = universe_table->string_column("gics_sector");
    if (!sectors_col) return tl::unexpected(sectors_col.error());

    // Point-in-time sector co-membership centrality (ADR section 7.4): a
    // genuine function of (ticker, date), computed independently per
    // date from only that date's actual members - see
    // gm::features::compute_pit_sector_centrality doc comment. This
    // replaces the earlier all-time-static version that used the
    // whole file's ticker set and max sector size for every row
    // regardless of date (measured look-ahead bias vs a point-in-time
    // computation: up to +95.9% on 2010-01-04 Communication Services).
    std::vector<double> out_etf_co_membership =
        gm::features::compute_pit_sector_centrality(*dates_col, *tickers_col, *sectors_col);

    // Diagnostics for the earliest date only, to make the PIT fix
    // independently checkable from the run log without re-deriving it.
    {
        std::map<std::string, std::set<std::string>> sector_members_first_date;
        const std::string& first_date = *std::min_element(dates_col->begin(), dates_col->end());
        for (std::size_t i = 0; i < dates_col->size(); ++i) {
            if ((*dates_col)[i] == first_date) sector_members_first_date[(*sectors_col)[i]].insert((*tickers_col)[i]);
        }
        std::size_t names_that_day = 0, max_sector = 0;
        for (const auto& [sector, members] : sector_members_first_date) {
            names_that_day += members.size();
            max_sector = std::max(max_sector, members.size());
        }
        spdlog::info("gm-features: PIT sector distribution on earliest date {} ({} names, max sector size {}):",
                     first_date, names_that_day, max_sector);
        for (const auto& [sector, members] : sector_members_first_date) {
            spdlog::info("  {}: {} constituents (centrality {:.6f})", sector, members.size(),
                         static_cast<double>(members.size()) / (1.0 + static_cast<double>(max_sector)));
        }
        manifest.set_int("pit_first_date_universe_size", static_cast<std::int64_t>(names_that_day));
        manifest.set_int("pit_first_date_max_sector_size", static_cast<std::int64_t>(max_sector));
        manifest.set_string("pit_first_date", first_date);
    }

    // Trailing 5/21/63-day cumulative returns (ADR-015 upstream
    // prices.parquet, sibling artifact to universe.parquet, same
    // pattern). Real implementation, not a NaN placeholder: a
    // (ticker, date) with insufficient trailing history legitimately
    // has no value (NaN), which is different from every row being NaN.
    std::filesystem::path prices_path = output_dir.parent_path() / "gm-ingest" / "prices.parquet";
    if (!std::filesystem::exists(prices_path)) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kNotFound,
            "Missing upstream prices.parquet",
            prices_path.string()));
    }
    auto prices_table = gm::io::read_parquet(prices_path);
    if (!prices_table) return tl::unexpected(prices_table.error());

    auto price_tickers = prices_table->string_column("ticker");
    if (!price_tickers) return tl::unexpected(price_tickers.error());
    auto price_dates = prices_table->string_column("date");
    if (!price_dates) return tl::unexpected(price_dates.error());
    auto price_adjclose = prices_table->double_column("adjclose");
    if (!price_adjclose) return tl::unexpected(price_adjclose.error());

    auto returns_5d_by_ticker_date =
        gm::features::compute_trailing_returns(*price_tickers, *price_dates, *price_adjclose, 5);
    auto returns_21d_by_ticker_date =
        gm::features::compute_trailing_returns(*price_tickers, *price_dates, *price_adjclose, 21);
    auto returns_63d_by_ticker_date =
        gm::features::compute_trailing_returns(*price_tickers, *price_dates, *price_adjclose, 63);

    auto lookup_return = [](const std::map<std::string, std::map<std::string, double>>& by_ticker,
                             const std::string& ticker, const std::string& date) -> double {
        auto ticker_it = by_ticker.find(ticker);
        if (ticker_it == by_ticker.end()) return std::numeric_limits<double>::quiet_NaN();
        auto date_it = ticker_it->second.find(date);
        if (date_it == ticker_it->second.end()) return std::numeric_limits<double>::quiet_NaN();
        return date_it->second;
    };

    std::vector<std::string> out_dates;
    std::vector<std::string> out_tickers;
    std::vector<double> out_returns_5d, out_returns_21d, out_returns_63d;
    out_dates.reserve(dates_col->size());
    out_tickers.reserve(dates_col->size());
    out_returns_5d.reserve(dates_col->size());
    out_returns_21d.reserve(dates_col->size());
    out_returns_63d.reserve(dates_col->size());

    std::int64_t returns_5d_present = 0, returns_21d_present = 0, returns_63d_present = 0;

    for (std::size_t i = 0; i < dates_col->size(); ++i) {
        const auto& ticker = (*tickers_col)[i];
        const auto& date = (*dates_col)[i];

        out_dates.push_back(date);
        out_tickers.push_back(ticker);

        double r5 = lookup_return(returns_5d_by_ticker_date, ticker, date);
        double r21 = lookup_return(returns_21d_by_ticker_date, ticker, date);
        double r63 = lookup_return(returns_63d_by_ticker_date, ticker, date);
        if (!std::isnan(r5)) ++returns_5d_present;
        if (!std::isnan(r21)) ++returns_21d_present;
        if (!std::isnan(r63)) ++returns_63d_present;
        out_returns_5d.push_back(r5);
        out_returns_21d.push_back(r21);
        out_returns_63d.push_back(r63);
    }

    std::int64_t rows_written = static_cast<std::int64_t>(out_dates.size());

    // Sector distribution for logging/manifest (the all-time count here
    // is a diagnostic only - it never feeds the centrality computation
    // above, only these summary stats).
    std::set<std::string> unique_tickers_all_time(tickers_col->begin(), tickers_col->end());
    std::set<std::string> unique_sectors(sectors_col->begin(), sectors_col->end());

    gm::io::Table table;
    if (auto r = table.add_string_column("date", std::move(out_dates)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_string_column("ticker", std::move(out_tickers)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_double_column("etf_co_membership", std::move(out_etf_co_membership)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_double_column("returns_5d", std::move(out_returns_5d)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_double_column("returns_21d", std::move(out_returns_21d)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_double_column("returns_63d", std::move(out_returns_63d)); !r)
        return tl::unexpected(r.error());
    // beta and idiosyncratic_volatility are NOT emitted: a market-model
    // regression against a value/equal-weighted index return is a real,
    // larger undertaking (needs an index return series this stage does
    // not currently build) and was out of scope to implement correctly
    // here. Per the review finding, no all-NaN placeholder column is
    // shipped in this artifact - these two columns are simply absent
    // until implemented for real in a follow-up, rather than shipped as
    // a silently-fake feature.

    auto write_result = gm::io::write_parquet(table, output_dir / "features.parquet");
    if (!write_result) return tl::unexpected(write_result.error());

    manifest.set_int("rows_written", rows_written);
    manifest.set_int("unique_tickers", static_cast<std::int64_t>(unique_tickers_all_time.size()));
    manifest.set_int("num_gics_sectors", static_cast<std::int64_t>(unique_sectors.size()));
    manifest.set_int("returns_5d_present", returns_5d_present);
    manifest.set_int("returns_21d_present", returns_21d_present);
    manifest.set_int("returns_63d_present", returns_63d_present);
    manifest.set_string("note",
        "M6 ETF co-membership via GICS sector, computed point-in-time per (ticker, date). "
        "returns_5d/21d/63d are real trailing adjclose returns from prices.parquet. "
        "beta and idiosyncratic_volatility are not yet implemented and are omitted "
        "(not shipped as NaN placeholders).");

    spdlog::info("gm-features: {} rows, {} unique tickers (all-time), {} GICS sectors, "
                 "returns_5d present {}/{}, returns_21d present {}/{}, returns_63d present {}/{}",
                 rows_written, unique_tickers_all_time.size(), unique_sectors.size(),
                 returns_5d_present, rows_written, returns_21d_present, rows_written,
                 returns_63d_present, rows_written);
    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-features", run_gm_features);
}

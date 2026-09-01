#include <gm-core/stage_main.hpp>
#include <gm-data/etf_holdings.hpp>
#include <gm-io/csv.hpp>
#include <gm-io/http_cache.hpp>
#include <gm-io/parquet.hpp>
#include <gm-io/table.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace {

gm::Result<gm::data::EtfHoldings> parse_ishares_csv(const std::string& csv_text) {
    auto parsed = gm::io::parse_csv(csv_text);
    if (!parsed) return tl::unexpected(parsed.error());

    auto ticker_col_idx = parsed->column_index("Ticker");
    if (!ticker_col_idx) {
        ticker_col_idx = parsed->column_index("ticker");
    }
    if (!ticker_col_idx) {
        ticker_col_idx = parsed->column_index("Symbol");
    }

    if (!ticker_col_idx) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kParseFailure,
            "iShares CSV missing ticker column"));
    }

    gm::data::EtfHoldings holdings;
    std::set<std::string> seen;

    for (const auto& row : parsed->rows) {
        if (*ticker_col_idx >= row.size()) continue;

        std::string ticker = row[*ticker_col_idx];
        ticker.erase(0, ticker.find_first_not_of(" \t\r\n"));
        ticker.erase(ticker.find_last_not_of(" \t\r\n") + 1);

        if (ticker.empty() || ticker == "Total") continue;

        if (seen.find(ticker) == seen.end()) {
            holdings.push_back(ticker);
            seen.insert(ticker);
        }
    }

    return holdings;
}

gm::Result<gm::data::EtfHoldings> fetch_etf_holdings(
    gm::io::HttpCache& cache,
    const gm::data::EtfConfig& etf) {
    auto entry = cache.get(etf.csv_url, etf.holdings_csv_cache_key);
    if (!entry) {
        return tl::unexpected(entry.error());
    }

    auto body = entry->read_body();
    if (!body) return tl::unexpected(body.error());

    return parse_ishares_csv(*body);
}

gm::Result<gm::data::AllHoldings> load_all_holdings(
    const std::vector<gm::data::EtfConfig>& etf_configs,
    gm::io::HttpCache& cache) {
    gm::data::AllHoldings all_holdings;
    int successful = 0;
    int total = 0;

    for (const auto& etf : etf_configs) {
        total++;
        auto result = fetch_etf_holdings(cache, etf);
        if (!result) {
            spdlog::debug("Failed to fetch {}: {}", etf.etf_ticker, result.error().to_string());
            continue;
        }

        all_holdings[etf.etf_ticker] = *result;
        successful++;
        spdlog::info("Fetched {} holdings: {} constituents", etf.etf_ticker, result->size());
    }

    spdlog::info("ETF fetching: {} succeeded out of {}", successful, total);
    
    if (successful == 0) {
        spdlog::warn("No ETF holdings fetched (network error or test environment); "
                    "co-membership centrality will be all zeros");
        return all_holdings;
    }

    return all_holdings;
}

gm::VoidResult run_gm_features(const gm::Config& config, const std::filesystem::path& output_dir,
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

    std::string cache_dir = config.get_string_or("features.cache_dir", "data/raw/etf_cache");
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kIoFailure,
                                               "failed to create cache directory",
                                               cache_dir));
    }

    gm::io::HttpCache cache(cache_dir);
    auto etf_configs = gm::data::default_etf_config();

    spdlog::info("Fetching {} ETFs for co-membership analysis", etf_configs.size());

    auto holdings = load_all_holdings(etf_configs, cache);
    if (!holdings) return tl::unexpected(holdings.error());

    std::map<std::string, double> centrality;
    std::map<std::string, int> ticker_count;

    for (const auto& [etf_ticker, tickers_held] : *holdings) {
        for (const auto& ticker : tickers_held) {
            ticker_count[ticker]++;
        }
    }

    double num_etfs = static_cast<double>(holdings->size());
    if (num_etfs > 0.0) {
        for (const auto& [ticker, count] : ticker_count) {
            centrality[ticker] = static_cast<double>(count) / num_etfs;
        }
    }

    spdlog::info("Computed centrality for {} unique tickers from {} ETFs", 
                 centrality.size(), holdings->size());

    std::vector<std::string> out_dates;
    std::vector<std::string> out_tickers;
    std::vector<double> out_etf_co_membership;

    for (std::size_t i = 0; i < dates_col->size(); ++i) {
        const auto& ticker = (*tickers_col)[i];
        const auto& date = (*dates_col)[i];

        double score = 0.0;
        auto it = centrality.find(ticker);
        if (it != centrality.end()) {
            score = it->second;
        }

        out_dates.push_back(date);
        out_tickers.push_back(ticker);
        out_etf_co_membership.push_back(score);
    }

    std::int64_t rows_written = static_cast<std::int64_t>(out_dates.size());
    std::vector<double> nan_column(out_dates.size(), std::numeric_limits<double>::quiet_NaN());

    gm::io::Table table;
    if (auto r = table.add_string_column("date", std::move(out_dates)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_string_column("ticker", std::move(out_tickers)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_double_column("etf_co_membership", std::move(out_etf_co_membership)); !r)
        return tl::unexpected(r.error());
    if (auto r = table.add_double_column("returns_5d", nan_column); !r)
        return tl::unexpected(r.error());

    auto write_result = gm::io::write_parquet(table, output_dir / "features.parquet");
    if (!write_result) return tl::unexpected(write_result.error());

    manifest.set_int("rows_written", rows_written);
    manifest.set_int("etfs_fetched", static_cast<std::int64_t>(holdings->size()));
    manifest.set_int("unique_tickers_with_membership", static_cast<std::int64_t>(centrality.size()));
    manifest.set_string("note", "M6 ETF co-membership layer (ADR 7.4)");

    spdlog::info("gm-features: {} rows, {} ETFs, {} tickers with membership",
                 rows_written, holdings->size(), centrality.size());
    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-features", run_gm_features);
}

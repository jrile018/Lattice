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

    std::map<std::string, std::set<std::string>> sector_members;
    std::map<std::string, std::string> ticker_sector;

    for (std::size_t i = 0; i < tickers_col->size(); ++i) {
        const auto& ticker = (*tickers_col)[i];
        const auto& sector = (*sectors_col)[i];
        
        if (ticker_sector.find(ticker) == ticker_sector.end()) {
            ticker_sector[ticker] = sector;
            sector_members[sector].insert(ticker);
        }
    }

    std::map<std::string, double> centrality;
    std::size_t max_sector_size = 0;
    for (const auto& [sector, members] : sector_members) {
        max_sector_size = std::max(max_sector_size, members.size());
    }
    
    spdlog::info("Sector distribution:");
    for (const auto& [sector, members] : sector_members) {
        spdlog::info("  {}: {} constituents", sector, members.size());
        
        for (const auto& ticker : members) {
            double count = 1.0 + static_cast<double>(members.size() - 1);
            double normalized = count / (1.0 + static_cast<double>(max_sector_size));
            centrality[ticker] = normalized;
        }
    }

    spdlog::info("Computed sector-based centrality for {} unique tickers", centrality.size());
    spdlog::info("Max sector size: {}, normalization divisor: {}", max_sector_size, 1 + max_sector_size);

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
    manifest.set_int("unique_tickers", static_cast<std::int64_t>(centrality.size()));
    manifest.set_int("num_gics_sectors", static_cast<std::int64_t>(sector_members.size()));
    manifest.set_int("max_sector_size", static_cast<std::int64_t>(max_sector_size));
    manifest.set_string("note", "M6 ETF co-membership via GICS sector. Sector SPDR ETFs hold exactly sector constituents.");

    spdlog::info("gm-features: {} rows, {} unique tickers, {} GICS sectors",
                 rows_written, centrality.size(), sector_members.size());
    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-features", run_gm_features);
}

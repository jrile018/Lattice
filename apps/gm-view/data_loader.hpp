#pragma once

#include <gm-core/error.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gm::view {

struct RunInfo {
    std::string run_id;
    std::filesystem::path run_dir;
};

[[nodiscard]] Result<std::vector<RunInfo>> list_available_runs(
    const std::filesystem::path& runs_base_dir);

struct Frame {
    std::string date;
    std::vector<std::string> tickers;
    std::vector<std::array<float, 3>> positions;
};

struct TickerMetadata {
    std::string ticker;
    std::string security_name;
    std::string gics_sector;
};

struct Score {
    std::string date;
    std::string ticker;
    std::string view;
    std::string estimator;
    double depth;
    double pvalue;
    bool inside;
};

struct Spread {
    std::string date;
    std::string ticker;
    double z;
    double spread;
    double half_life;
    int n_neighbors;
};

struct BasketWeight {
    std::string date;
    std::string ticker;
    std::string neighbor_ticker;
    double weight;
};

struct Excursion {
    std::string ticker;
    std::string start_date;
    std::string end_date;
    double peak_depth;
    bool reverted;
    int duration_days;
};

/// SEC EDGAR company profile for the Learn panel (ADR §8.2, meta/profiles.json).
/// Field names and types here are the canonical schema and MUST match what
/// apps/gm-profiles/main.cpp writes exactly - see the comment there. There is
/// no separate "description"/"sic_industry"/"economic_links" - gm-profiles
/// does not fetch or produce that data, so this struct only carries what the
/// writer actually produces: ticker, company_name, sic_code (string - SIC
/// codes are conventionally treated as codes, not numbers), sic_description,
/// edgar_url.
struct TickerProfile {
    std::string ticker;
    std::string company_name;
    std::string sic_code;
    std::string sic_description;
    std::string edgar_url;
};

struct LoadedRun {
    std::vector<Frame> frames;
    std::vector<std::string> regime_dates;
    std::vector<double> structural_change;
    std::map<std::string, TickerMetadata> ticker_metadata;
    std::vector<Score> scores;
    std::vector<Spread> spreads;
    std::vector<BasketWeight> baskets;
    std::vector<Excursion> excursions;
    std::map<std::string, TickerProfile> profiles;

    // Built once here (not on every rendered frame - ADR-9's <1ms decode
    // budget) so the viewer's Learn panel can look a ticker/date up in
    // O(log n) instead of linear-scanning the full scores/baskets vectors
    // (1.82M / 3.92M rows on the real M6 run) on every single frame.
    std::map<std::pair<std::string, std::string>, std::vector<Score>> scores_by_ticker_date;
    std::map<std::pair<std::string, std::string>, std::vector<BasketWeight>> baskets_by_ticker_date;
    std::map<std::string, std::vector<Excursion>> excursions_by_ticker;
};

[[nodiscard]] Result<LoadedRun> load_run(const std::filesystem::path& run_dir);

} // namespace gm::view

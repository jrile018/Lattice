#pragma once

#include <gm-core/error.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
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

struct TickerProfile {
    std::string ticker;
    std::string description;
    std::string sic_code;
    std::string sic_industry;
    std::vector<std::string> economic_links;
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
};

[[nodiscard]] Result<LoadedRun> load_run(const std::filesystem::path& run_dir);

} // namespace gm::view

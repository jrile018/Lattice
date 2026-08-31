#include "data_loader.hpp"

#include <gm-io/parquet.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>

namespace gm::view {

Result<std::vector<RunInfo>> list_available_runs(const std::filesystem::path& runs_base_dir) {
    std::vector<RunInfo> runs;

    std::error_code ec;
    if (!std::filesystem::exists(runs_base_dir, ec) || !std::filesystem::is_directory(runs_base_dir, ec)) {
        return runs;  // no runs yet is a valid, empty state - not an error
    }

    for (const auto& entry : std::filesystem::directory_iterator(runs_base_dir, ec)) {
        if (!entry.is_directory()) continue;
        if (std::filesystem::exists(entry.path() / "manifest.json")) {
            runs.push_back(RunInfo{entry.path().filename().string(), entry.path()});
        }
    }
    if (ec) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kIoFailure, "failed to scan runs directory",
                                               runs_base_dir.string() + ": " + ec.message()));
    }

    std::sort(runs.begin(), runs.end(),
              [](const RunInfo& a, const RunInfo& b) { return a.run_id > b.run_id; });
    return runs;
}

Result<LoadedRun> load_run(const std::filesystem::path& run_dir) {
    std::filesystem::path geometry_path = run_dir / "gm-geometry" / "geometry.parquet";
    auto geometry = gm::io::read_parquet(geometry_path);
    if (!geometry) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kNotFound, "gm-geometry has not produced output for this run yet",
            geometry_path.string() + ": " + geometry.error().to_string()));
    }

    auto date_col = geometry->string_column("date");
    if (!date_col) return tl::unexpected(date_col.error());
    auto ticker_col = geometry->string_column("ticker");
    if (!ticker_col) return tl::unexpected(ticker_col.error());
    auto x_col = geometry->double_column("x");
    if (!x_col) return tl::unexpected(x_col.error());
    auto y_col = geometry->double_column("y");
    if (!y_col) return tl::unexpected(y_col.error());
    auto z_col = geometry->double_column("z");
    if (!z_col) return tl::unexpected(z_col.error());

    std::map<std::string, Frame> frames_by_date;
    for (std::size_t i = 0; i < date_col->size(); ++i) {
        const std::string& date = (*date_col)[i];
        Frame& frame = frames_by_date[date];
        frame.date = date;
        frame.tickers.push_back((*ticker_col)[i]);
        frame.positions.push_back({static_cast<float>((*x_col)[i]), static_cast<float>((*y_col)[i]),
                                    static_cast<float>((*z_col)[i])});
    }

    LoadedRun result;
    result.frames.reserve(frames_by_date.size());
    for (auto& [date, frame] : frames_by_date) result.frames.push_back(std::move(frame));
    // std::map iterates in ascending key order already (dates are
    // ISO-8601, sorting correctly as strings), so result.frames is
    // already date-ascending without a separate sort.

    std::filesystem::path regime_path = run_dir / "gm-geometry" / "regime.parquet";
    auto regime = gm::io::read_parquet(regime_path);
    if (regime) {
        auto regime_dates = regime->string_column("date");
        auto sc = regime->double_column("structural_change");
        if (regime_dates && sc) {
            result.regime_dates.assign(regime_dates->begin(), regime_dates->end());
            result.structural_change.assign(sc->begin(), sc->end());
        } else {
            // Distinguish "the file doesn't exist yet" (read_parquet
            // itself fails below, expected for a run gm-geometry hasn't
            // reached) from "the file exists but doesn't look like a
            // regime table" (a real schema problem) - the latter would
            // otherwise silently present as an empty Evolution tab with
            // no indication anything was wrong, and a user could easily
            // mistake that for "the run just hasn't finished yet."
            spdlog::warn(
                "gm-view: {} exists but is missing the expected 'date'/'structural_change' "
                "columns - the Evolution tab's regime strip will be empty",
                regime_path.string());
        }
    }
    // A missing regime.parquet (the common case: gm-geometry hasn't
    // produced it yet, or this run predates the metric) is tolerated
    // silently as "no regime strip to show" - only an unexpected schema
    // on a file that DOES exist is worth telling the user about.

    return result;
}

} // namespace gm::view

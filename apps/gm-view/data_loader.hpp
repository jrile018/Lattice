#pragma once

// gm-view is a pure reader (ADR-006/ADR-018): it never computes
// anything financial, only loads and displays what the pipeline stages
// already wrote. This file is the entire read boundary.

#include <gm-core/error.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace gm::view {

struct RunInfo {
    std::string run_id;
    std::filesystem::path run_dir;
};

/// Lists runs under `runs_base_dir` that have a top-level manifest.json
/// (ADR-017) - i.e. completed gm-run invocations, not partial/in-progress
/// directories. Sorted descending by run_id (most recent first, given
/// this project's run_id convention of a leading date).
[[nodiscard]] Result<std::vector<RunInfo>> list_available_runs(
    const std::filesystem::path& runs_base_dir);

struct Frame {
    std::string date;
    std::vector<std::string> tickers;
    std::vector<std::array<float, 3>> positions;  // parallel to tickers
};

struct LoadedRun {
    std::vector<Frame> frames;  // ascending by date, one per gm-geometry frame
    std::vector<std::string> regime_dates;
    std::vector<double> structural_change;  // parallel to regime_dates
};

/// Reads `<run_dir>/gm-geometry/{geometry,regime}.parquet`. Fails if
/// gm-geometry hasn't run yet for this run_id (a real, expected state
/// for a run still in progress, not something to guess a fallback for).
[[nodiscard]] Result<LoadedRun> load_run(const std::filesystem::path& run_dir);

} // namespace gm::view

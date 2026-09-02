#include "data_loader.hpp"
#include <gm-io/parquet.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <map>

namespace gm::view {

Result<std::vector<RunInfo>> list_available_runs(const std::filesystem::path& runs_base_dir) {
    std::vector<RunInfo> runs;
    std::error_code ec;
    if (!std::filesystem::exists(runs_base_dir, ec) || !std::filesystem::is_directory(runs_base_dir, ec)) {
        return runs;
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

    std::filesystem::path regime_path = run_dir / "gm-geometry" / "regime.parquet";
    auto regime = gm::io::read_parquet(regime_path);
    if (regime) {
        auto regime_dates = regime->string_column("date");
        auto sc = regime->double_column("structural_change");
        if (regime_dates && sc) {
            result.regime_dates.assign(regime_dates->begin(), regime_dates->end());
            result.structural_change.assign(sc->begin(), sc->end());
        }
    }

    std::filesystem::path universe_path = run_dir / "gm-universe" / "universe.parquet";
    auto universe = gm::io::read_parquet(universe_path);
    if (universe) {
        auto ticker_col_u = universe->string_column("ticker");
        auto security_col = universe->string_column("security_name");
        auto sector_col = universe->string_column("gics_sector");
        if (ticker_col_u && security_col && sector_col && 
            ticker_col_u->size() == security_col->size() && 
            ticker_col_u->size() == sector_col->size()) {
            for (std::size_t i = 0; i < ticker_col_u->size(); ++i) {
                result.ticker_metadata[(*ticker_col_u)[i]] = TickerMetadata{
                    (*ticker_col_u)[i], (*security_col)[i], (*sector_col)[i]};
            }
        }
    }

    std::filesystem::path scores_path = run_dir / "gm-boundaries" / "scores.parquet";
    auto scores = gm::io::read_parquet(scores_path);
    if (scores) {
        auto date_s = scores->string_column("date");
        auto ticker_s = scores->string_column("ticker");
        auto view_s = scores->string_column("view");
        auto estimator_s = scores->string_column("estimator");
        auto depth_s = scores->double_column("depth");
        auto pvalue_s = scores->double_column("pvalue");
        auto inside_s = scores->bool_column("inside");
        if (date_s && ticker_s && view_s && estimator_s && depth_s && pvalue_s && inside_s &&
            date_s->size() == ticker_s->size()) {
            result.scores.reserve(date_s->size());
            for (std::size_t i = 0; i < date_s->size(); ++i) {
                result.scores.push_back(Score{(*date_s)[i], (*ticker_s)[i], (*view_s)[i], (*estimator_s)[i],
                    (*depth_s)[i], (*pvalue_s)[i], (*inside_s)[i] != 0});
            }
            // Index once here rather than linear-scanning all rows on
            // every rendered frame in the Learn panel - see LoadedRun's
            // scores_by_ticker_date comment (data_loader.hpp).
            for (const auto& score : result.scores) {
                result.scores_by_ticker_date[{score.ticker, score.date}].push_back(score);
            }
        }
    }

    std::filesystem::path spreads_path = run_dir / "gm-signals" / "spreads.parquet";
    auto spreads = gm::io::read_parquet(spreads_path);
    if (spreads) {
        auto date_sp = spreads->string_column("date");
        auto ticker_sp = spreads->string_column("ticker");
        auto z_sp = spreads->double_column("z");
        auto spread_sp = spreads->double_column("spread");
        auto half_life_sp = spreads->double_column("half_life");
        auto n_neighbors_sp = spreads->int64_column("n_neighbors");
        if (date_sp && ticker_sp && z_sp && spread_sp && half_life_sp && n_neighbors_sp &&
            date_sp->size() == ticker_sp->size()) {
            for (std::size_t i = 0; i < date_sp->size(); ++i) {
                result.spreads.push_back(Spread{(*date_sp)[i], (*ticker_sp)[i], (*z_sp)[i],
                    (*spread_sp)[i], (*half_life_sp)[i], static_cast<int>((*n_neighbors_sp)[i])});
            }
        }
    }

    std::filesystem::path baskets_path = run_dir / "gm-signals" / "baskets.parquet";
    auto baskets = gm::io::read_parquet(baskets_path);
    if (baskets) {
        auto date_b = baskets->string_column("date");
        auto ticker_b = baskets->string_column("ticker");
        auto neighbor_b = baskets->string_column("neighbor_ticker");
        auto weight_b = baskets->double_column("weight");
        if (date_b && ticker_b && neighbor_b && weight_b && date_b->size() == ticker_b->size()) {
            result.baskets.reserve(date_b->size());
            for (std::size_t i = 0; i < date_b->size(); ++i) {
                result.baskets.push_back(BasketWeight{(*date_b)[i], (*ticker_b)[i], (*neighbor_b)[i], (*weight_b)[i]});
            }
            for (const auto& basket : result.baskets) {
                result.baskets_by_ticker_date[{basket.ticker, basket.date}].push_back(basket);
            }
        }
    }

    std::filesystem::path excursions_path = run_dir / "gm-signals" / "excursions.parquet";
    auto excursions = gm::io::read_parquet(excursions_path);
    if (excursions) {
        auto ticker_e = excursions->string_column("ticker");
        auto start_date_e = excursions->string_column("start_date");
        auto end_date_e = excursions->string_column("end_date");
        auto peak_depth_e = excursions->double_column("peak_depth");
        auto reverted_e = excursions->bool_column("reverted");
        auto duration_e = excursions->int64_column("duration_days");
        if (ticker_e && start_date_e && end_date_e && peak_depth_e && reverted_e && duration_e &&
            ticker_e->size() == start_date_e->size()) {
            result.excursions.reserve(ticker_e->size());
            for (std::size_t i = 0; i < ticker_e->size(); ++i) {
                result.excursions.push_back(Excursion{(*ticker_e)[i], (*start_date_e)[i], (*end_date_e)[i],
                    (*peak_depth_e)[i], (*reverted_e)[i] != 0, static_cast<int>((*duration_e)[i])});
            }
            for (const auto& exc : result.excursions) {
                result.excursions_by_ticker[exc.ticker].push_back(exc);
            }
        }
    }

    // meta/profiles.json (ADR §8.2) - written by gm-profiles, keyed by
    // ticker. Optional: an older run predating gm-profiles, or one where
    // every fetch failed, legitimately has no such file - that's not a
    // load failure, the Learn panel just has nothing to show for it.
    std::filesystem::path profiles_path = run_dir / "meta" / "profiles.json";
    std::error_code profiles_ec;
    if (std::filesystem::exists(profiles_path, profiles_ec)) {
        std::ifstream profiles_stream(profiles_path);
        if (profiles_stream) {
            try {
                nlohmann::json profiles_json;
                profiles_stream >> profiles_json;
                for (const auto& [ticker, entry] : profiles_json.items()) {
                    TickerProfile profile;
                    profile.ticker = entry.value("ticker", ticker);
                    profile.company_name = entry.value("company_name", std::string{});
                    profile.sic_code = entry.value("sic_code", std::string{});
                    profile.sic_description = entry.value("sic_description", std::string{});
                    profile.edgar_url = entry.value("edgar_url", std::string{});
                    result.profiles[ticker] = std::move(profile);
                }
            } catch (const nlohmann::json::exception& e) {
                spdlog::warn("gm-view: failed to parse {}: {} - Learn panel will show no profile data",
                             profiles_path.string(), e.what());
            }
        }
    }

    return result;
}

} // namespace gm::view

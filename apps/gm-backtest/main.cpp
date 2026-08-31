// gm-backtest: the ADR §6.5 walk-forward trade simulation (ADR §13 M5:
// "Walk-forward engine, cost model, DSR ... deflated, cost-netted
// walk-forward results with machine-counted trials").
//
// For each excursion gm-signals detected (a View C z-score crossing
// episode), applies EVERY ADR §6.5 entry condition at its start_date:
//   1. View C: already satisfied by construction (excursions.parquet
//      only exists where |z| already crossed z_entry) - PLUS half-life
//      in the tradable band (min/max_half_life_days), checked here.
//   2. View B: outside its own surface (gm-boundaries' scores.parquet,
//      view="B", the Mahalanobis estimator specifically - ADR-007's
//      "statistical anchor").
//   3. View A: the frame's structural-change metric below a veto
//      threshold (a data-driven quantile of its own full-history
//      distribution, not a hardcoded constant - gm-geometry's
//      regime.parquet).
//   4. No SEC 8-K filing inside the EXPECTED holding horizon
//      (start_date to start_date + horizon_multiplier*half_life) -
//      fetched directly here via gm::signals::fetch_filing_dates
//      (the SAME library gm-report uses, NOT gm-report's derived
//      excursions_tagged.parquet: gm-backtest runs BEFORE gm-report in
//      gm-run's fixed stage order, so depending on gm-report's output
//      would violate ADR-006's one-way data flow. Both stages
//      independently call the same underlying fetch, cached, so
//      whichever runs second gets a cache hit rather than a wasted
//      re-fetch - and checking against the EXPECTED horizon here is
//      actually a more precise match to ADR §6.5's literal wording
//      than gm-report's realized-excursion-span proxy).
//   5. Liquidity/borrow feasibility - a documented simplification:
//      always satisfied (no live liquidity/borrow feed exists yet;
//      the universe is already liquidity-screened at ADR-001's stage).
//
// Eligible excursions become TradeCandidates and are handed to
// gm::backtest::simulate_portfolio for a daily equal-weighted return
// series, then scored with gm::backtest::sample_moments and
// gm::backtest::deflated_sharpe_ratio (N=1 for this single default-
// parameter run - the real multi-trial DSR is gm-sweep's job, once it
// exists, using the trial count and Sharpe variance it tracks).

#include <gm-backtest/deflated_sharpe.hpp>
#include <gm-backtest/trade_simulation.hpp>
#include <gm-core/stage_main.hpp>
#include <gm-io/http_cache.hpp>
#include <gm-io/parquet.hpp>
#include <gm-io/table.hpp>
#include <gm-signals/earnings.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace {

struct SpreadPoint {
    double z;
    double spread;
    double half_life;
    int n_neighbors;
};

using SpreadsByTickerDate = std::map<std::string, std::map<std::string, SpreadPoint>>;

gm::Result<SpreadsByTickerDate> load_spreads(const gm::io::Table& t) {
    auto ticker_col = t.string_column("ticker");
    if (!ticker_col) return tl::unexpected(ticker_col.error());
    auto date_col = t.string_column("date");
    if (!date_col) return tl::unexpected(date_col.error());
    auto z_col = t.double_column("z");
    if (!z_col) return tl::unexpected(z_col.error());
    auto spread_col = t.double_column("spread");
    if (!spread_col) return tl::unexpected(spread_col.error());
    auto half_life_col = t.double_column("half_life");
    if (!half_life_col) return tl::unexpected(half_life_col.error());
    auto n_col = t.int64_column("n_neighbors");
    if (!n_col) return tl::unexpected(n_col.error());

    SpreadsByTickerDate result;
    for (std::size_t i = 0; i < ticker_col->size(); ++i) {
        result[(*ticker_col)[i]][(*date_col)[i]] =
            SpreadPoint{(*z_col)[i], (*spread_col)[i], (*half_life_col)[i], static_cast<int>((*n_col)[i])};
    }
    return result;
}

using ViewBByTickerDate = std::map<std::string, std::map<std::string, bool>>;

gm::Result<ViewBByTickerDate> load_view_b(const gm::io::Table& t) {
    auto date_col = t.string_column("date");
    if (!date_col) return tl::unexpected(date_col.error());
    auto ticker_col = t.string_column("ticker");
    if (!ticker_col) return tl::unexpected(ticker_col.error());
    auto view_col = t.string_column("view");
    if (!view_col) return tl::unexpected(view_col.error());
    auto estimator_col = t.string_column("estimator");
    if (!estimator_col) return tl::unexpected(estimator_col.error());
    auto inside_col = t.bool_column("inside");
    if (!inside_col) return tl::unexpected(inside_col.error());

    ViewBByTickerDate result;
    for (std::size_t i = 0; i < date_col->size(); ++i) {
        if ((*view_col)[i] != "B" || (*estimator_col)[i] != "mahalanobis") continue;
        result[(*ticker_col)[i]][(*date_col)[i]] = (*inside_col)[i];
    }
    return result;
}

gm::Result<std::map<std::string, double>> load_regime(const gm::io::Table& t) {
    auto date_col = t.string_column("date");
    if (!date_col) return tl::unexpected(date_col.error());
    auto sc_col = t.double_column("structural_change");
    if (!sc_col) return tl::unexpected(sc_col.error());

    std::map<std::string, double> result;
    for (std::size_t i = 0; i < date_col->size(); ++i) result[(*date_col)[i]] = (*sc_col)[i];
    return result;
}

double quantile_of(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(values.size() - 1));
    return values[idx];
}

struct ExcursionRow {
    std::string ticker, start_date, end_date;
};

gm::Result<std::vector<ExcursionRow>> load_excursions(const gm::io::Table& t) {
    auto ticker_col = t.string_column("ticker");
    if (!ticker_col) return tl::unexpected(ticker_col.error());
    auto start_col = t.string_column("start_date");
    if (!start_col) return tl::unexpected(start_col.error());
    auto end_col = t.string_column("end_date");
    if (!end_col) return tl::unexpected(end_col.error());

    std::vector<ExcursionRow> result;
    for (std::size_t i = 0; i < ticker_col->size(); ++i) {
        result.push_back(ExcursionRow{(*ticker_col)[i], (*start_col)[i], (*end_col)[i]});
    }
    return result;
}

/// Steps forward `n_trading_days` from `from_date` in `ticker`'s OWN
/// date sequence (not calendar arithmetic - weekends/holidays are not
/// trading days), capping at the last date the ticker actually has
/// data for if the horizon would run past the end of history.
std::string horizon_date(const std::map<std::string, SpreadPoint>& ticker_dates, const std::string& from_date,
                          int n_trading_days) {
    auto it = ticker_dates.find(from_date);
    if (it == ticker_dates.end()) return from_date; // shouldn't happen; defensive fallback
    for (int i = 0; i < n_trading_days && std::next(it) != ticker_dates.end(); ++i) ++it;
    return it->first;
}

gm::VoidResult run_gm_backtest(const gm::Config& config, const std::filesystem::path& output_dir,
                                gm::Manifest& manifest) {
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kIoFailure, "failed to create output directory", output_dir.string()));
    }

    std::filesystem::path signals_dir = output_dir.parent_path() / "gm-signals";
    std::filesystem::path boundaries_dir = output_dir.parent_path() / "gm-boundaries";
    std::filesystem::path geometry_dir = output_dir.parent_path() / "gm-geometry";
    std::filesystem::path universe_dir = output_dir.parent_path() / "gm-universe";

    auto excursions_table = gm::io::read_parquet(signals_dir / "excursions.parquet");
    if (!excursions_table) return tl::unexpected(excursions_table.error());
    auto spreads_table = gm::io::read_parquet(signals_dir / "spreads.parquet");
    if (!spreads_table) return tl::unexpected(spreads_table.error());
    auto scores_table = gm::io::read_parquet(boundaries_dir / "scores.parquet");
    if (!scores_table) return tl::unexpected(scores_table.error());
    auto regime_table = gm::io::read_parquet(geometry_dir / "regime.parquet");
    if (!regime_table) return tl::unexpected(regime_table.error());
    auto universe_table = gm::io::read_parquet(universe_dir / "universe.parquet");
    if (!universe_table) return tl::unexpected(universe_table.error());

    auto excursions = load_excursions(*excursions_table);
    if (!excursions) return tl::unexpected(excursions.error());
    auto spreads = load_spreads(*spreads_table);
    if (!spreads) return tl::unexpected(spreads.error());
    auto view_b = load_view_b(*scores_table);
    if (!view_b) return tl::unexpected(view_b.error());
    auto regime = load_regime(*regime_table);
    if (!regime) return tl::unexpected(regime.error());

    auto uni_ticker = universe_table->string_column("ticker");
    if (!uni_ticker) return tl::unexpected(uni_ticker.error());
    auto uni_cik = universe_table->int64_column("cik");
    if (!uni_cik) return tl::unexpected(uni_cik.error());
    std::map<std::string, std::int64_t> cik_by_ticker;
    for (std::size_t i = 0; i < uni_ticker->size(); ++i) cik_by_ticker.try_emplace((*uni_ticker)[i], (*uni_cik)[i]);

    std::int64_t min_half_life = config.get_int_or("backtest.min_half_life_days", 3);
    std::int64_t max_half_life = config.get_int_or("backtest.max_half_life_days", 30);
    double veto_percentile = config.get_double_or("backtest.structural_change_veto_percentile", 0.95);
    double cost_bps_per_leg = config.get_double_or("backtest.cost_bps_per_leg", 5.0);
    double horizon_multiplier = config.get_double_or("backtest.horizon_multiplier", 3.0);
    std::string cache_dir = config.get_string_or("backtest.sec_cache_dir", "data/raw/sec_submissions");

    std::vector<double> all_regime_values;
    all_regime_values.reserve(regime->size());
    for (const auto& [date, sc] : *regime) all_regime_values.push_back(sc);
    double veto_threshold = quantile_of(all_regime_values, veto_percentile);

    // Fetch 8-K dates only for tickers that actually have excursions -
    // same HttpCache directory gm-report uses by default, so whichever
    // of the two stages runs second in a given invocation gets a cache
    // hit rather than a redundant live fetch.
    std::set<std::string> tickers_with_excursions;
    for (const auto& e : *excursions) tickers_with_excursions.insert(e.ticker);

    gm::io::HttpCache http_cache(cache_dir);
    std::map<std::string, std::vector<std::string>> filing_dates_by_ticker;
    for (const auto& ticker : tickers_with_excursions) {
        auto cik_it = cik_by_ticker.find(ticker);
        if (cik_it == cik_by_ticker.end()) continue;
        auto filings = gm::signals::fetch_filing_dates(http_cache, cik_it->second, {"8-K"});
        if (!filings) continue; // a single ticker's fetch failing shouldn't halt the whole backtest
        std::vector<std::string> dates;
        dates.reserve(filings->size());
        for (const auto& f : *filings) dates.push_back(f.date);
        std::sort(dates.begin(), dates.end());
        filing_dates_by_ticker[ticker] = std::move(dates);
    }

    std::int64_t rejected_no_spread_data = 0;
    std::int64_t rejected_half_life_band = 0;
    std::int64_t rejected_view_b = 0;
    std::int64_t rejected_view_a_veto = 0;
    std::int64_t rejected_earnings = 0;

    std::vector<gm::backtest::TradeCandidate> candidates;
    // ticker -> date -> spread level, the narrower lookup simulate_portfolio needs.
    std::map<std::string, std::map<std::string, double>> spread_levels;
    for (const auto& [ticker, dates] : *spreads) {
        for (const auto& [date, pt] : dates) spread_levels[ticker][date] = pt.spread;
    }

    for (const auto& exc : *excursions) {
        auto ticker_spreads_it = spreads->find(exc.ticker);
        if (ticker_spreads_it == spreads->end()) {
            ++rejected_no_spread_data;
            continue;
        }
        auto point_it = ticker_spreads_it->second.find(exc.start_date);
        if (point_it == ticker_spreads_it->second.end()) {
            ++rejected_no_spread_data;
            continue;
        }
        const SpreadPoint& entry_point = point_it->second;

        if (entry_point.half_life < static_cast<double>(min_half_life) ||
            entry_point.half_life > static_cast<double>(max_half_life)) {
            ++rejected_half_life_band;
            continue;
        }

        auto vb_ticker_it = view_b->find(exc.ticker);
        bool view_b_outside = false;
        if (vb_ticker_it != view_b->end()) {
            auto vb_date_it = vb_ticker_it->second.find(exc.start_date);
            if (vb_date_it != vb_ticker_it->second.end()) view_b_outside = !vb_date_it->second;
        }
        if (!view_b_outside) {
            ++rejected_view_b;
            continue;
        }

        auto regime_it = regime->find(exc.start_date);
        if (regime_it == regime->end() || regime_it->second >= veto_threshold) {
            ++rejected_view_a_veto;
            continue;
        }

        int horizon_days = static_cast<int>(std::ceil(horizon_multiplier * entry_point.half_life));
        std::string horizon_exit = horizon_date(ticker_spreads_it->second, exc.start_date, horizon_days);
        std::string exit_date = std::min(exc.end_date, horizon_exit);

        auto filing_it = filing_dates_by_ticker.find(exc.ticker);
        bool had_earnings_in_horizon = false;
        if (filing_it != filing_dates_by_ticker.end()) {
            auto lb = std::lower_bound(filing_it->second.begin(), filing_it->second.end(), exc.start_date);
            if (lb != filing_it->second.end() && *lb <= horizon_exit) had_earnings_in_horizon = true;
        }
        if (had_earnings_in_horizon) {
            ++rejected_earnings;
            continue;
        }

        candidates.push_back(gm::backtest::TradeCandidate{
            exc.ticker, exc.start_date, exit_date, /*long_the_spread=*/entry_point.z < 0.0,
            /*num_legs=*/1 + entry_point.n_neighbors});
    }

    auto portfolio = gm::backtest::simulate_portfolio(candidates, spread_levels, cost_bps_per_leg);
    if (!portfolio) return tl::unexpected(portfolio.error());

    gm::io::Table daily_table;
    if (auto r = daily_table.add_string_column("date", portfolio->dates); !r) return tl::unexpected(r.error());
    if (auto r = daily_table.add_double_column("return", portfolio->daily_returns); !r)
        return tl::unexpected(r.error());
    if (auto r = daily_table.add_int64_column("num_open_positions", portfolio->num_open_positions); !r)
        return tl::unexpected(r.error());

    auto write1 = gm::io::write_parquet(daily_table, output_dir / "daily_returns.parquet");
    if (!write1) return tl::unexpected(write1.error());

    nlohmann::json results;
    results["excursions_total"] = excursions->size();
    results["candidates_eligible"] = candidates.size();
    results["rejected_no_spread_data"] = rejected_no_spread_data;
    results["rejected_half_life_band"] = rejected_half_life_band;
    results["rejected_view_b"] = rejected_view_b;
    results["rejected_view_a_veto"] = rejected_view_a_veto;
    results["rejected_earnings_in_horizon"] = rejected_earnings;
    results["structural_change_veto_threshold"] = veto_threshold;
    results["trading_days_with_positions"] = portfolio->dates.size();

    if (portfolio->daily_returns.size() >= 2) {
        auto moments = gm::backtest::sample_moments(portfolio->daily_returns);
        if (!moments) return tl::unexpected(moments.error());

        double sum = 0.0;
        for (double r : portfolio->daily_returns) sum += r;
        double mean = sum / static_cast<double>(portfolio->daily_returns.size());
        double sum_sq_dev = 0.0;
        for (double r : portfolio->daily_returns) sum_sq_dev += (r - mean) * (r - mean);
        double sample_variance = sum_sq_dev / static_cast<double>(portfolio->daily_returns.size() - 1);
        double sample_std = std::sqrt(sample_variance);

        double sharpe = sample_std > 0.0 ? mean / sample_std : 0.0;
        double annualized_sharpe = sharpe * std::sqrt(252.0);

        auto sr0 = gm::backtest::expected_max_sharpe(0.0, /*n_trials=*/1);
        if (!sr0) return tl::unexpected(sr0.error());
        auto dsr = gm::backtest::deflated_sharpe_ratio(
            sharpe, *sr0, static_cast<int>(portfolio->daily_returns.size()), moments->skewness, moments->kurtosis);

        results["mean_daily_return"] = mean;
        results["sample_std_daily_return"] = sample_std;
        results["sharpe_ratio_daily"] = sharpe;
        results["sharpe_ratio_annualized"] = annualized_sharpe;
        results["skewness"] = moments->skewness;
        results["kurtosis"] = moments->kurtosis;
        results["deflated_sharpe_ratio"] = dsr ? nlohmann::json(*dsr) : nlohmann::json(nullptr);
        results["n_trials"] = 1;
        results["dsr_note"] =
            "N=1 (this single default-parameter run) - the real multi-trial DSR requires gm-sweep";

        manifest.set_double("sharpe_ratio_annualized", annualized_sharpe);
        if (dsr) manifest.set_double("deflated_sharpe_ratio", *dsr);
    } else {
        results["sharpe_ratio_daily"] = nullptr;
        results["note"] = "fewer than 2 days with open positions - not enough data for Sharpe/DSR";
    }

    std::filesystem::path results_path = output_dir / "backtest_results.json";
    std::ofstream results_out(results_path, std::ios::binary | std::ios::trunc);
    if (!results_out) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kIoFailure, "failed to write backtest results", results_path.string()));
    }
    results_out << results.dump(2);

    manifest.set_int("excursions_total", static_cast<std::int64_t>(excursions->size()));
    manifest.set_int("candidates_eligible", static_cast<std::int64_t>(candidates.size()));
    manifest.set_int("trading_days_with_positions", static_cast<std::int64_t>(portfolio->dates.size()));
    manifest.set_double("structural_change_veto_threshold", veto_threshold);
    manifest.set_double("cost_bps_per_leg", cost_bps_per_leg);
    manifest.set_int("min_half_life_days", min_half_life);
    manifest.set_int("max_half_life_days", max_half_life);

    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-backtest", run_gm_backtest);
}

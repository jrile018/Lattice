#pragma once

// Walk-forward portfolio simulation (ADR §6.5 signal, ADR-014 walk-
// forward discipline). Deliberately split from apps/gm-backtest/main.cpp
// the same way every other stage in this codebase splits math from
// I/O: this module takes ALREADY-FILTERED trade candidates (the
// eligibility logic - Views A/B/C, earnings, tradable half-life band -
// lives in the stage app, which is where the multi-source parquet
// joins naturally belong) and turns them into a daily portfolio return
// series, so the actual simulation math is testable against small
// hand-built fixtures independent of any real data file.

#include <gm-core/error.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gm::backtest {

struct TradeCandidate {
    std::string ticker;
    std::string entry_date; // inclusive
    std::string exit_date;  // inclusive; must be >= entry_date
    bool long_the_spread;   // true if entry z < 0 (spread below its mean, expect it to rise)
    int num_legs;           // 1 (the target) + number of peer-basket neighbours, for cost scaling
};

struct PortfolioResult {
    std::vector<std::string> dates;             // every date at least one position had a return contribution
    std::vector<double> daily_returns;           // equal-weighted average across that day's open positions
    std::vector<std::int64_t> num_open_positions; // how many positions contributed to that day's average
};

/// `spread_by_ticker_date` is ticker -> (date -> spread level), the
/// same log-price-weighted quantity gm-signals already computes
/// (spreads.parquet). For each candidate, walks its own ticker's
/// spread series between entry_date and exit_date (using that
/// ticker's OWN actual trading days, not calendar arithmetic - matching
/// gm-signals' own convention) and computes a daily return contribution
/// of +/-(spread[t] - spread[t-1]) depending on long_the_spread. Round-
/// trip transaction cost (`cost_bps_per_leg` * num_legs * 2, entry and
/// exit) is deducted as a lump sum split evenly across the entry day
/// and the exit day's contribution for that position - costs are
/// incurred at trade time, not smoothly amortized across the holding
/// period.
///
/// A candidate whose ticker/date range has fewer than 2 spread
/// observations (nothing to compute a return between) contributes no
/// days and is otherwise silently skipped - not a validation failure,
/// since a single-day candidate the data has no continuation for is an
/// expected data-boundary case, not a bug in the study itself.
[[nodiscard]] Result<PortfolioResult> simulate_portfolio(
    const std::vector<TradeCandidate>& candidates,
    const std::map<std::string, std::map<std::string, double>>& spread_by_ticker_date,
    double cost_bps_per_leg);

} // namespace gm::backtest

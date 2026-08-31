#include <gm-backtest/trade_simulation.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using gm::backtest::simulate_portfolio;
using gm::backtest::TradeCandidate;

namespace {
std::map<std::string, std::map<std::string, double>> single_ticker_spreads(
    const std::string& ticker, const std::vector<std::pair<std::string, double>>& points) {
    std::map<std::string, std::map<std::string, double>> result;
    for (const auto& [date, spread] : points) result[ticker][date] = spread;
    return result;
}
} // namespace

TEST_CASE("a long position's daily returns are the spread's day-over-day change", "[trade_simulation]") {
    auto spreads = single_ticker_spreads("AAPL", {{"2024-01-01", 1.0}, {"2024-01-02", 1.2}, {"2024-01-03", 1.5}});
    std::vector<TradeCandidate> candidates = {
        {"AAPL", "2024-01-01", "2024-01-03", /*long_the_spread=*/true, /*num_legs=*/2}};

    auto result = simulate_portfolio(candidates, spreads, /*cost_bps_per_leg=*/0.0);
    REQUIRE(result.has_value());
    REQUIRE(result->dates.size() == 2);
    CHECK(result->dates[0] == "2024-01-02");
    CHECK(result->daily_returns[0] == Catch::Approx(0.2));
    CHECK(result->dates[1] == "2024-01-03");
    CHECK(result->daily_returns[1] == Catch::Approx(0.3));
    CHECK(result->num_open_positions[0] == 1);
}

TEST_CASE("a short position's daily returns are the negated spread change", "[trade_simulation]") {
    auto spreads = single_ticker_spreads("AAPL", {{"2024-01-01", 1.0}, {"2024-01-02", 1.2}, {"2024-01-03", 1.5}});
    std::vector<TradeCandidate> candidates = {
        {"AAPL", "2024-01-01", "2024-01-03", /*long_the_spread=*/false, /*num_legs=*/2}};

    auto result = simulate_portfolio(candidates, spreads, 0.0);
    REQUIRE(result.has_value());
    REQUIRE(result->dates.size() == 2);
    CHECK(result->daily_returns[0] == Catch::Approx(-0.2));
    CHECK(result->daily_returns[1] == Catch::Approx(-0.3));
}

TEST_CASE("a single-return-day trade is charged the full round-trip cost on that one day",
          "[trade_simulation]") {
    auto spreads = single_ticker_spreads("AAPL", {{"2024-01-01", 1.0}, {"2024-01-02", 1.2}});
    std::vector<TradeCandidate> candidates = {{"AAPL", "2024-01-01", "2024-01-02", true, /*num_legs=*/2}};

    // total_cost = (10/10000) * 2 legs * 2 (entry+exit) = 0.004
    auto result = simulate_portfolio(candidates, spreads, /*cost_bps_per_leg=*/10.0);
    REQUIRE(result.has_value());
    REQUIRE(result->dates.size() == 1);
    CHECK(result->daily_returns[0] == Catch::Approx(0.2 - 0.004));
}

TEST_CASE("a multi-day trade splits round-trip cost across the first and last day only",
          "[trade_simulation]") {
    auto spreads = single_ticker_spreads(
        "AAPL", {{"2024-01-01", 1.0}, {"2024-01-02", 1.1}, {"2024-01-03", 1.3}, {"2024-01-04", 1.6}});
    std::vector<TradeCandidate> candidates = {{"AAPL", "2024-01-01", "2024-01-04", true, /*num_legs=*/1}};

    // total_cost = (10/10000) * 1 leg * 2 = 0.002, split 0.001 entry / 0.001 exit
    auto result = simulate_portfolio(candidates, spreads, /*cost_bps_per_leg=*/10.0);
    REQUIRE(result.has_value());
    REQUIRE(result->dates.size() == 3);
    CHECK(result->daily_returns[0] == Catch::Approx(0.1 - 0.001));  // entry day: charged
    CHECK(result->daily_returns[1] == Catch::Approx(0.2));          // middle day: no cost
    CHECK(result->daily_returns[2] == Catch::Approx(0.3 - 0.001));  // exit day: charged
}

TEST_CASE("two candidates on different tickers on overlapping dates are equal-weight averaged",
          "[trade_simulation]") {
    std::map<std::string, std::map<std::string, double>> spreads;
    spreads["AAPL"] = {{"2024-01-01", 1.0}, {"2024-01-02", 1.4}}; // +0.4
    spreads["MSFT"] = {{"2024-01-01", 2.0}, {"2024-01-02", 1.8}}; // -0.2

    std::vector<TradeCandidate> candidates = {
        {"AAPL", "2024-01-01", "2024-01-02", true, 1},  // contributes +0.4
        {"MSFT", "2024-01-01", "2024-01-02", true, 1},  // contributes -0.2
    };

    auto result = simulate_portfolio(candidates, spreads, 0.0);
    REQUIRE(result.has_value());
    REQUIRE(result->dates.size() == 1);
    CHECK(result->daily_returns[0] == Catch::Approx((0.4 + (-0.2)) / 2.0));
    CHECK(result->num_open_positions[0] == 2);
}

TEST_CASE("a candidate for a ticker with no spread data is silently skipped, not an error",
          "[trade_simulation]") {
    auto spreads = single_ticker_spreads("AAPL", {{"2024-01-01", 1.0}, {"2024-01-02", 1.2}});
    std::vector<TradeCandidate> candidates = {{"NOTREAL", "2024-01-01", "2024-01-02", true, 1}};

    auto result = simulate_portfolio(candidates, spreads, 0.0);
    REQUIRE(result.has_value());
    CHECK(result->dates.empty());
}

TEST_CASE("a candidate with only one spread observation in range contributes nothing", "[trade_simulation]") {
    auto spreads = single_ticker_spreads("AAPL", {{"2024-01-01", 1.0}, {"2024-06-01", 5.0}});
    // entry/exit span only captures the single 2024-01-01 point.
    std::vector<TradeCandidate> candidates = {{"AAPL", "2024-01-01", "2024-01-01", true, 1}};

    auto result = simulate_portfolio(candidates, spreads, 0.0);
    REQUIRE(result.has_value());
    CHECK(result->dates.empty());
}

TEST_CASE("exit_date before entry_date is rejected", "[trade_simulation]") {
    auto spreads = single_ticker_spreads("AAPL", {{"2024-01-01", 1.0}});
    std::vector<TradeCandidate> candidates = {{"AAPL", "2024-01-05", "2024-01-01", true, 1}};
    auto result = simulate_portfolio(candidates, spreads, 0.0);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("num_legs < 1 is rejected", "[trade_simulation]") {
    auto spreads = single_ticker_spreads("AAPL", {{"2024-01-01", 1.0}});
    std::vector<TradeCandidate> candidates = {{"AAPL", "2024-01-01", "2024-01-02", true, 0}};
    auto result = simulate_portfolio(candidates, spreads, 0.0);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("negative cost_bps_per_leg is rejected", "[trade_simulation]") {
    std::vector<TradeCandidate> candidates;
    auto result = simulate_portfolio(candidates, {}, -1.0);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("an empty candidate list produces an empty result, not an error", "[trade_simulation]") {
    auto result = simulate_portfolio({}, {}, 5.0);
    REQUIRE(result.has_value());
    CHECK(result->dates.empty());
}

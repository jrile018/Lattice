#include "valuation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using gm::features::compute_valuation_panel;
using gm::features::compute_valuation_yields;
using gm::features::FundamentalsRow;
using gm::features::select_as_of;
using gm::features::ValuationStatus;

namespace {

constexpr double kTol = 1e-9;

// A plausible mid-cap: 100m shares, $50 close -> $5bn market cap.
// EV = 5bn + 1bn debt - 0.5bn cash = 5.5bn.
FundamentalsRow sample_row() {
    FundamentalsRow r;
    r.ticker = "AAA";
    r.period_end = "2024-03-31";
    r.available_date = "2024-05-10";
    r.net_income_ttm = 250e6;
    r.ebitda_ttm = 700e6;
    r.free_cash_flow_ttm = 400e6;
    r.total_debt = 1000e6;
    r.cash_and_equivalents = 500e6;
    r.shares_outstanding = 100e6;
    return r;
}

FundamentalsRow quarter(std::string ticker, std::string period_end, std::string available_date) {
    FundamentalsRow r = sample_row();
    r.ticker = std::move(ticker);
    r.period_end = std::move(period_end);
    r.available_date = std::move(available_date);
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// The as-of rule
// ---------------------------------------------------------------------------

TEST_CASE("select_as_of: the most recently PUBLISHED period wins, not the most recent period",
          "[gm-features][valuation]") {
    // Q1 ends 31 March but is not published until 10 May; Q2 ends 30 June and
    // is published 9 August. This is the whole reason the rule exists.
    const std::vector<FundamentalsRow> rows{
        quarter("AAA", "2024-03-31", "2024-05-10"),
        quarter("AAA", "2024-06-30", "2024-08-09"),
    };

    SECTION("on a day between the two publications, Q1 is the latest available") {
        auto i = select_as_of(rows, "AAA", "2024-07-01");
        REQUIRE(i.has_value());
        CHECK(rows[*i].period_end == "2024-03-31");
    }

    SECTION("the day before Q2 is published, Q1 is still the answer") {
        auto i = select_as_of(rows, "AAA", "2024-08-08");
        REQUIRE(i.has_value());
        CHECK(rows[*i].period_end == "2024-03-31");
    }

    SECTION("on the publication day itself, Q2 becomes available (the rule is <=, not <)") {
        auto i = select_as_of(rows, "AAA", "2024-08-09");
        REQUIRE(i.has_value());
        CHECK(rows[*i].period_end == "2024-06-30");
    }
}

TEST_CASE("select_as_of: joining on period_end instead would leak the future",
          "[gm-features][valuation]") {
    // This is the discriminating test for the entire point-in-time rule, and
    // it is written as a contrast rather than an assertion about our own
    // output, because the bug it guards against is silent - it improves the
    // equity curve rather than breaking anything.
    //
    // On 2024-04-01 the quarter ending 2024-03-31 is OVER but unpublished.
    // A period_end <= D join returns it. The correct rule returns nothing,
    // because on 1 April no market participant had those numbers.
    const std::vector<FundamentalsRow> rows{
        quarter("AAA", "2023-12-31", "2024-02-14"),
        quarter("AAA", "2024-03-31", "2024-05-10"),
    };
    const std::string d = "2024-04-01";

    // What the wrong rule would have said.
    std::optional<std::size_t> by_period_end;
    for (std::size_t k = 0; k < rows.size(); ++k) {
        if (rows[k].period_end <= d &&
            (!by_period_end || rows[k].period_end > rows[*by_period_end].period_end)) {
            by_period_end = k;
        }
    }
    REQUIRE(by_period_end.has_value());
    CHECK(rows[*by_period_end].period_end == "2024-03-31");

    // What the rule actually implemented says: the previous quarter, which is
    // the newest thing anyone could have read on 1 April.
    auto i = select_as_of(rows, "AAA", d);
    REQUIRE(i.has_value());
    CHECK(rows[*i].period_end == "2023-12-31");
    CHECK(*i != *by_period_end);
}

TEST_CASE("select_as_of: nothing published yet is absent, not zero",
          "[gm-features][valuation]") {
    const std::vector<FundamentalsRow> rows{quarter("AAA", "2024-03-31", "2024-05-10")};
    CHECK_FALSE(select_as_of(rows, "AAA", "2024-05-09").has_value());
    CHECK_FALSE(select_as_of(rows, "AAA", "2010-01-01").has_value());
    CHECK(select_as_of(rows, "AAA", "2024-05-10").has_value());
}

TEST_CASE("select_as_of: a restatement supersedes only once it is itself published",
          "[gm-features][valuation]") {
    // Same period_end, published twice: the original in May, a restatement in
    // November. Tie-breaking on available_date is what makes the second one
    // win, and only from November onward.
    std::vector<FundamentalsRow> rows{
        quarter("AAA", "2024-03-31", "2024-05-10"),
        quarter("AAA", "2024-03-31", "2024-11-01"),
    };
    rows[0].net_income_ttm = 250e6;
    rows[1].net_income_ttm = 180e6; // restated downward

    auto before = select_as_of(rows, "AAA", "2024-10-31");
    REQUIRE(before.has_value());
    CHECK(rows[*before].net_income_ttm == 250e6);

    auto after = select_as_of(rows, "AAA", "2024-11-01");
    REQUIRE(after.has_value());
    CHECK(rows[*after].net_income_ttm == 180e6);
}

TEST_CASE("select_as_of: a later period already published outranks an earlier restatement",
          "[gm-features][valuation]") {
    // Q1 published, Q2 published, then Q1 restated. The answer stays Q2:
    // greatest period_end first, available_date only as a tie-break.
    std::vector<FundamentalsRow> rows{
        quarter("AAA", "2024-03-31", "2024-05-10"),
        quarter("AAA", "2024-06-30", "2024-08-09"),
        quarter("AAA", "2024-03-31", "2024-09-20"),
    };
    auto i = select_as_of(rows, "AAA", "2024-12-31");
    REQUIRE(i.has_value());
    CHECK(rows[*i].period_end == "2024-06-30");
}

TEST_CASE("select_as_of: the answer does not depend on the order of the input rows",
          "[gm-features][valuation]") {
    std::vector<FundamentalsRow> rows{
        quarter("AAA", "2023-12-31", "2024-02-14"),
        quarter("AAA", "2024-03-31", "2024-05-10"),
        quarter("AAA", "2024-06-30", "2024-08-09"),
        quarter("AAA", "2024-09-30", "2024-11-07"),
    };
    // Every permutation of four rows, so this is exhaustive rather than a
    // sample: a comparison that happened to work on sorted input would show up.
    std::sort(rows.begin(), rows.end(),
              [](const FundamentalsRow& a, const FundamentalsRow& b) {
                  return a.available_date < b.available_date;
              });
    int permutations = 0;
    do {
        ++permutations;
        auto i = select_as_of(rows, "AAA", "2024-08-20");
        REQUIRE(i.has_value());
        INFO("permutation " << permutations);
        CHECK(rows[*i].period_end == "2024-06-30");
    } while (std::next_permutation(
        rows.begin(), rows.end(), [](const FundamentalsRow& a, const FundamentalsRow& b) {
            return a.available_date < b.available_date;
        }));
    CHECK(permutations == 24);
}

TEST_CASE("select_as_of: other tickers' filings are never selected",
          "[gm-features][valuation]") {
    const std::vector<FundamentalsRow> rows{
        quarter("AAA", "2024-03-31", "2024-05-10"),
        quarter("BBB", "2024-06-30", "2024-08-09"),
    };
    auto i = select_as_of(rows, "AAA", "2024-12-31");
    REQUIRE(i.has_value());
    CHECK(rows[*i].ticker == "AAA");
    CHECK(rows[*i].period_end == "2024-03-31");

    CHECK_FALSE(select_as_of(rows, "CCC", "2024-12-31").has_value());
}

// ---------------------------------------------------------------------------
// The yields
// ---------------------------------------------------------------------------

TEST_CASE("compute_valuation_yields: values are the hand-computed ones",
          "[gm-features][valuation]") {
    // 100m shares at $50 -> 5.0e9 market cap.
    // EV = 5.0e9 + 1.0e9 - 0.5e9 = 5.5e9.
    const auto point = compute_valuation_yields(sample_row(), 50.0);
    REQUIRE(point.status == ValuationStatus::kOk);
    CHECK(std::abs(point.yields.earnings_yield - 250e6 / 5.0e9) < kTol);
    CHECK(std::abs(point.yields.ebitda_ev_yield - 700e6 / 5.5e9) < kTol);
    CHECK(std::abs(point.yields.fcf_yield - 400e6 / 5.0e9) < kTol);
}

TEST_CASE("compute_valuation_yields: market cap uses the price given, so the yields move daily",
          "[gm-features][valuation]") {
    // The reason a 756-day window holds 756 distinct points and not 12
    // (ADR-022): the numerator steps quarterly, the denominator does not.
    const auto row = sample_row();
    const auto cheap = compute_valuation_yields(row, 25.0);
    const auto dear = compute_valuation_yields(row, 100.0);
    REQUIRE(cheap.status == ValuationStatus::kOk);
    REQUIRE(dear.status == ValuationStatus::kOk);
    CHECK(cheap.yields.earnings_yield > dear.yields.earnings_yield);
    // Halving the price exactly doubles the earnings yield.
    const auto base = compute_valuation_yields(row, 50.0);
    CHECK(std::abs(cheap.yields.earnings_yield - 2.0 * base.yields.earnings_yield) < kTol);
}

TEST_CASE("compute_valuation_yields: earnings yield stays finite and continuous through zero",
          "[gm-features][valuation]") {
    // The property that motivates yields over multiples. P/E would diverge
    // here and flip sign; E/P walks smoothly through zero.
    auto row = sample_row();
    double previous = 1.0;
    for (double earnings : {100e6, 10e6, 1e6, 0.0, -1e6, -10e6, -100e6}) {
        row.net_income_ttm = earnings;
        const auto point = compute_valuation_yields(row, 50.0);
        REQUIRE(point.status == ValuationStatus::kOk);
        const double y = point.yields.earnings_yield;
        INFO("earnings = " << earnings << " -> E/P = " << y);
        CHECK(std::isfinite(y));
        CHECK(y < previous); // monotone decreasing, no sign discontinuity
        previous = y;
    }
    row.net_income_ttm = 0.0;
    CHECK(compute_valuation_yields(row, 50.0).yields.earnings_yield == 0.0);
}

TEST_CASE("compute_valuation_yields: a non-positive enterprise value has no coordinate at all",
          "[gm-features][valuation]") {
    // A company trading below its net cash. EBITDA/EV would flip sign, and a
    // sign-flipped coordinate is not an outlier a robust estimator can
    // absorb - it is a different quantity pointing the wrong way. ADR-022
    // excludes the ticker-day rather than clamping the denominator.
    auto row = sample_row();
    row.cash_and_equivalents = 8000e6; // EV = 5.0e9 + 1.0e9 - 8.0e9 < 0
    const auto point = compute_valuation_yields(row, 50.0);
    CHECK(point.status == ValuationStatus::kNonPositiveEnterpriseValue);

    SECTION("exactly zero is also excluded, not divided by") {
        auto zero_ev = sample_row();
        zero_ev.cash_and_equivalents = 6000e6; // EV = 5.0e9 + 1.0e9 - 6.0e9 = 0
        CHECK(compute_valuation_yields(zero_ev, 50.0).status ==
              ValuationStatus::kNonPositiveEnterpriseValue);
    }
}

TEST_CASE("compute_valuation_yields: a small but positive EV is left alone, not clamped",
          "[gm-features][valuation]") {
    // The deliberate other half of the decision above. Clamping a denominator
    // to keep a number small is the eigenvalue-floor mistake; FastMCD's job
    // downstream is precisely to ignore a minority of extreme points.
    auto row = sample_row();
    row.cash_and_equivalents = 5999e6; // EV = 1.0e6, tiny but positive
    const auto point = compute_valuation_yields(row, 50.0);
    REQUIRE(point.status == ValuationStatus::kOk);
    CHECK(std::abs(point.yields.ebitda_ev_yield - 700e6 / 1.0e6) < 1e-6);
    CHECK(point.yields.ebitda_ev_yield > 100.0); // genuinely enormous, and kept
}

TEST_CASE("compute_valuation_yields: non-positive price or share count is rejected",
          "[gm-features][valuation]") {
    CHECK(compute_valuation_yields(sample_row(), 0.0).status ==
          ValuationStatus::kNonPositiveMarketCap);
    CHECK(compute_valuation_yields(sample_row(), -50.0).status ==
          ValuationStatus::kNonPositiveMarketCap);

    auto no_shares = sample_row();
    no_shares.shares_outstanding = 0.0;
    CHECK(compute_valuation_yields(no_shares, 50.0).status ==
          ValuationStatus::kNonPositiveMarketCap);
}

TEST_CASE("compute_valuation_yields: an absent source field is reported, never used",
          "[gm-features][valuation]") {
    // Free fundamentals sources leave fields empty; a Parquet double column
    // carries that as NaN. NaN is never a sentinel in this codebase
    // (ADR-019), so it has to be turned back into an explicit status here
    // rather than propagated into a coordinate.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double bad : {nan, inf, -inf}) {
        auto row = sample_row();
        row.ebitda_ttm = bad;
        CHECK(compute_valuation_yields(row, 50.0).status == ValuationStatus::kNonFiniteInput);

        auto row2 = sample_row();
        row2.total_debt = bad;
        CHECK(compute_valuation_yields(row2, 50.0).status == ValuationStatus::kNonFiniteInput);

        CHECK(compute_valuation_yields(sample_row(), bad).status ==
              ValuationStatus::kNonFiniteInput);
    }
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

TEST_CASE("compute_valuation_panel: days before the first filing carry no coordinate",
          "[gm-features][valuation]") {
    const std::vector<FundamentalsRow> fundamentals{quarter("AAA", "2024-03-31", "2024-05-10")};
    const std::vector<std::string> tickers{"AAA", "AAA", "AAA"};
    const std::vector<std::string> dates{"2024-05-08", "2024-05-09", "2024-05-10"};
    const std::vector<double> close{50.0, 50.0, 50.0};

    const auto panel = compute_valuation_panel(fundamentals, tickers, dates, close);
    REQUIRE(panel.yields.count("AAA") == 1);
    const auto& series = panel.yields.at("AAA");
    CHECK(series.count("2024-05-08") == 0);
    CHECK(series.count("2024-05-09") == 0);
    CHECK(series.count("2024-05-10") == 1);
    CHECK(panel.status_counts.at(ValuationStatus::kNoFundamentalsAvailable) == 2);
    CHECK(panel.status_counts.at(ValuationStatus::kOk) == 1);
}

TEST_CASE("compute_valuation_panel: tickers are independent", "[gm-features][valuation]") {
    std::vector<FundamentalsRow> fundamentals{
        quarter("AAA", "2024-03-31", "2024-05-10"),
        quarter("BBB", "2024-03-31", "2024-08-01"),
    };
    fundamentals[0].net_income_ttm = 250e6;
    fundamentals[1].net_income_ttm = 500e6;

    const std::vector<std::string> tickers{"AAA", "BBB", "AAA", "BBB"};
    const std::vector<std::string> dates{"2024-06-01", "2024-06-01", "2024-09-01", "2024-09-01"};
    const std::vector<double> close{50.0, 50.0, 50.0, 50.0};

    const auto panel = compute_valuation_panel(fundamentals, tickers, dates, close);
    CHECK(panel.yields.at("AAA").count("2024-06-01") == 1);
    CHECK(panel.yields.at("BBB").count("2024-06-01") == 0); // not published until August
    CHECK(panel.yields.at("BBB").count("2024-09-01") == 1);
    CHECK(std::abs(panel.yields.at("BBB").at("2024-09-01").earnings_yield - 500e6 / 5.0e9) < kTol);
}

TEST_CASE("compute_valuation_panel: every ticker-day is accounted for in exactly one status",
          "[gm-features][valuation]") {
    // The counts are published in the stage manifest as coverage, so they
    // have to tally: no ticker-day may be silently dropped.
    std::vector<FundamentalsRow> fundamentals{quarter("AAA", "2024-03-31", "2024-05-10")};
    fundamentals.push_back(quarter("BBB", "2024-03-31", "2024-05-10"));
    fundamentals[1].cash_and_equivalents = 8000e6; // BBB has negative EV

    const std::vector<std::string> tickers{"AAA", "AAA", "BBB", "BBB", "CCC"};
    const std::vector<std::string> dates{"2024-05-09", "2024-06-01", "2024-05-09", "2024-06-01",
                                         "2024-06-01"};
    const std::vector<double> close{50.0, 0.0, 50.0, 50.0, 50.0};

    const auto panel = compute_valuation_panel(fundamentals, tickers, dates, close);
    std::int64_t total = 0;
    for (const auto& [status, count] : panel.status_counts) total += count;
    CHECK(total == static_cast<std::int64_t>(tickers.size()));

    CHECK(panel.status_counts.at(ValuationStatus::kNoFundamentalsAvailable) == 3); // AAA/BBB 05-09, CCC
    CHECK(panel.status_counts.at(ValuationStatus::kNonPositiveEnterpriseValue) == 1);
    CHECK(panel.status_counts.at(ValuationStatus::kNonPositiveMarketCap) == 1);
    CHECK(panel.status_counts.count(ValuationStatus::kOk) == 0);
}

TEST_CASE("compute_valuation_panel: mismatched price column lengths yield nothing rather than "
          "reading past the end",
          "[gm-features][valuation]") {
    const std::vector<FundamentalsRow> fundamentals{quarter("AAA", "2024-03-31", "2024-05-10")};
    const std::vector<std::string> tickers{"AAA", "AAA"};
    const std::vector<std::string> dates{"2024-06-01"};
    const std::vector<double> close{50.0, 50.0};

    const auto panel = compute_valuation_panel(fundamentals, tickers, dates, close);
    CHECK(panel.yields.empty());
    CHECK(panel.status_counts.empty());
}

TEST_CASE("to_string covers every status", "[gm-features][valuation]") {
    for (ValuationStatus s : {ValuationStatus::kOk, ValuationStatus::kNoFundamentalsAvailable,
                              ValuationStatus::kNonFiniteInput,
                              ValuationStatus::kNonPositiveMarketCap,
                              ValuationStatus::kNonPositiveEnterpriseValue}) {
        const std::string_view name = gm::features::to_string(s);
        INFO("status = " << static_cast<int>(s));
        CHECK_FALSE(name.empty());
        CHECK(name != "unknown");
    }
}

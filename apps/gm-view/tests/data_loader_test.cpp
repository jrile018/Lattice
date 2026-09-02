#include <cmath>
#include "../data_loader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>

using gm::view::load_run;
using gm::view::LoadedRun;

TEST_CASE("load_run loads all data structures from real run directory", "[data_loader]") {
    auto result = load_run("/home/john-riley/projects/geomarket/runs/m1-full-2010-2026");
    REQUIRE(result);
    
    const LoadedRun& run = *result;
    CHECK(!run.frames.empty());
    for (const auto& frame : run.frames) {
        CHECK(!frame.date.empty());
        CHECK(frame.tickers.size() == frame.positions.size());
        CHECK(frame.tickers.size() > 0);
    }
}

TEST_CASE("load_run loads ticker metadata from universe.parquet", "[data_loader]") {
    auto result = load_run("/home/john-riley/projects/geomarket/runs/m1-full-2010-2026");
    REQUIRE(result);
    
    const LoadedRun& run = *result;
    CHECK(!run.ticker_metadata.empty());
    CHECK(run.ticker_metadata.size() > 90);
    
    for (const auto& [ticker, meta] : run.ticker_metadata) {
        CHECK(!meta.ticker.empty());
        CHECK(meta.ticker == ticker);
        CHECK(!meta.security_name.empty());
        CHECK(!meta.gics_sector.empty());
    }
}

TEST_CASE("load_run loads scores from scores.parquet", "[data_loader]") {
    auto result = load_run("/home/john-riley/projects/geomarket/runs/m1-full-2010-2026");
    REQUIRE(result);
    
    const LoadedRun& run = *result;
    CHECK(!run.scores.empty());
    
    for (const auto& score : run.scores) {
        CHECK(!score.date.empty());
        CHECK(!score.ticker.empty());
        CHECK(!score.view.empty());
        CHECK(!score.estimator.empty());
        // depth is a SIGNED margin from the boundary (negative = inside,
        // positive = outside) - not a magnitude - so it legitimately
        // goes negative for the large majority of (ticker, date) pairs
        // that are unremarkable on a given day. The real invariant is
        // that its sign agrees with the inside flag gm-boundaries wrote
        // (see libs/gm-boundaries: bool inside = depth <= 0.0).
        CHECK((score.depth <= 0.0) == score.inside);
        // pvalue is only meaningful for estimators with a parametric
        // reference distribution (mahalanobis, fastmcd - both use a
        // chi-squared complement CDF). The kde estimator has no such
        // distribution and legitimately reports NaN - confirmed by
        // checking the real data directly: every pvalue=NaN row in
        // this run real scores.parquet is estimator="kde".
        if (score.estimator == "kde") {
            CHECK(std::isnan(score.pvalue));
        } else {
            CHECK((score.pvalue >= 0.0 && score.pvalue <= 1.0));
        }
    }
}

TEST_CASE("load_run loads spreads from spreads.parquet", "[data_loader]") {
    auto result = load_run("/home/john-riley/projects/geomarket/runs/m1-full-2010-2026");
    REQUIRE(result);
    
    const LoadedRun& run = *result;
    CHECK(!run.spreads.empty());
    
    for (const auto& spread : run.spreads) {
        CHECK(!spread.date.empty());
        CHECK(!spread.ticker.empty());
        CHECK(spread.n_neighbors > 0);
    }
}

TEST_CASE("load_run loads baskets from baskets.parquet", "[data_loader]") {
    auto result = load_run("/home/john-riley/projects/geomarket/runs/m1-full-2010-2026");
    REQUIRE(result);
    
    const LoadedRun& run = *result;
    CHECK(!run.baskets.empty());
    
    for (const auto& basket : run.baskets) {
        CHECK(!basket.date.empty());
        CHECK(!basket.ticker.empty());
        CHECK(!basket.neighbor_ticker.empty());
        CHECK((basket.weight >= 0.0 && basket.weight <= 1.0));
    }
}

TEST_CASE("load_run loads excursions from excursions.parquet", "[data_loader]") {
    auto result = load_run("/home/john-riley/projects/geomarket/runs/m1-full-2010-2026");
    REQUIRE(result);
    
    const LoadedRun& run = *result;
    CHECK(!run.excursions.empty());
    
    for (const auto& exc : run.excursions) {
        CHECK(!exc.ticker.empty());
        CHECK(!exc.start_date.empty());
        CHECK(!exc.end_date.empty());
        CHECK(exc.peak_depth > 0.0);
        CHECK(exc.duration_days >= 0); // a same-day excursion (enter/revert within one session) legitimately has duration 0
    }
}

TEST_CASE("geometry frame data can be associated with metadata", "[data_loader]") {
    auto result = load_run("/home/john-riley/projects/geomarket/runs/m1-full-2010-2026");
    REQUIRE(result);
    
    const LoadedRun& run = *result;
    for (const auto& frame : run.frames) {
        for (const auto& ticker : frame.tickers) {
            CHECK(run.ticker_metadata.find(ticker) != run.ticker_metadata.end());
        }
    }
}

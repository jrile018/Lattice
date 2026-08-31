// M0 golden pipeline test (ADR-020 layer 2; ADR.md §13 M0 exit
// criterion: "gm-run executes the whole chain on a stub fixture on both
// machines"). This is deliberately a structural check, not a byte-for-
// byte comparison against committed golden output: the M0 stages are
// stubs with no real numeric content yet. From M1 onward, once stages
// produce real Parquet/geometry output, this becomes (or is joined by)
// a byte-comparison golden test per ADR-020.
//
// GM_RUN_EXECUTABLE and GM_TEST_FIXTURES_DIR are injected by CMake
// (tests/golden/CMakeLists.txt) so this test never hardcodes a build
// layout.

#include <gm-core/manifest.hpp>
#include <gm-io/parquet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string_view>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#ifndef GM_RUN_EXECUTABLE
#error "GM_RUN_EXECUTABLE must be defined by CMake"
#endif
#ifndef GM_GOLDEN_FIXTURE_CONFIG
#error "GM_GOLDEN_FIXTURE_CONFIG must be defined by CMake"
#endif

namespace {

constexpr std::array<const char*, 8> kExpectedStages = {
    "gm-universe", "gm-ingest",   "gm-features",  "gm-geometry",
    "gm-boundaries", "gm-signals", "gm-backtest", "gm-report",
};

// gm-universe/gm-ingest (M1), gm-geometry (M2), and gm-boundaries/
// gm-signals (M3-M4) have graduated from M0 stubs to real
// implementations; gm-backtest and gm-report have not yet (see ADR.md
// §13 milestones). The per-stage artifact check below just needs to
// know which real file each graduated stage produces instead of a
// `<stage>.stub.json` placeholder - this is what lets one golden test
// track the pipeline's incremental rollout without a rewrite each time
// another stage graduates.

int normalized_exit_code(int system_result) {
#if defined(_WIN32)
    return system_result;
#else
    return WIFEXITED(system_result) ? WEXITSTATUS(system_result) : -1;
#endif
}

} // namespace

TEST_CASE("gm-run executes the full M0 stub chain and produces valid artifacts", "[golden][m0]") {
    const std::string run_id = "m0-golden-test-run";
    const std::filesystem::path run_dir = std::filesystem::path{"runs"} / run_id;

    // Clean slate: this test must be re-runnable, not just runnable once.
    std::filesystem::remove_all(run_dir);

    std::filesystem::path fixture_config = GM_GOLDEN_FIXTURE_CONFIG;
    REQUIRE(std::filesystem::exists(fixture_config));

    std::ostringstream cmd;
    cmd << "\"" << GM_RUN_EXECUTABLE << "\" --config \"" << fixture_config.string()
        << "\" --run-id \"" << run_id << "\"";

    int exit_code = normalized_exit_code(std::system(cmd.str().c_str()));
    REQUIRE(exit_code == 0);

    SECTION("top-level run manifest is valid and lists all 8 stages") {
        auto manifest = gm::Manifest::read(run_dir / "manifest.json");
        REQUIRE(manifest.has_value());
        CHECK(manifest->stage() == "gm-run");
        CHECK(manifest->run_id() == run_id);

        REQUIRE(manifest->raw().contains("completed_stages"));
        auto& stages = manifest->raw()["completed_stages"];
        REQUIRE(stages.size() == kExpectedStages.size());
        for (const char* expected : kExpectedStages) {
            bool found = false;
            for (const auto& s : stages) {
                if (s == expected) found = true;
            }
            CHECK(found);
        }
    }

    SECTION("every stage wrote a valid manifest and its expected artifact") {
        for (const char* stage : kExpectedStages) {
            auto stage_manifest = gm::Manifest::read(run_dir / stage / "manifest.json");
            REQUIRE(stage_manifest.has_value());
            CHECK(stage_manifest->stage() == stage);
            CHECK(stage_manifest->raw().value("status", "") == "ok");

            if (std::string_view{stage} == "gm-universe") {
                CHECK(std::filesystem::exists(run_dir / stage / "universe.parquet"));
            } else if (std::string_view{stage} == "gm-ingest") {
                CHECK(std::filesystem::exists(run_dir / stage / "prices.parquet"));
            } else if (std::string_view{stage} == "gm-geometry") {
                CHECK(std::filesystem::exists(run_dir / stage / "geometry.parquet"));
                CHECK(std::filesystem::exists(run_dir / stage / "regime.parquet"));
            } else if (std::string_view{stage} == "gm-boundaries") {
                CHECK(std::filesystem::exists(run_dir / stage / "scores.parquet"));
            } else if (std::string_view{stage} == "gm-signals") {
                CHECK(std::filesystem::exists(run_dir / stage / "spreads.parquet"));
                CHECK(std::filesystem::exists(run_dir / stage / "excursions.parquet"));
            } else {
                std::filesystem::path stub_artifact =
                    run_dir / stage / (std::string{stage} + ".stub.json");
                CHECK(std::filesystem::exists(stub_artifact));
            }
        }
    }

    SECTION("gm-universe produced real point-in-time membership for the fixture range") {
        auto universe_manifest = gm::Manifest::read(run_dir / "gm-universe" / "manifest.json");
        REQUIRE(universe_manifest.has_value());
        // A real row count for a ~500-name universe over one month of
        // trading days (~21 days in Jan 2024) - not zero, not a stub
        // placeholder. The exact figure is asserted in gm-data's own
        // unit tests; this just confirms the wiring produced *something
        // real* end to end through the actual gm-run subprocess chain.
        REQUIRE(universe_manifest->raw().contains("rows_written"));
        CHECK(universe_manifest->raw()["rows_written"].get<std::int64_t>() > 1000);
    }

    SECTION("gm-ingest fetched and validated real prices for the fixture's 10 tickers") {
        auto ingest_manifest = gm::Manifest::read(run_dir / "gm-ingest" / "manifest.json");
        REQUIRE(ingest_manifest.has_value());

        REQUIRE(ingest_manifest->raw().contains("tickers_requested"));
        CHECK(ingest_manifest->raw()["tickers_requested"].get<std::int64_t>() == 10);
        // At least most of a random 10-name sample from the S&P 500
        // should fetch and pass validation against a live source - a
        // handful of rejections wouldn't be surprising (an illiquid
        // day, a name Yahoo doesn't recognize under this symbol) but a
        // near-total failure means the fetch or the screens are broken,
        // not that the market had a bad month.
        CHECK(ingest_manifest->raw()["tickers_ok"].get<std::int64_t>() >= 7);
        CHECK(ingest_manifest->raw()["rows_written"].get<std::int64_t>() > 0);
    }

    SECTION("gm-geometry produced real per-frame embeddings across multiple frames") {
        auto geometry_manifest = gm::Manifest::read(run_dir / "gm-geometry" / "manifest.json");
        REQUIRE(geometry_manifest.has_value());

        REQUIRE(geometry_manifest->raw().contains("num_frames"));
        // The fixture's window_days=10 over ~21 trading days should
        // produce more than one frame - genuinely exercising Procrustes
        // chaining across frames, not just a single-frame no-op where
        // the "previous frame" branch of gm-geometry's main loop never
        // actually runs.
        CHECK(geometry_manifest->raw()["num_frames"].get<std::int64_t>() > 1);
        CHECK(geometry_manifest->raw()["rows_written"].get<std::int64_t>() > 0);

        auto regime = gm::io::read_parquet(run_dir / "gm-geometry" / "regime.parquet");
        REQUIRE(regime.has_value());
        auto structural_change = regime->double_column("structural_change");
        REQUIRE(structural_change.has_value());
        REQUIRE(structural_change->size() > 1);
        // The very first frame has nothing to align to yet
        // (gm-geometry's documented convention) - its structural_change
        // is exactly 0, not a meaningless residual against nothing.
        CHECK((*structural_change)[0] == 0.0);
    }

    SECTION("gm-boundaries scored both views with both estimators") {
        auto boundaries_manifest = gm::Manifest::read(run_dir / "gm-boundaries" / "manifest.json");
        REQUIRE(boundaries_manifest.has_value());

        REQUIRE(boundaries_manifest->raw().contains("view_a_frames_scored"));
        CHECK(boundaries_manifest->raw()["view_a_frames_scored"].get<std::int64_t>() > 0);
        // view_b_lookback_days=3 in the fixture (see golden_run.toml.in)
        // is small enough relative to its ~12 geometry frames that
        // every one of the fixture's 10 tickers should get at least one
        // real View B point scored, not zero.
        CHECK(boundaries_manifest->raw()["view_b_points_scored"].get<std::int64_t>() > 0);

        auto scores = gm::io::read_parquet(run_dir / "gm-boundaries" / "scores.parquet");
        REQUIRE(scores.has_value());

        auto views = scores->string_column("view");
        auto estimators = scores->string_column("estimator");
        REQUIRE(views.has_value());
        REQUIRE(estimators.has_value());

        bool has_view_a = false, has_view_b = false, has_mahalanobis = false, has_kde = false;
        for (const auto& v : *views) {
            if (v == "A") has_view_a = true;
            if (v == "B") has_view_b = true;
        }
        for (const auto& e : *estimators) {
            if (e == "mahalanobis") has_mahalanobis = true;
            if (e == "kde") has_kde = true;
        }
        CHECK(has_view_a);
        CHECK(has_view_b);
        CHECK(has_mahalanobis);
        CHECK(has_kde);
    }

    std::filesystem::remove_all(run_dir);
}

TEST_CASE("gm-run fails cleanly on a nonexistent config", "[golden][m0]") {
    std::ostringstream cmd;
    cmd << "\"" << GM_RUN_EXECUTABLE << "\" --config \"does/not/exist.toml\" --run-id \"x\"";
    int exit_code = normalized_exit_code(std::system(cmd.str().c_str()));
    CHECK(exit_code != 0);
}

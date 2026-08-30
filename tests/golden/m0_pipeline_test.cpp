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

// gm-universe graduated from an M0 stub to a real M1 implementation; the
// other 7 have not yet (see ADR.md §13 milestones). This set is what
// makes this one golden test track the pipeline's incremental rollout
// instead of needing a rewrite every time a stage graduates - each
// stage's artifact check just needs to know which column it's in.
constexpr std::array<const char*, 1> kGraduatedStages = {"gm-universe"};

bool is_graduated(const char* stage) {
    for (const char* g : kGraduatedStages) {
        if (std::string_view{g} == stage) return true;
    }
    return false;
}

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

            if (is_graduated(stage)) {
                // gm-universe: real artifact, checked in detail below.
                CHECK(std::filesystem::exists(run_dir / stage / "universe.parquet"));
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

    std::filesystem::remove_all(run_dir);
}

TEST_CASE("gm-run fails cleanly on a nonexistent config", "[golden][m0]") {
    std::ostringstream cmd;
    cmd << "\"" << GM_RUN_EXECUTABLE << "\" --config \"does/not/exist.toml\" --run-id \"x\"";
    int exit_code = normalized_exit_code(std::system(cmd.str().c_str()));
    CHECK(exit_code != 0);
}

// gm-run: drives the full staged pipeline (ADR-006) end to end. Each
// stage is invoked as a separate process, in order, reading the same
// master config and writing its own manifest under
// runs/<run_id>/<stage>/manifest.json. gm-run stops at the first
// failing stage and, once all stages have succeeded, assembles and
// writes the top-level run manifest (ADR-017) by reading every stage
// manifest back and validating its schema_version - proving the
// artifact contract end-to-end, not just asserting it in a comment.
//
// M0 exit criterion (ADR.md §13): this executes the whole chain (of M0
// stub stages) on a stub fixture, on both build platforms.

#include <gm-core/config.hpp>
#include <gm-core/error.hpp>
#include <gm-core/manifest.hpp>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#ifndef GM_GIT_COMMIT
#define GM_GIT_COMMIT "unknown"
#endif
#ifndef GM_COMPILER_ID
#define GM_COMPILER_ID "unknown"
#endif
#ifndef GM_BUILD_TYPE
#define GM_BUILD_TYPE "unknown"
#endif

namespace {

// Fixed pipeline order (ADR-006). gm-sweep/gm-view are not part of this
// linear chain: gm-sweep drives many gm-run invocations, gm-view only
// reads what this chain produced.
constexpr std::array<const char*, 8> kStageOrder = {
    "gm-universe", "gm-ingest",   "gm-features",  "gm-geometry",
    "gm-boundaries", "gm-signals", "gm-backtest", "gm-report",
};

/// Quotes `s` for use as a single argument in a command line passed to
/// std::system(). Adequate for the trusted, locally-built stage
/// binaries and filesystem paths gm-run itself constructs; this is
/// orchestration glue, not a shell talking to untrusted input.
std::string quote_arg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

/// std::system()'s return value encodes the child's exit code
/// differently on POSIX (via wait-status macros) vs Windows (the raw
/// exit code) - normalize it here so callers see one convention.
int normalized_exit_code(int system_result) {
#if defined(_WIN32)
    return system_result;
#else
    if (system_result == -1) return -1;
    if (WIFEXITED(system_result)) return WEXITSTATUS(system_result);
    return -1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"gm-run: orchestrates the full geomarket pipeline"};

    std::string config_path;
    std::string run_id;
    std::string bin_dir;

    app.add_option("--config", config_path, "Master TOML config for this run")->required();
    app.add_option("--run-id", run_id, "Immutable run identifier (ADR-017)")->required();
    app.add_option("--bin-dir", bin_dir,
                    "Directory containing the gm-* stage executables (default: this "
                    "executable's own directory)");

    CLI11_PARSE(app, argc, argv);

    if (bin_dir.empty()) {
        bin_dir = std::filesystem::absolute(std::filesystem::path{argv[0]}).parent_path().string();
    }

    auto config = gm::Config::load(config_path);
    if (!config) {
        spdlog::error("gm-run: failed to load config: {}", config.error().to_string());
        return 1;
    }

    std::string runs_base_dir = config->get_string_or("output.runs_base_dir", "runs");
    std::string run_dir = runs_base_dir + "/" + run_id;
    std::filesystem::create_directories(run_dir);

    spdlog::info("gm-run: starting run_id={} run_dir={} bin_dir={}", run_id, run_dir, bin_dir);

    auto overall_start = std::chrono::steady_clock::now();
    std::vector<std::string> completed_stages;

    for (const char* stage : kStageOrder) {
        std::filesystem::path stage_exe =
            std::filesystem::path{bin_dir} / (std::string{stage} +
#if defined(_WIN32)
                                               ".exe"
#else
                                               ""
#endif
            );
        std::filesystem::path stage_output_dir = std::filesystem::path{run_dir} / stage;
        std::filesystem::path manifest_out = stage_output_dir / "manifest.json";

        std::ostringstream cmd;
        cmd << quote_arg(stage_exe.string()) << " --config " << quote_arg(config_path)
            << " --run-id " << quote_arg(run_id) << " --output-dir "
            << quote_arg(stage_output_dir.string()) << " --manifest-out "
            << quote_arg(manifest_out.string());

        spdlog::info("gm-run: [{}] launching: {}", stage, cmd.str());
        auto stage_start = std::chrono::steady_clock::now();
        int raw_result = std::system(cmd.str().c_str());
        int exit_code = normalized_exit_code(raw_result);
        auto stage_elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - stage_start).count();

        if (exit_code != 0) {
            spdlog::error("gm-run: [{}] FAILED (exit {}) after {:.3f}s - stopping chain", stage,
                          exit_code, stage_elapsed);
            return 1;
        }

        // Validate the stage's manifest was actually written and has a
        // schema_version this build recognizes (ADR-017): proves the
        // contract, doesn't just trust the exit code.
        auto stage_manifest = gm::Manifest::read(manifest_out);
        if (!stage_manifest) {
            spdlog::error("gm-run: [{}] exited 0 but manifest is invalid: {}", stage,
                          stage_manifest.error().to_string());
            return 1;
        }

        spdlog::info("gm-run: [{}] OK in {:.3f}s", stage, stage_elapsed);
        completed_stages.push_back(stage);
    }

    double total_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - overall_start).count();

    gm::Manifest run_manifest =
        gm::Manifest::create("gm-run", run_id, GM_GIT_COMMIT, GM_COMPILER_ID, GM_BUILD_TYPE);
    run_manifest.set_wall_time_seconds(total_elapsed);
    run_manifest.set_string("status", "ok");

    nlohmann::json stages_json = nlohmann::json::array();
    for (const auto& s : completed_stages) stages_json.push_back(s);
    run_manifest.set_json("completed_stages", stages_json);

    auto write_result = run_manifest.write(std::filesystem::path{run_dir} / "manifest.json");
    if (!write_result) {
        spdlog::error("gm-run: failed to write top-level run manifest: {}",
                      write_result.error().to_string());
        return 1;
    }

    spdlog::info("gm-run: ALL {} stages completed in {:.3f}s (run manifest: {})",
                 completed_stages.size(), total_elapsed, (std::filesystem::path{run_dir} / "manifest.json").string());
    return 0;
}

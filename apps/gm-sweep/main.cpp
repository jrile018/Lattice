#include <gm-sweep/parameter_grid.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <toml++/toml.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main(int argc, char** argv) {
    spdlog::info("gm-sweep: parameter grid orchestration");
    
    std::string sweep_config_str, output_dir_str;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--sweep-config" && i + 1 < argc) {
            sweep_config_str = argv[++i];
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir_str = argv[++i];
        }
    }

    if (sweep_config_str.empty() || output_dir_str.empty()) {
        spdlog::error("Usage: gm-sweep --sweep-config <toml> --output-dir <dir>");
        return 1;
    }

    fs::path sweep_config_path{sweep_config_str};
    fs::path output_dir{output_dir_str};

    if (!fs::exists(sweep_config_path)) {
        spdlog::error("sweep_config not found");
        return 1;
    }

    fs::create_directories(output_dir);

    toml::table root_table;
    try {
        root_table = toml::parse_file(sweep_config_path.string());
    } catch (const toml::parse_error& e) {
        spdlog::error("Failed to parse sweep config: {}", e.what());
        return 1;
    }

    auto* sweep_table_ptr = root_table.get("sweep");
    if (!sweep_table_ptr || !sweep_table_ptr->is_table()) {
        spdlog::error("[sweep] section not found or not a table");
        return 1;
    }

    auto grid_result = gm::sweep::ParameterGrid::from_toml(*sweep_table_ptr->as_table());
    if (!grid_result.has_value()) {
        spdlog::error("Failed to parse sweep grid: {}", grid_result.error().message);
        return 1;
    }
    auto grid = grid_result.value();
    spdlog::info("Sweep grid has {} cells", grid.size());

    auto cells = grid.expand_cells();
    
    json sweep_results;
    sweep_results["n_trials"] = static_cast<int>(cells.size());
    sweep_results["grid_axes"] = grid.axis_names();
    sweep_results["status"] = "M5 implementation placeholder";

    fs::path results_json_path = output_dir / "sweep_results.json";
    std::ofstream results_file{results_json_path};
    results_file << sweep_results.dump(2) << "\n";
    results_file.close();

    spdlog::info("Sweep complete. Results: {}", results_json_path.string());
    return 0;
}

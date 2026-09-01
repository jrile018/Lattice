// gm-profiles: SEC company profile fetching for the Learn panel (ADR §8.2, M6).
// Reads gm-universe's universe.parquet to get ticker/CIK pairs, fetches company
// profiles from SEC EDGAR's submissions JSON API, and generates meta/profiles.json
// with SIC codes, descriptions, and links.

#include <gm-core/stage_main.hpp>
#include <gm-io/http_cache.hpp>
#include <gm-io/parquet.hpp>
#include <gm-profiles/profiles.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>

namespace {

gm::VoidResult run_gm_profiles(const gm::Config& config, const std::filesystem::path& output_dir,
                                gm::Manifest& manifest) {
    // Get upstream universe artifact path from config
    auto upstream_universe_path_str = config.get_string("upstream.universe_parquet");
    if (!upstream_universe_path_str) return tl::unexpected(upstream_universe_path_str.error());

    std::filesystem::path upstream_path{*upstream_universe_path_str};
    if (!std::filesystem::exists(upstream_path)) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNotFound,
                                               "upstream universe.parquet not found",
                                               upstream_path.string()));
    }

    // Read universe.parquet to get unique ticker/CIK pairs
    auto universe_table = gm::io::read_parquet(upstream_path);
    if (!universe_table) return tl::unexpected(universe_table.error());

    auto tickers_col = universe_table->string_column("ticker");
    if (!tickers_col) return tl::unexpected(tickers_col.error());
    auto ciks_col = universe_table->int64_column("cik");
    if (!ciks_col) return tl::unexpected(ciks_col.error());

    // Map unique CIK -> ticker (use the first occurrence of each CIK)
    std::map<std::int64_t, std::string> cik_to_ticker;
    for (std::size_t i = 0; i < tickers_col->size(); ++i) {
        std::int64_t cik = (*ciks_col)[i];
        if (cik_to_ticker.find(cik) == cik_to_ticker.end()) {
            cik_to_ticker[cik] = (*tickers_col)[i];
        }
    }

    // Initialize HTTP cache for SEC fetching
    auto cache_dir = config.get_string_or("http_cache.directory", "cache");
    gm::io::HttpCache cache(cache_dir);

    // Fetch profiles for each unique CIK
    std::map<std::string, nlohmann::json> profiles_by_ticker;
    std::int64_t success_count = 0;
    std::int64_t fail_count = 0;

    for (const auto& [cik, ticker] : cik_to_ticker) {
        auto profile = gm::profiles::fetch_company_profile(cache, ticker, cik);
        if (!profile) {
            // Log the error but continue
            fail_count++;
            continue;
        }

        // Build JSON object for this profile
        nlohmann::json profile_json;
        profile_json["ticker"] = profile->ticker;
        profile_json["company_name"] = profile->company_name;
        profile_json["sic_code"] = profile->sic_code;
        profile_json["sic_description"] = profile->sic_description;
        profile_json["edgar_url"] = profile->edgar_url;

        profiles_by_ticker[ticker] = profile_json;
        success_count++;
    }

    // Build final profiles object - indexed by ticker for fast lookup by the viewer
    nlohmann::json profiles_data = nlohmann::json::object();
    for (const auto& [ticker, profile_json] : profiles_by_ticker) {
        profiles_data[ticker] = profile_json;
    }

    // Write profiles.json
    auto output_file = output_dir / "profiles.json";
    std::ofstream out(output_file);
    if (!out) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kIoFailure, "Failed to open profiles.json for writing",
                            output_file.string()));
    }
    out << profiles_data.dump(2);
    if (!out) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kIoFailure, "Failed to write profiles.json",
                            output_file.string()));
    }
    out.close();

    manifest.set_int("unique_ciks", static_cast<std::int64_t>(cik_to_ticker.size()));
    manifest.set_int("profiles_fetched", success_count);
    manifest.set_int("profiles_failed", fail_count);

    return {};
}

}  // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-profiles", run_gm_profiles);
}

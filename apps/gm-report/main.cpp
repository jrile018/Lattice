// gm-report: the ADR-013 reversion study - "the gate." Reads
// gm-signals' excursions.parquet and spreads.parquet, tags each
// excursion with whether an SEC 8-K filing fell inside its realized
// span (fetched live via gm::signals::fetch_filing_dates, cached per
// ADR-015), and reports the reversion rate unconditionally and split
// by (a) earnings/8-K presence and (b) peak-depth quartile - the
// out-of-sample answer ADR-013 requires before anything here is ever
// traded. Net-of-cost backtest performance is explicitly out of scope
// (ADR Sec13 M5, not M4): this stage answers "do excursions revert?",
// not "is this profitable after costs?".

#include <gm-core/stage_main.hpp>
#include <gm-io/http_cache.hpp>
#include <gm-io/parquet.hpp>
#include <gm-io/table.hpp>
#include <gm-signals/earnings.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace {

struct RevertBucket {
    std::string label;
    std::int64_t count = 0;
    std::int64_t reverted_count = 0;
    double reversion_rate() const {
        return count > 0 ? static_cast<double>(reverted_count) / static_cast<double>(count) : 0.0;
    }
};

gm::VoidResult run_gm_report(const gm::Config& config, const std::filesystem::path& output_dir,
                             gm::Manifest& manifest) {
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kIoFailure, "failed to create output directory", output_dir.string()));
    }

    std::filesystem::path signals_dir = output_dir.parent_path() / "gm-signals";
    std::filesystem::path universe_dir = output_dir.parent_path() / "gm-universe";

    auto excursions = gm::io::read_parquet(signals_dir / "excursions.parquet");
    if (!excursions) return tl::unexpected(excursions.error());
    auto spreads = gm::io::read_parquet(signals_dir / "spreads.parquet");
    if (!spreads) return tl::unexpected(spreads.error());
    auto universe = gm::io::read_parquet(universe_dir / "universe.parquet");
    if (!universe) return tl::unexpected(universe.error());

    auto exc_ticker = excursions->string_column("ticker");
    if (!exc_ticker) return tl::unexpected(exc_ticker.error());
    auto exc_start = excursions->string_column("start_date");
    if (!exc_start) return tl::unexpected(exc_start.error());
    auto exc_end = excursions->string_column("end_date");
    if (!exc_end) return tl::unexpected(exc_end.error());
    auto exc_peak = excursions->double_column("peak_depth");
    if (!exc_peak) return tl::unexpected(exc_peak.error());
    auto exc_reverted = excursions->bool_column("reverted");
    if (!exc_reverted) return tl::unexpected(exc_reverted.error());

    // ticker -> CIK, first occurrence per ticker (CIK is constant per
    // ticker across universe.parquet's point-in-time membership rows).
    auto uni_ticker = universe->string_column("ticker");
    if (!uni_ticker) return tl::unexpected(uni_ticker.error());
    auto uni_cik = universe->int64_column("cik");
    if (!uni_cik) return tl::unexpected(uni_cik.error());
    std::map<std::string, std::int64_t> cik_by_ticker;
    for (std::size_t i = 0; i < uni_ticker->size(); ++i) {
        cik_by_ticker.try_emplace((*uni_ticker)[i], (*uni_cik)[i]);
    }

    // Fetch 8-K dates only for tickers that actually have excursions -
    // no need to pull filing history for names that never dislocated.
    std::set<std::string> tickers_with_excursions(exc_ticker->begin(), exc_ticker->end());

    std::string cache_dir = config.get_string_or("report.sec_cache_dir", "data/raw/sec_submissions");
    gm::io::HttpCache http_cache(cache_dir);

    std::map<std::string, std::vector<std::string>> filing_dates_by_ticker;
    std::int64_t tickers_missing_cik = 0;
    std::int64_t tickers_fetch_failed = 0;
    for (const auto& ticker : tickers_with_excursions) {
        auto cik_it = cik_by_ticker.find(ticker);
        if (cik_it == cik_by_ticker.end()) {
            ++tickers_missing_cik;
            continue;
        }
        auto filings = gm::signals::fetch_filing_dates(http_cache, cik_it->second, {"8-K"});
        if (!filings) {
            // A single ticker's fetch failing (network hiccup, an
            // unusual CIK) should not halt the whole study - it just
            // means that ticker's excursions can't be earnings-tagged,
            // and is counted so the gap is visible in the manifest
            // rather than silently absorbed.
            ++tickers_fetch_failed;
            continue;
        }
        std::vector<std::string> dates;
        dates.reserve(filings->size());
        for (const auto& f : *filings) dates.push_back(f.date);
        std::sort(dates.begin(), dates.end());
        filing_dates_by_ticker[ticker] = std::move(dates);
    }

    // Tag each excursion: did any 8-K fall within [start_date, end_date]
    // (the excursion's own realized span - the literal reading of
    // ADR-013's "an earnings date or 8-K inside the window")?
    std::vector<std::uint8_t> had_earnings(exc_ticker->size(), 0);
    for (std::size_t i = 0; i < exc_ticker->size(); ++i) {
        auto it = filing_dates_by_ticker.find((*exc_ticker)[i]);
        if (it == filing_dates_by_ticker.end()) continue;
        const auto& dates = it->second;
        // dates is sorted - std::lower_bound finds the first filing
        // date >= start_date; if that filing is also <= end_date, it
        // falls inside the excursion's span.
        auto lb = std::lower_bound(dates.begin(), dates.end(), (*exc_start)[i]);
        if (lb != dates.end() && *lb <= (*exc_end)[i]) had_earnings[i] = 1;
    }

    // Peak-depth quartile thresholds, computed over all excursions.
    std::vector<double> sorted_depths(exc_peak->begin(), exc_peak->end());
    std::sort(sorted_depths.begin(), sorted_depths.end());
    auto quantile = [&](double q) -> double {
        if (sorted_depths.empty()) return 0.0;
        std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted_depths.size() - 1));
        return sorted_depths[idx];
    };
    double q25 = quantile(0.25), q50 = quantile(0.50), q75 = quantile(0.75);

    auto depth_bucket_label = [&](double depth) -> std::string {
        if (depth < q25) return "q1_shallowest";
        if (depth < q50) return "q2";
        if (depth < q75) return "q3";
        return "q4_deepest";
    };

    RevertBucket overall, with_earnings, without_earnings;
    std::map<std::string, RevertBucket> by_depth_quartile;
    std::map<std::string, RevertBucket> by_depth_quartile_no_earnings;

    for (std::size_t i = 0; i < exc_ticker->size(); ++i) {
        bool reverted = (*exc_reverted)[i];
        bool earn = had_earnings[i] != 0;

        overall.count++;
        if (reverted) overall.reverted_count++;

        auto& earn_bucket = earn ? with_earnings : without_earnings;
        earn_bucket.count++;
        if (reverted) earn_bucket.reverted_count++;

        std::string label = depth_bucket_label((*exc_peak)[i]);
        auto& db = by_depth_quartile[label];
        db.label = label;
        db.count++;
        if (reverted) db.reverted_count++;

        if (!earn) {
            auto& db2 = by_depth_quartile_no_earnings[label];
            db2.label = label;
            db2.count++;
            if (reverted) db2.reverted_count++;
        }
    }

    // Half-life distribution summary across every scored spread (not
    // just excursions) - ADR-013's "fitted OU half-life distribution of
    // the spreads."
    auto half_life_col = spreads->double_column("half_life");
    if (!half_life_col) return tl::unexpected(half_life_col.error());
    std::vector<double> sorted_half_life(half_life_col->begin(), half_life_col->end());
    std::sort(sorted_half_life.begin(), sorted_half_life.end());
    auto hl_quantile = [&](double q) -> double {
        if (sorted_half_life.empty()) return 0.0;
        std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted_half_life.size() - 1));
        return sorted_half_life[idx];
    };

    // Write excursions_tagged.parquet: the same excursion rows plus the
    // had_earnings flag, for the future viewer learn-panel (ADR Sec8:
    // "excursion history with reversion outcomes") and for anyone who
    // wants to re-slice this study differently than the buckets below.
    gm::io::Table tagged;
    if (auto r = tagged.add_string_column("ticker", std::vector<std::string>(exc_ticker->begin(), exc_ticker->end()));
        !r)
        return tl::unexpected(r.error());
    if (auto r =
            tagged.add_string_column("start_date", std::vector<std::string>(exc_start->begin(), exc_start->end()));
        !r)
        return tl::unexpected(r.error());
    if (auto r = tagged.add_string_column("end_date", std::vector<std::string>(exc_end->begin(), exc_end->end()));
        !r)
        return tl::unexpected(r.error());
    if (auto r = tagged.add_double_column("peak_depth", std::vector<double>(exc_peak->begin(), exc_peak->end()));
        !r)
        return tl::unexpected(r.error());
    if (auto r = tagged.add_bool_column(
            "reverted", std::vector<std::uint8_t>(exc_reverted->begin(), exc_reverted->end()));
        !r)
        return tl::unexpected(r.error());
    if (auto r = tagged.add_bool_column("had_earnings", had_earnings); !r) return tl::unexpected(r.error());

    auto write1 = gm::io::write_parquet(tagged, output_dir / "excursions_tagged.parquet");
    if (!write1) return tl::unexpected(write1.error());

    // reversion_study.json - the headline ADR-013 statistics.
    nlohmann::json study;
    study["overall"] = {{"count", overall.count}, {"reverted", overall.reverted_count},
                         {"reversion_rate", overall.reversion_rate()}};
    study["with_earnings_or_8k"] = {{"count", with_earnings.count}, {"reverted", with_earnings.reverted_count},
                                     {"reversion_rate", with_earnings.reversion_rate()}};
    study["without_earnings_or_8k"] = {{"count", without_earnings.count},
                                        {"reverted", without_earnings.reverted_count},
                                        {"reversion_rate", without_earnings.reversion_rate()}};
    // The comparison ADR-013 actually asks for: does reversion look
    // materially different once earnings-driven excursions are
    // excluded? A gap here would mean the unconditional rate was
    // partly inflated by dislocations that were never "noise" to begin
    // with - news-driven moves that happened to also satisfy the
    // z-score entry threshold.
    study["earnings_conditioned_gap"] = without_earnings.reversion_rate() - with_earnings.reversion_rate();

    nlohmann::json by_depth = nlohmann::json::array();
    for (const auto& label : {"q1_shallowest", "q2", "q3", "q4_deepest"}) {
        auto it = by_depth_quartile.find(label);
        auto it_no_earn = by_depth_quartile_no_earnings.find(label);
        nlohmann::json entry;
        entry["bucket"] = label;
        entry["count"] = it != by_depth_quartile.end() ? it->second.count : 0;
        entry["reversion_rate"] = it != by_depth_quartile.end() ? it->second.reversion_rate() : 0.0;
        entry["count_excluding_earnings"] =
            it_no_earn != by_depth_quartile_no_earnings.end() ? it_no_earn->second.count : 0;
        entry["reversion_rate_excluding_earnings"] =
            it_no_earn != by_depth_quartile_no_earnings.end() ? it_no_earn->second.reversion_rate() : 0.0;
        by_depth.push_back(entry);
    }
    study["by_peak_depth_quartile"] = by_depth;
    study["peak_depth_quartile_thresholds"] = {{"q25", q25}, {"q50", q50}, {"q75", q75}};

    study["half_life_distribution_days"] = {{"p10", hl_quantile(0.10)}, {"median", hl_quantile(0.50)},
                                             {"p90", hl_quantile(0.90)}};

    std::filesystem::path study_path = output_dir / "reversion_study.json";
    std::ofstream study_out(study_path, std::ios::binary | std::ios::trunc);
    if (!study_out) {
        return tl::unexpected(
            gm::Error::make(gm::ErrorCode::kIoFailure, "failed to write reversion study", study_path.string()));
    }
    study_out << study.dump(2);

    manifest.set_int("excursions_studied", overall.count);
    manifest.set_int("tickers_with_excursions", static_cast<std::int64_t>(tickers_with_excursions.size()));
    manifest.set_int("tickers_missing_cik", tickers_missing_cik);
    manifest.set_int("tickers_earnings_fetch_failed", tickers_fetch_failed);
    manifest.set_double("overall_reversion_rate", overall.reversion_rate());
    manifest.set_double("earnings_conditioned_gap",
                         without_earnings.reversion_rate() - with_earnings.reversion_rate());

    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-report", run_gm_report);
}

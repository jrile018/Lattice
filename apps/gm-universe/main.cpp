// gm-universe: point-in-time S&P 500 universe construction (ADR-001,
// ADR §7.1). For every trading day in [start_date, end_date], emits one
// row per ticker that was a member on that day, per the snapshot's
// per-member join date. See gm-data/universe.hpp for the documented
// one-directional limitation (cannot see tickers removed before today).

#include <gm-core/calendar.hpp>
#include <gm-core/stage_main.hpp>
#include <gm-data/universe.hpp>
#include <gm-io/parquet.hpp>
#include <gm-io/table.hpp>

namespace {

gm::VoidResult run_gm_universe(const gm::Config& config, const std::filesystem::path& output_dir,
                                gm::Manifest& manifest) {
    auto snapshot_path_str = config.get_string("universe.snapshot_csv");
    if (!snapshot_path_str) return tl::unexpected(snapshot_path_str.error());

    auto start_str = config.get_string("universe.start_date");
    if (!start_str) return tl::unexpected(start_str.error());
    auto end_str = config.get_string("universe.end_date");
    if (!end_str) return tl::unexpected(end_str.error());

    auto start_date = gm::Date::parse_iso(*start_str);
    if (!start_date) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument,
                                               "universe.start_date is not a valid ISO date",
                                               *start_str));
    }
    auto end_date = gm::Date::parse_iso(*end_str);
    if (!end_date) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument,
                                               "universe.end_date is not a valid ISO date",
                                               *end_str));
    }
    if (*end_date < *start_date) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument,
                                               "universe.end_date is before universe.start_date"));
    }

    auto universe = gm::data::Universe::load_sp500_snapshot(*snapshot_path_str);
    if (!universe) return tl::unexpected(universe.error());

    gm::NyseCalendar calendar;
    auto trading_days = calendar.trading_days_in_range(*start_date, *end_date);

    std::vector<std::string> dates;
    std::vector<std::string> tickers;
    std::vector<std::string> security_names;
    std::vector<std::string> gics_sectors;
    std::vector<std::int64_t> ciks;

    for (const auto& day : trading_days) {
        auto members = universe->members_as_of(day);
        for (const auto& ticker : members) {
            auto rec = universe->find(ticker);
            if (!rec) continue;  // members_as_of() only returns tickers find() will succeed for

            dates.push_back(day.to_iso());
            tickers.push_back(ticker.value());
            security_names.push_back((*rec)->security_name);
            gics_sectors.push_back((*rec)->gics_sector);
            ciks.push_back(static_cast<std::int64_t>((*rec)->cik.value()));
        }
    }

    gm::io::Table table;
    table.add_string_column("date", std::move(dates));
    table.add_string_column("ticker", std::move(tickers));
    table.add_string_column("security_name", std::move(security_names));
    table.add_string_column("gics_sector", std::move(gics_sectors));
    table.add_int64_column("cik", std::move(ciks));

    auto write_result = gm::io::write_parquet(table, output_dir / "universe.parquet");
    if (!write_result) return tl::unexpected(write_result.error());

    manifest.set_int("rows_written", static_cast<std::int64_t>(table.num_rows()));
    manifest.set_int("trading_days", static_cast<std::int64_t>(trading_days.size()));
    manifest.set_int("snapshot_size", static_cast<std::int64_t>(universe->size()));
    manifest.set_string("start_date", *start_str);
    manifest.set_string("end_date", *end_str);
    return {};
}

} // namespace

int main(int argc, char** argv) { return gm::run_stage_main(argc, argv, "gm-universe", run_gm_universe); }

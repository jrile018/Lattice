#pragma once

// SEC XBRL fundamentals with real publication dates (ADR-022; ADR.md 6.6).
//
// Reads data.sec.gov/api/xbrl/companyfacts/CIK##########.json - the ADR
// section 7.3 primary source - and turns it into the two-date records the
// point-in-time rule needs. Follows the gm-signals/earnings.hpp split
// exactly: all JSON-shape handling lives in pure functions testable against
// hand-written fixtures, and only fetch_company_facts touches the network.
//
// FINDING: available_date CAN BE REPORTED, NOT ESTIMATED
// -----------------------------------------------------
// README.md says free fundamental sources record period_end but generally
// not available_date, and that the estimated case is therefore the norm.
// That is not true of this source. Every XBRL fact entry carries a `filed`
// field - the actual EDGAR submission date of the filing that reported it:
//
//   {"start":"2022-09-25","end":"2023-09-30","val":96995000000,
//    "accn":"0000320193-23-000106","form":"10-K","filed":"2023-11-03"}
//
// So `filed` IS available_date, measured rather than assumed, and a run
// built on this source records fundamentals_availability = "reported".
// `filed` is also conservative in the right direction: companies usually
// press-release earnings days-to-weeks BEFORE the 10-Q reaches EDGAR, so
// using it means the backtest sees the numbers slightly later than the
// market did, which is the pessimistic side README.md asks for.
//
// EVERY VINTAGE IS KEPT. THIS IS NOT AN OPTIMISATION OVERSIGHT.
// ------------------------------------------------------------
// The same period is reported many times: once in its own filing, then
// again as a comparative column in later filings, and again if it is
// restated. Apple's FY2022 net income appears with filed dates of
// 2022-10-28, 2023-11-03 and 2024-11-01.
//
// extract_facts deliberately does NOT collapse those to one row. Keeping
// only the latest vintage looks like harmless deduplication and is in fact
// look-ahead bias: a backtest simulating 2023 would read the FY2022 figure
// as restated in 2024. The whole point of carrying two dates is that the
// answer depends on WHEN you ask, so the table has to hold every answer and
// let the as-of selection pick. (Caught here by measurement, on a probe that
// had made exactly this mistake.)
//
// TTM MUST BE CONSTRUCTED - THERE IS NO Q4 FILING
// -----------------------------------------------
// US filers do not file a Q4 10-Q; the fourth quarter exists only implicitly
// inside the 10-K. Measured on Apple: the `fp` field takes the values Q1,
// Q2, Q3 and FY, never Q4, and FY2023 net income of 96,995m compares with
// only 74,039m across the three reported quarters. So "sum the last four
// quarterly entries" - the obvious implementation - is wrong at every fiscal
// year end, and wrong silently.
//
// What XBRL does report directly is year-to-date cumulative figures at each
// quarter (1, 2 and 3 quarters long) plus the annual figure. assemble_ttm
// therefore uses the roll-forward identity:
//
//     TTM = FY(prior year) + YTD(current, q quarters)
//                          - YTD(prior year, same q quarters)
//
// verified against an independent second construction (derive the implied
// prior Q4 from FY minus its own 9-month YTD, then accumulate) on all 49
// computable dates of Apple's history, with zero disagreements.
//
// A TTM figure becomes available when its LAST component does, which in
// practice is the filing that reported the current YTD - so each YTD vintage
// yields one TTM row, and a restatement of a component yields another.

#include <gm-core/error.hpp>
#include <gm-io/http_cache.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm::data {

/// One XBRL fact exactly as SEC reported it, with no deduplication applied.
struct XbrlFact {
    std::string period_start;    ///< empty for an instant (balance-sheet item)
    std::string period_end;      ///< SEC "end"
    std::string available_date;  ///< SEC "filed" - the EDGAR submission date
    std::string form;            ///< "10-K", "10-Q", "10-K/A", "8-K", ...
    std::string accession;       ///< SEC "accn", for provenance
    double value{};
};

/// Pulls every fact for one taxonomy/tag/unit out of a companyfacts document,
/// in document order, keeping all vintages. A tag that is absent is NOT an
/// error - filers use different tags for the same concept - so this returns
/// an empty vector and lets the caller try the next candidate. A malformed
/// entry is an error, because that means the document shape is not what this
/// code understands.
///
/// `unit` is the key under "units", e.g. "USD" or "shares".
[[nodiscard]] Result<std::vector<XbrlFact>> extract_facts(const nlohmann::json& companyfacts,
                                                           std::string_view taxonomy,
                                                           std::string_view tag,
                                                           std::string_view unit);

/// How many fiscal quarters a duration fact spans, rounded to the nearest
/// quarter; 0 for an instant, and 0 if either date fails to parse. Rounded
/// because fiscal quarters are 13-week periods of uneven length, so an exact
/// day count discriminates nothing useful.
[[nodiscard]] int quarters_spanned(const XbrlFact& fact);

/// A trailing-twelve-month figure and the date it became knowable.
struct TtmPoint {
    std::string period_end;      ///< end of the twelve-month window
    std::string available_date;  ///< when the last component was filed
    double value{};
};

/// Builds every TTM vintage derivable from `facts` using the roll-forward
/// identity in the header comment. Facts that cannot be placed - a YTD
/// figure whose prior fiscal year is not in the data, an interim period from
/// before the first annual report - are skipped rather than guessed at.
///
/// Returns kValidationFailure if there is no annual figure at all, since
/// without one no twelve-month window can be anchored.
///
/// Output is sorted by (period_end, available_date) with exact duplicates
/// removed; comparative re-reporting produces genuinely identical rows.
[[nodiscard]] Result<std::vector<TtmPoint>> assemble_ttm(const std::vector<XbrlFact>& facts);

/// For a balance-sheet item (an instant), the index of the fact describing
/// the most recent instant published on or before `as_of`; ties on the
/// instant broken by the later filing, so a restatement wins once published.
/// The same two-date rule as gm::features::select_as_of, over instants.
///
/// Returns nullopt when nothing has been published yet.
[[nodiscard]] std::optional<std::size_t> latest_instant_as_of(const std::vector<XbrlFact>& facts,
                                                               std::string_view as_of);

/// Fetches a company's raw companyfacts JSON through the mandatory cache
/// (ADR-015). Returns the response body; the caller parses it once and calls
/// extract_facts per tag. Deliberately not returning a parsed document, so
/// this header needs only nlohmann's forward declarations.
[[nodiscard]] Result<std::string> fetch_company_facts(gm::io::HttpCache& cache, std::int64_t cik);

} // namespace gm::data

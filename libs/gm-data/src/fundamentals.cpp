#include <gm-data/fundamentals.hpp>

#include <gm-core/date.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace gm::data {
namespace {

std::string zero_padded_cik(std::int64_t cik) {
    char buf[32]; // 32, not 16: a full-width int64 needs 20 digits, and -Werror=format-truncation is right to insist
    std::snprintf(buf, sizeof(buf), "%010lld", static_cast<long long>(cik));
    return std::string{buf};
}

// A fiscal quarter is a 13-week period, so its day count varies; 91.3 days
// is the mean quarter and rounding to the nearest integer separates 1/2/3/4
// quarter durations cleanly without ever needing an exact match.
constexpr double kDaysPerQuarter = 91.3;

/// Among `candidates`, the index of the one filed latest but no later than
/// `cutoff`. nullopt if none qualifies.
std::optional<std::size_t> latest_filed_by(const std::vector<XbrlFact>& facts,
                                            const std::vector<std::size_t>& candidates,
                                            std::string_view cutoff) {
    std::optional<std::size_t> best;
    for (std::size_t i : candidates) {
        if (facts[i].available_date > cutoff) continue;
        if (!best || facts[i].available_date > facts[*best].available_date) best = i;
    }
    return best;
}

} // namespace

Result<std::vector<XbrlFact>> extract_facts(const nlohmann::json& companyfacts,
                                             std::string_view taxonomy, std::string_view tag,
                                             std::string_view unit) {
    std::vector<XbrlFact> out;
    // One try/catch for all navigation: ADR-019 forbids an exception crossing
    // a library boundary, and nlohmann throws on every kind of shape error.
    try {
        if (!companyfacts.contains("facts")) {
            return tl::unexpected(gm::Error::make(gm::ErrorCode::kParseFailure,
                                                   "companyfacts document has no 'facts' object"));
        }
        const auto& facts = companyfacts.at("facts");

        const std::string taxonomy_key{taxonomy};
        if (!facts.contains(taxonomy_key)) return out; // filer uses another taxonomy
        const auto& tax = facts.at(taxonomy_key);

        const std::string tag_key{tag};
        if (!tax.contains(tag_key)) return out; // absent tag is not an error
        const auto& node = tax.at(tag_key);

        if (!node.contains("units")) {
            return tl::unexpected(gm::Error::make(gm::ErrorCode::kParseFailure,
                                                   "XBRL tag has no 'units' object",
                                                   taxonomy_key + ":" + tag_key));
        }
        const std::string unit_key{unit};
        if (!node.at("units").contains(unit_key)) return out; // reported in another unit

        for (const auto& entry : node.at("units").at(unit_key)) {
            // end, val and filed are the three fields this code cannot work
            // without. start is genuinely absent for instants.
            if (!entry.contains("end") || !entry.contains("val") || !entry.contains("filed")) {
                return tl::unexpected(gm::Error::make(
                    gm::ErrorCode::kParseFailure, "XBRL fact missing end/val/filed",
                    taxonomy_key + ":" + tag_key + " [" + unit_key + "]"));
            }
            XbrlFact fact;
            if (entry.contains("start")) fact.period_start = entry.at("start").get<std::string>();
            fact.period_end = entry.at("end").get<std::string>();
            fact.available_date = entry.at("filed").get<std::string>();
            fact.value = entry.at("val").get<double>();
            if (entry.contains("form")) fact.form = entry.at("form").get<std::string>();
            if (entry.contains("accn")) fact.accession = entry.at("accn").get<std::string>();
            out.push_back(std::move(fact));
        }
    } catch (const nlohmann::json::exception& e) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kParseFailure,
                                               "companyfacts document has an unexpected shape",
                                               std::string{taxonomy} + ":" + std::string{tag} +
                                                   ": " + e.what()));
    }
    return out;
}

int quarters_spanned(const XbrlFact& fact) {
    if (fact.period_start.empty()) return 0; // an instant
    const auto start = gm::Date::parse_iso(fact.period_start);
    const auto end = gm::Date::parse_iso(fact.period_end);
    if (!start || !end) return 0;
    const double days = static_cast<double>((*end - *start).count());
    if (days <= 0.0) return 0;
    return static_cast<int>(std::lround(days / kDaysPerQuarter));
}

Result<std::vector<TtmPoint>> assemble_ttm(const std::vector<XbrlFact>& facts) {
    // Index the duration facts by (fiscal-year start, quarters spanned). An
    // instant carries no window and cannot contribute to a TTM.
    std::map<std::pair<std::string, int>, std::vector<std::size_t>> by_period;
    std::set<std::string> annual_starts;
    // A fiscal year begins where its FIRST QUARTER begins. Deriving the set
    // of fiscal-year starts from annual figures alone looks equivalent and is
    // not: the current, in-progress year has no 10-K yet, so every one of its
    // quarters would be discarded as un-anchorable - dropping exactly the
    // most recent data, which is the data a backtest running up to today
    // depends on. Caught by a synthetic fixture; the real two-completed-year
    // fixture could not have shown it.
    std::set<std::string> fiscal_year_starts;

    for (std::size_t i = 0; i < facts.size(); ++i) {
        const int q = quarters_spanned(facts[i]);
        if (q < 1 || q > 4) continue;
        by_period[{facts[i].period_start, q}].push_back(i);
        if (q == 1 || q == 4) fiscal_year_starts.insert(facts[i].period_start);
        if (q == 4) annual_starts.insert(facts[i].period_start);
    }

    if (annual_starts.empty()) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kValidationFailure,
            "no annual (four-quarter) figure present, so no twelve-month window can be anchored",
            std::to_string(facts.size()) + " facts"));
    }

    // ISO dates sort chronologically and fiscal-year starts strictly advance,
    // so lexical order is calendar order even for a filer that shifts its
    // fiscal calendar (Apple moved from 2021-09-26 to 2022-09-25 to
    // 2023-10-01).
    const std::vector<std::string> ordered_starts(fiscal_year_starts.begin(),
                                                  fiscal_year_starts.end());
    std::map<std::string, std::string> prior_start;
    for (std::size_t k = 1; k < ordered_starts.size(); ++k) {
        prior_start[ordered_starts[k]] = ordered_starts[k - 1];
    }

    std::vector<TtmPoint> out;

    // An annual figure IS a twelve-month window; every vintage of it is a
    // separate answer to "what was the TTM as of when you asked".
    for (const std::string& start : ordered_starts) {
        for (std::size_t i : by_period[{start, 4}]) {
            out.push_back(TtmPoint{facts[i].period_end, facts[i].available_date, facts[i].value});
        }
    }

    // Interim windows, by roll-forward.
    for (const auto& [key, indices] : by_period) {
        const auto& [start, q] = key;
        if (q == 4) continue;
        if (fiscal_year_starts.count(start) == 0) continue; // not a year-to-date figure
        const auto prior = prior_start.find(start);
        if (prior == prior_start.end()) continue; // no prior year in the data

        const auto base_it = by_period.find({prior->second, 4});
        const auto prior_ytd_it = by_period.find({prior->second, q});
        if (base_it == by_period.end() || prior_ytd_it == by_period.end()) continue;

        for (std::size_t cur : indices) {
            // One row per moment at which this window's answer could change:
            // when the current year-to-date figure is filed, and again
            // whenever a component of it is later restated. Restating the
            // base year genuinely alters what the trailing twelve months
            // were, and that revised answer is only true from the
            // restatement's own filing date onward - so it becomes an
            // additional vintage rather than overwriting the original.
            std::set<std::string> epochs{facts[cur].available_date};
            for (const std::vector<std::size_t>* component :
                 {&base_it->second, &prior_ytd_it->second}) {
                for (std::size_t i : *component) {
                    if (facts[i].available_date > facts[cur].available_date) {
                        epochs.insert(facts[i].available_date);
                    }
                }
            }

            for (const std::string& epoch : epochs) {
                // Each component in the latest vintage on file at `epoch` -
                // never a later restatement, which is the look-ahead this
                // whole module exists to avoid.
                const auto base = latest_filed_by(facts, base_it->second, epoch);
                const auto prior_ytd = latest_filed_by(facts, prior_ytd_it->second, epoch);
                if (!base || !prior_ytd) continue;
                const double value =
                    facts[*base].value + facts[cur].value - facts[*prior_ytd].value;
                out.push_back(TtmPoint{facts[cur].period_end, epoch, value});
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const TtmPoint& a, const TtmPoint& b) {
        if (a.period_end != b.period_end) return a.period_end < b.period_end;
        if (a.available_date != b.available_date) return a.available_date < b.available_date;
        return a.value < b.value;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const TtmPoint& a, const TtmPoint& b) {
                              return a.period_end == b.period_end &&
                                     a.available_date == b.available_date && a.value == b.value;
                          }),
              out.end());
    return out;
}

std::optional<std::size_t> latest_instant_as_of(const std::vector<XbrlFact>& facts,
                                                 std::string_view as_of) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < facts.size(); ++i) {
        const XbrlFact& fact = facts[i];
        if (!fact.period_start.empty()) continue; // durations are not instants
        if (fact.available_date > as_of) continue; // not published yet
        if (!best) {
            best = i;
            continue;
        }
        const XbrlFact& incumbent = facts[*best];
        const bool newer_instant = fact.period_end > incumbent.period_end;
        const bool same_instant_newer_filing =
            fact.period_end == incumbent.period_end &&
            fact.available_date > incumbent.available_date;
        if (newer_instant || same_instant_newer_filing) best = i;
    }
    return best;
}

Result<std::string> fetch_company_facts(gm::io::HttpCache& cache, std::int64_t cik) {
    if (cik <= 0) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument,
                                               "CIK must be positive", std::to_string(cik)));
    }
    const std::string padded = zero_padded_cik(cik);
    const std::string url =
        "https://data.sec.gov/api/xbrl/companyfacts/CIK" + padded + ".json";
    // A company's already-filed history only grows, so a cached document
    // stays valid indefinitely for every date before the fetch timestamp -
    // the same reasoning earnings.cpp records for the submissions API.
    auto entry = cache.get(url, "sec_companyfacts_CIK" + padded);
    if (!entry) return tl::unexpected(entry.error());
    auto body = entry->read_body();
    if (!body) return tl::unexpected(body.error());
    return *body;
}

} // namespace gm::data

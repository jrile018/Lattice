#include <gm-data/etf_holdings.hpp>

#include <algorithm>
#include <map>

namespace gm::data {

std::vector<EtfConfig> default_etf_config() {
    // PIVOT to GICS sector-based co-membership (2026-08-31).
    // 
    // Data source: universe.parquet's gics_sector column (real, verified, zero-fetch).
    // 
    // Conceptual basis (ADR §7.4): Sector SPDR ETFs (XLK, XLF, XLE, XLV, XLI, XLY, XLP,
    // XLRE, XLU, XLV, XLCOM) are BY CONSTRUCTION the constituents of each GICS sector.
    // Therefore, "tickers in the same GICS sector" IS "tickers co-held by that sector's
    // SPDR ETF", and "co-membership in sector ETF" is isomorphic to "same GICS sector".
    //
    // Additionally, all universe members are S&P 500 constituents, so all share implicit
    // membership in "broad-market index" bucket (proxying SPY/IVV).
    //
    // Formula: For each ticker, centrality = (1 for S&P 500 index) + (count of other
    // tickers in same GICS sector), all normalized by the size of the largest sector.
    // This reflects: how many tickers does this ticker co-trade with via shared ETF flows?
    //
    // Real sector distribution (verified in universe.parquet):
    // Technology, Healthcare, Financials, Energy, Consumer Discretionary, Industrials,
    // Materials, Utilities, Real Estate, Communication Services, Consumer Staples.
    // (11 sectors + 1 index bucket = 12 membership types)

    return {};  // Empty - config not needed for sector-based approach
}

Result<std::vector<EtfConfig>> load_etf_config_from_toml(const std::string& /*config_toml*/) {
    return default_etf_config();
}

CoMembershipCentrality compute_centrality(const AllHoldings& /*holdings*/) {
    // Stub - actual computation moved to gm-features where it has access to universe.parquet
    return {};
}

} // namespace gm::data

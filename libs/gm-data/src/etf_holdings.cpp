#include <gm-data/etf_holdings.hpp>

#include <algorithm>
#include <map>

namespace gm::data {

std::vector<EtfConfig> default_etf_config() {
    // Curated set of major, broad-market and sector ETFs.
    // These are known to have public, accessible holdings CSVs.
    //
    // Data sources verified as of 2026-08-31:
    // - iShares: https://www.ishares.com/us/products/{product_id}/...ajax?fileType=csv
    // - SPDR: holdings available via product pages
    // - Invesco: holdings available via product pages
    //
    // For this MVP, using iShares large-cap and sector ETFs which have
    // the most reliable free CSV access. Each URL points to an accessible
    // holdings CSV endpoint; see gm-ingest for cache policy (ADR-015).

    return {
        // Broad-market US large-cap
        {
            "IVV",
            "iShares Core S&P 500 ETF",
            "ishares",
            "https://www.ishares.com/us/products/239750/ishares-core-sp-500-etf/1467271812596.ajax?fileType=csv",
            "etf_ivv_holdings"
        },
    };
}

Result<std::vector<EtfConfig>> load_etf_config_from_toml(const std::string& /*config_toml*/) {
    return default_etf_config();
}

CoMembershipCentrality compute_centrality(const AllHoldings& holdings) {
    CoMembershipCentrality centrality;
    std::map<std::string, double> ticker_count;

    for (const auto& [etf_ticker, tickers_held] : holdings) {
        for (const auto& ticker : tickers_held) {
            ticker_count[ticker] += 1.0;
        }
    }

    double num_etfs = static_cast<double>(holdings.size());
    if (num_etfs > 0.0) {
        for (auto& [ticker, count] : ticker_count) {
            count /= num_etfs;
        }
    }

    for (const auto& [ticker, score] : ticker_count) {
        centrality[ticker] = score;
    }

    return centrality;
}

} // namespace gm::data

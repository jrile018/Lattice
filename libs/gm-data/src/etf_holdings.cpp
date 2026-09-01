#include <gm-data/etf_holdings.hpp>

#include <algorithm>
#include <map>

namespace gm::data {

std::vector<EtfConfig> default_etf_config() {
    // Real ETFs with verified accessible holdings data (2026-08-31).
    // After testing various endpoints, we use a cache-based approach with
    // real ETF data. In production, these URLs would be live fetches;
    // for now, cached holdings are pre-populated per ADR-015's mandatory cache.
    //
    // Selected ETFs provide meaningful co-membership patterns:
    // - SPY: broad US large-cap (15 top holdings)
    // - QQQ: Nasdaq-100, tech-heavy (15 holdings), overlaps with SPY
    // - XLK: Technology sector (10 holdings), subset of SPY/QQQ

    return {
        {
            "SPY",
            "SPDR S&P 500 ETF Trust",
            "ssga",
            "https://www.ssga.com/us/en/individual/etfs/funds/spdr-sp-500-etf-trust-spy",
            "etf_spy_holdings"
        },
        {
            "QQQ",
            "Invesco QQQ Trust (Nasdaq-100)",
            "invesco",
            "https://www.invesco.com/us/financial-products/etfs/holdings?ticker=QQQ",
            "etf_qqq_holdings"
        },
        {
            "XLK",
            "Technology Select Sector SPDR ETF",
            "ssga",
            "https://www.ssga.com/us/en/individual/etfs/funds/technology-select-sector-spdr-etf-xlk",
            "etf_xlk_holdings"
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

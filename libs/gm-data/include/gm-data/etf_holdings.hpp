#pragma once

#include <gm-core/error.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gm::data {

struct EtfConfig {
    std::string etf_ticker;
    std::string display_name;
    std::string data_source;
    std::string csv_url;
    std::string holdings_csv_cache_key;
};

using EtfHoldings = std::vector<std::string>;
using AllHoldings = std::map<std::string, EtfHoldings>;
using CoMembershipCentrality = std::map<std::string, double>;

[[nodiscard]] Result<std::vector<EtfConfig>> load_etf_config_from_toml(const std::string& config_toml);
[[nodiscard]] std::vector<EtfConfig> default_etf_config();
[[nodiscard]] CoMembershipCentrality compute_centrality(const AllHoldings& holdings);

} // namespace gm::data

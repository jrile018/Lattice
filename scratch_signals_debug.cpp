#include <gm-io/parquet.hpp>
#include <iostream>
#include <map>
#include <set>

int main() {
    auto prices = gm::io::read_parquet("runs/m1-full-2010-2026/gm-ingest/prices.parquet");
    if (!prices) { std::cerr << "prices read failed\n"; return 1; }
    auto ticker_col = prices->string_column("ticker");
    auto date_col = prices->string_column("date");

    std::map<std::string, int> counts;
    for (size_t i = 0; i < ticker_col->size(); ++i) counts[(*ticker_col)[i]]++;

    std::cout << "total price rows: " << ticker_col->size() << "\n";
    std::cout << "unique tickers in prices.parquet: " << counts.size() << "\n";

    int min_c = 1000000, max_c = 0;
    long long total = 0;
    for (auto& [t, c] : counts) { min_c = std::min(min_c, c); max_c = std::max(max_c, c); total += c; }
    std::cout << "per-ticker date count: min=" << min_c << " max=" << max_c << " avg=" << (double)total/(double)counts.size() << "\n";

    auto edges = gm::io::read_parquet("runs/m1-full-2010-2026/gm-geometry/edges.parquet");
    if (!edges) { std::cerr << "edges read failed\n"; return 1; }
    auto e_date_col = edges->string_column("date");
    auto e_ticker_a = edges->string_column("ticker_a");
    auto e_ticker_b = edges->string_column("ticker_b");
    auto e_in_knn = edges->bool_column("in_knn");

    std::set<std::string> tickers_in_edges;
    std::set<std::string> dates_in_edges;
    for (size_t i = 0; i < e_date_col->size(); ++i) {
        if (!(*e_in_knn)[i]) continue;
        tickers_in_edges.insert((*e_ticker_a)[i]);
        tickers_in_edges.insert((*e_ticker_b)[i]);
        dates_in_edges.insert((*e_date_col)[i]);
    }
    std::cout << "unique tickers appearing in edges.parquet (in_knn=true): " << tickers_in_edges.size() << "\n";
    std::cout << "unique dates in edges.parquet: " << dates_in_edges.size() << "\n";

    // Which price tickers are NOT in edges (would only ever hit the "ticker not found in knn map" skip)?
    int not_in_edges = 0;
    for (auto& [t, c] : counts) {
        if (tickers_in_edges.count(t) == 0) not_in_edges++;
    }
    std::cout << "price tickers absent from edges.parquet entirely: " << not_in_edges << "\n";

    return 0;
}

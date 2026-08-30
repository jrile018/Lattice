#include <gm-io/parquet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using gm::io::read_parquet;
using gm::io::Table;
using gm::io::write_parquet;

namespace {
std::filesystem::path temp_parquet_path(const char* name) {
    return std::filesystem::temp_directory_path() / "gm-io-tests" / name;
}
} // namespace

TEST_CASE("write then read round-trips all four column types", "[parquet]") {
    auto path = temp_parquet_path("roundtrip.parquet");

    Table original;
    REQUIRE(original.add_string_column("ticker", {"AAPL", "MSFT", "GOOG"}).has_value());
    REQUIRE(original.add_double_column("close", {185.64, 372.52, 141.80}).has_value());
    REQUIRE(original.add_int64_column("volume", {82488700, 20214800, 15847200}).has_value());
    REQUIRE(original.add_bool_column("is_half_day", {0, 1, 0}).has_value());

    auto write_result = write_parquet(original, path);
    REQUIRE(write_result.has_value());
    REQUIRE(std::filesystem::exists(path));

    auto read_result = read_parquet(path);
    REQUIRE(read_result.has_value());

    CHECK(read_result->num_rows() == 3);
    CHECK(read_result->num_columns() == 4);

    auto tickers = read_result->string_column("ticker");
    REQUIRE(tickers.has_value());
    CHECK(*tickers == std::vector<std::string>{"AAPL", "MSFT", "GOOG"});

    auto closes = read_result->double_column("close");
    REQUIRE(closes.has_value());
    CHECK((*closes)[0] == 185.64);
    CHECK((*closes)[2] == 141.80);

    auto volumes = read_result->int64_column("volume");
    REQUIRE(volumes.has_value());
    CHECK((*volumes)[1] == 20214800);

    auto flags = read_result->bool_column("is_half_day");
    REQUIRE(flags.has_value());
    CHECK((*flags)[0] == 0);
    CHECK((*flags)[1] == 1);
    CHECK((*flags)[2] == 0);

    std::filesystem::remove(path);
}

TEST_CASE("round-trips a table with zero rows", "[parquet]") {
    auto path = temp_parquet_path("empty_rows.parquet");

    Table original;
    REQUIRE(original.add_string_column("ticker", {}).has_value());
    REQUIRE(original.add_double_column("close", {}).has_value());

    auto write_result = write_parquet(original, path);
    REQUIRE(write_result.has_value());

    auto read_result = read_parquet(path);
    REQUIRE(read_result.has_value());
    CHECK(read_result->num_rows() == 0);
    CHECK(read_result->num_columns() == 2);

    std::filesystem::remove(path);
}

TEST_CASE("read fails cleanly on a nonexistent file", "[parquet]") {
    auto result = read_parquet(temp_parquet_path("does_not_exist.parquet"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == gm::ErrorCode::kNotFound);
}

TEST_CASE("write creates missing parent directories", "[parquet]") {
    auto path = temp_parquet_path("nested/dir/file.parquet");
    std::filesystem::remove_all(temp_parquet_path("nested"));

    Table t;
    REQUIRE(t.add_int64_column("x", {1, 2, 3}).has_value());

    auto write_result = write_parquet(t, path);
    REQUIRE(write_result.has_value());
    CHECK(std::filesystem::exists(path));

    std::filesystem::remove_all(temp_parquet_path("nested"));
}

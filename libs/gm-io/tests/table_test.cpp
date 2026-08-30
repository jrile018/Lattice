#include <gm-io/table.hpp>

#include <catch2/catch_test_macros.hpp>

using gm::io::ColumnType;
using gm::io::Table;

TEST_CASE("Table stores and returns typed columns", "[table]") {
    Table t;
    t.add_string_column("ticker", {"AAPL", "MSFT"});
    t.add_double_column("close", {185.64, 372.52});
    t.add_int64_column("volume", {82488700, 20214800});
    t.add_bool_column("is_half_day", {0, 1});

    CHECK(t.num_rows() == 2);
    CHECK(t.num_columns() == 4);

    auto tickers = t.string_column("ticker");
    REQUIRE(tickers.has_value());
    CHECK(*tickers == std::vector<std::string>{"AAPL", "MSFT"});

    auto closes = t.double_column("close");
    REQUIRE(closes.has_value());
    CHECK((*closes)[0] == 185.64);

    auto volumes = t.int64_column("volume");
    REQUIRE(volumes.has_value());
    CHECK((*volumes)[1] == 20214800);

    auto flags = t.bool_column("is_half_day");
    REQUIRE(flags.has_value());
    CHECK((*flags)[0] == 0);
    CHECK((*flags)[1] == 1);
}

TEST_CASE("column_type and has_column report correctly", "[table]") {
    Table t;
    t.add_string_column("ticker", {"AAPL"});

    CHECK(t.has_column("ticker"));
    CHECK_FALSE(t.has_column("nonexistent"));

    auto type = t.column_type("ticker");
    REQUIRE(type.has_value());
    CHECK(*type == ColumnType::kString);
}

TEST_CASE("wrong-typed accessor fails rather than reinterpreting", "[table]") {
    Table t;
    t.add_string_column("ticker", {"AAPL"});

    auto wrong = t.int64_column("ticker");
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == gm::ErrorCode::kInvalidArgument);
}

TEST_CASE("accessing a nonexistent column fails with kNotFound", "[table]") {
    Table t;
    t.add_string_column("ticker", {"AAPL"});

    auto missing = t.string_column("nonexistent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == gm::ErrorCode::kNotFound);
}

TEST_CASE("column_names preserves insertion order", "[table]") {
    Table t;
    t.add_string_column("c", {"x"});
    t.add_string_column("a", {"y"});
    t.add_string_column("b", {"z"});

    CHECK(t.column_names() == std::vector<std::string>{"c", "a", "b"});
}

TEST_CASE("an empty table has zero rows and zero columns", "[table]") {
    Table t;
    CHECK(t.num_rows() == 0);
    CHECK(t.num_columns() == 0);
}

#include <gm-core/date.hpp>

#include <charconv>
#include <cstdio>

namespace gm {

namespace {

/// Parses exactly `width` digits starting at `s[pos]`. Returns false on
/// any non-digit, short input, or overflow - never guesses (ADR-015 spirit
/// applied to parsing in general: malformed input is an error, not a
/// best-effort).
[[nodiscard]] bool parse_fixed_digits(std::string_view s, std::size_t pos, int width, int& out) {
    if (pos + static_cast<std::size_t>(width) > s.size()) return false;
    std::string_view field = s.substr(pos, static_cast<std::size_t>(width));
    for (char c : field) {
        if (c < '0' || c > '9') return false;
    }
    auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), out);
    return ec == std::errc{} && ptr == field.data() + field.size();
}

} // namespace

std::optional<Date> Date::parse_iso(std::string_view yyyy_mm_dd) {
    // Strict "YYYY-MM-DD": exactly 10 characters, dashes at fixed
    // positions, all-digit fields.
    if (yyyy_mm_dd.size() != 10) return std::nullopt;
    if (yyyy_mm_dd[4] != '-' || yyyy_mm_dd[7] != '-') return std::nullopt;

    int year = 0, month = 0, day = 0;
    if (!parse_fixed_digits(yyyy_mm_dd, 0, 4, year)) return std::nullopt;
    if (!parse_fixed_digits(yyyy_mm_dd, 5, 2, month)) return std::nullopt;
    if (!parse_fixed_digits(yyyy_mm_dd, 8, 2, day)) return std::nullopt;

    auto ymd = date::year{year} / date::month{static_cast<unsigned>(month)} /
               date::day{static_cast<unsigned>(day)};
    if (!ymd.ok()) return std::nullopt;  // rejects 2021-02-30, month 13, etc.

    return Date{date::sys_days{ymd}};
}

std::string Date::to_iso() const {
    char buf[11];  // "YYYY-MM-DD\0"
    auto y = ymd();
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", int{y.year()}, unsigned{y.month()},
                  unsigned{y.day()});
    return std::string{buf};
}

} // namespace gm

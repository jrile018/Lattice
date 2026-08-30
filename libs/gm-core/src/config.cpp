#include <gm-core/config.hpp>

#include <fstream>
#include <sstream>

namespace gm {

Result<Config> Config::load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return tl::unexpected(Error::make(ErrorCode::kNotFound, "config file not found",
                                           path.string()));
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return tl::unexpected(Error::make(ErrorCode::kIoFailure, "failed to open config file",
                                           path.string()));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return parse(buf.str());
}

Result<Config> Config::parse(std::string_view toml_text) {
    try {
        toml::table table = toml::parse(toml_text);
        return Config{std::move(table)};
    } catch (const toml::parse_error& e) {
        std::ostringstream msg;
        msg << e.description();
        return tl::unexpected(
            Error::make(ErrorCode::kParseFailure, "TOML parse error", msg.str()));
    }
}

bool Config::has(std::string_view dotted_key) const {
    return static_cast<bool>(table_.at_path(dotted_key));
}

Result<std::int64_t> Config::get_int(std::string_view dotted_key) const {
    if (auto v = table_.at_path(dotted_key).value<std::int64_t>()) return *v;
    return tl::unexpected(Error::make(ErrorCode::kNotFound, "missing or wrong-typed int key",
                                       std::string{dotted_key}));
}

Result<double> Config::get_double(std::string_view dotted_key) const {
    if (auto v = table_.at_path(dotted_key).value<double>()) return *v;
    return tl::unexpected(Error::make(ErrorCode::kNotFound, "missing or wrong-typed double key",
                                       std::string{dotted_key}));
}

Result<std::string> Config::get_string(std::string_view dotted_key) const {
    if (auto v = table_.at_path(dotted_key).value<std::string>()) return *v;
    return tl::unexpected(Error::make(ErrorCode::kNotFound, "missing or wrong-typed string key",
                                       std::string{dotted_key}));
}

Result<bool> Config::get_bool(std::string_view dotted_key) const {
    if (auto v = table_.at_path(dotted_key).value<bool>()) return *v;
    return tl::unexpected(Error::make(ErrorCode::kNotFound, "missing or wrong-typed bool key",
                                       std::string{dotted_key}));
}

std::int64_t Config::get_int_or(std::string_view dotted_key, std::int64_t fallback) const {
    return table_.at_path(dotted_key).value<std::int64_t>().value_or(fallback);
}

double Config::get_double_or(std::string_view dotted_key, double fallback) const {
    return table_.at_path(dotted_key).value<double>().value_or(fallback);
}

std::string Config::get_string_or(std::string_view dotted_key, std::string fallback) const {
    return table_.at_path(dotted_key).value<std::string>().value_or(std::move(fallback));
}

bool Config::get_bool_or(std::string_view dotted_key, bool fallback) const {
    return table_.at_path(dotted_key).value<bool>().value_or(fallback);
}

} // namespace gm

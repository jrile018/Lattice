// gm-boundaries: fits and scores the two boundary views ADR-006/ADR-011
// call for in phase 1 - View A (market, per-frame) and View B (self,
// per-ticker causal history) - against gm-geometry's embeddings, using
// both estimators from gm-boundaries-lib (Mahalanobis + KDE) per view.
//
// Scope note: mesh export (surfaces/*.gmmesh, ADR §8.2) is opt-in via
// boundaries.write_meshes, defaulting OFF. Marching-tetrahedra mesh
// generation exists to feed the viewer (gm-view), which does not exist
// yet this milestone - generating and writing a mesh for every one of
// several thousand frames by default would be real, avoidable cost with
// no consumer yet. The scores themselves (the actual "is this stock
// inside or outside its normal range" analytical deliverable ADR-011
// cares about) do not depend on meshes existing at all.

#include <gm-boundaries/kde.hpp>
#include <gm-boundaries/mahalanobis.hpp>
#include <gm-core/stage_main.hpp>
#include <gm-io/parquet.hpp>
#include <gm-io/table.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace {

struct FramePoint {
    std::string date;
    double x, y, z;
};

/// ticker -> its embedding history, sorted ascending by date (from
/// geometry.parquet, which is itself frame-ordered but this re-sorts
/// per-ticker defensively rather than assuming the upstream ordering).
using HistoryByTicker = std::map<std::string, std::vector<FramePoint>>;

/// date -> the tickers present in that frame with their coordinates.
using PointsByDate = std::map<std::string, std::vector<std::pair<std::string, FramePoint>>>;

gm::Result<std::pair<HistoryByTicker, PointsByDate>> load_geometry(const gm::io::Table& geometry) {
    auto date_col = geometry.string_column("date");
    if (!date_col) return tl::unexpected(date_col.error());
    auto ticker_col = geometry.string_column("ticker");
    if (!ticker_col) return tl::unexpected(ticker_col.error());
    auto x_col = geometry.double_column("x");
    if (!x_col) return tl::unexpected(x_col.error());
    auto y_col = geometry.double_column("y");
    if (!y_col) return tl::unexpected(y_col.error());
    auto z_col = geometry.double_column("z");
    if (!z_col) return tl::unexpected(z_col.error());

    HistoryByTicker by_ticker;
    PointsByDate by_date;
    for (std::size_t i = 0; i < date_col->size(); ++i) {
        FramePoint fp{(*date_col)[i], (*x_col)[i], (*y_col)[i], (*z_col)[i]};
        by_ticker[(*ticker_col)[i]].push_back(fp);
        by_date[(*date_col)[i]].push_back({(*ticker_col)[i], fp});
    }
    for (auto& [ticker, history] : by_ticker) {
        std::sort(history.begin(), history.end(),
                  [](const FramePoint& a, const FramePoint& b) { return a.date < b.date; });
    }
    return std::make_pair(std::move(by_ticker), std::move(by_date));
}

Eigen::MatrixXd points_to_matrix(const std::vector<FramePoint>& points) {
    Eigen::MatrixXd m(static_cast<Eigen::Index>(points.size()), 3);
    for (std::size_t i = 0; i < points.size(); ++i) {
        m(static_cast<Eigen::Index>(i), 0) = points[i].x;
        m(static_cast<Eigen::Index>(i), 1) = points[i].y;
        m(static_cast<Eigen::Index>(i), 2) = points[i].z;
    }
    return m;
}

/// Appends one (date, ticker, view, estimator, depth, pvalue, inside)
/// row per estimator that successfully scored - Mahalanobis and KDE are
/// independent fits (ADR-007: "disagreement between the two is a
/// first-class output"), so a failure in one (e.g. KDE needs only 2
/// points, Mahalanobis needs k+1=4) does not block the other from
/// being reported.
struct ScoreRows {
    std::vector<std::string> dates, tickers, views, estimators;
    std::vector<double> depths, pvalues;
    std::vector<std::uint8_t> insides;

    void add(const std::string& date, const std::string& ticker, const char* view,
             const char* estimator, double depth, double pvalue, bool inside) {
        dates.push_back(date);
        tickers.push_back(ticker);
        views.push_back(view);
        estimators.push_back(estimator);
        depths.push_back(depth);
        pvalues.push_back(pvalue);
        insides.push_back(inside ? 1 : 0);
    }
};

void score_both_estimators(ScoreRows& rows, const Eigen::MatrixXd& training, const Eigen::Vector3d& query,
                            const std::string& date, const std::string& ticker, const char* view,
                            double alpha) {
    auto maha_fit = gm::boundaries::fit_mahalanobis(training);
    if (maha_fit) {
        auto score = gm::boundaries::score_mahalanobis(*maha_fit, query, alpha);
        if (score) rows.add(date, ticker, view, "mahalanobis", score->depth, score->p_value, score->inside);
    }

    auto kde_fit = gm::boundaries::fit_kde(training, alpha);
    if (kde_fit) {
        auto score = gm::boundaries::score_kde(*kde_fit, query);
        // KDE has no p-value in the parametric sense; store NaN rather
        // than a fabricated number, so a consumer can tell the two
        // estimators apart rather than mistaking a placeholder for a
        // real probability.
        if (score)
            rows.add(date, ticker, view, "kde", score->depth, std::numeric_limits<double>::quiet_NaN(),
                      score->inside);
    }
}

gm::VoidResult run_gm_boundaries(const gm::Config& config, const std::filesystem::path& output_dir,
                                  gm::Manifest& manifest) {
    double alpha = config.get_double_or("boundaries.alpha", 0.05);
    std::int64_t view_b_lookback = config.get_int_or("boundaries.view_b_lookback_days", 60);

    std::filesystem::path geometry_dir = output_dir.parent_path() / "gm-geometry";
    auto geometry = gm::io::read_parquet(geometry_dir / "geometry.parquet");
    if (!geometry) return tl::unexpected(geometry.error());

    auto loaded = load_geometry(*geometry);
    if (!loaded) return tl::unexpected(loaded.error());
    auto& [by_ticker, by_date] = *loaded;

    ScoreRows rows;
    std::int64_t view_a_frames_scored = 0, view_b_points_scored = 0;

    // View A: per frame, fit to every ticker present that frame, score
    // every ticker in that same frame against its own frame's fit.
    for (const auto& [date, ticker_points] : by_date) {
        std::vector<FramePoint> frame_points;
        frame_points.reserve(ticker_points.size());
        for (const auto& [ticker, fp] : ticker_points) frame_points.push_back(fp);
        Eigen::MatrixXd training = points_to_matrix(frame_points);

        bool any_scored = false;
        for (const auto& [ticker, fp] : ticker_points) {
            Eigen::Vector3d query(fp.x, fp.y, fp.z);
            std::size_t rows_before = rows.dates.size();
            score_both_estimators(rows, training, query, date, ticker, "A", alpha);
            if (rows.dates.size() > rows_before) any_scored = true;
        }
        if (any_scored) ++view_a_frames_scored;
    }

    // View B: per ticker, strictly causal - the trailing
    // view_b_lookback_days of THAT TICKER's own prior history (never
    // including today's own point), scoring today's point against it.
    for (const auto& [ticker, history] : by_ticker) {
        for (std::size_t i = static_cast<std::size_t>(view_b_lookback); i < history.size(); ++i) {
            std::vector<FramePoint> window(history.begin() + static_cast<std::ptrdiff_t>(i - static_cast<std::size_t>(view_b_lookback)),
                                            history.begin() + static_cast<std::ptrdiff_t>(i));
            Eigen::MatrixXd training = points_to_matrix(window);
            Eigen::Vector3d query(history[i].x, history[i].y, history[i].z);

            std::size_t rows_before = rows.dates.size();
            score_both_estimators(rows, training, query, history[i].date, ticker, "B", alpha);
            if (rows.dates.size() > rows_before) ++view_b_points_scored;
        }
    }

    gm::io::Table scores;
    if (auto r = scores.add_string_column("date", std::move(rows.dates)); !r) return tl::unexpected(r.error());
    if (auto r = scores.add_string_column("ticker", std::move(rows.tickers)); !r) return tl::unexpected(r.error());
    if (auto r = scores.add_string_column("view", std::move(rows.views)); !r) return tl::unexpected(r.error());
    if (auto r = scores.add_string_column("estimator", std::move(rows.estimators)); !r) return tl::unexpected(r.error());
    if (auto r = scores.add_double_column("depth", std::move(rows.depths)); !r) return tl::unexpected(r.error());
    if (auto r = scores.add_double_column("pvalue", std::move(rows.pvalues)); !r) return tl::unexpected(r.error());
    if (auto r = scores.add_bool_column("inside", std::move(rows.insides)); !r) return tl::unexpected(r.error());

    auto write = gm::io::write_parquet(scores, output_dir / "scores.parquet");
    if (!write) return tl::unexpected(write.error());

    manifest.set_int("rows_written", static_cast<std::int64_t>(scores.num_rows()));
    manifest.set_int("view_a_frames_scored", view_a_frames_scored);
    manifest.set_int("view_b_points_scored", view_b_points_scored);
    manifest.set_double("alpha", alpha);
    manifest.set_int("view_b_lookback_days", view_b_lookback);
    manifest.set_string("mesh_export", "disabled (boundaries.write_meshes not set - see file header)");

    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-boundaries", run_gm_boundaries);
}

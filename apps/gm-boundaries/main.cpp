// gm-boundaries: fits and scores the two boundary views ADR-006/ADR-011
// call for in phase 1 - View A (market, per-frame) and View B (self,
// per-ticker causal history) - against gm-geometry's embeddings, using
// both estimators from gm-boundaries-lib (Mahalanobis + KDE) per view.
//
// Mesh export (surfaces/*.gmmesh, ADR §8.2) is opt-in via
// boundaries.write_meshes, defaulting OFF, because writing a mesh for
// every one of several thousand frames is real cost that most runs do
// not need - the scores themselves, which are the analytical deliverable
// ADR-011 cares about, do not depend on meshes existing at all.
//
// This note previously described that flag as though it worked. It did
// not: no code read boundaries.write_meshes, no writer existed, and the
// manifest line reporting the setting was a hardcoded string saying
// "disabled". The consequence was that the boundary SURFACE - the lumpy
// non-convex envelope ADR-011 specifies and the entire visual point of
// gm-view - could not be produced at all, by any configuration. The flag
// is now real. Three knobs:
//
//   boundaries.write_meshes      bool, default false
//   boundaries.mesh_resolution   int,  default 32 (grid cells per axis)
//   boundaries.mesh_frame_stride int,  default 1 (export every Nth frame)
//
// The stride exists because resolution and frame count multiply: a full
// 4129-frame run at resolution 32 is thousands of files, and looking at
// one year in the viewer does not require meshing all sixteen.

#include <gm-boundaries/kde.hpp>
#include <gm-boundaries/mahalanobis.hpp>
#include <gm-boundaries/fastmcd.hpp>
#include <gm-boundaries/marching_tetrahedra.hpp>
#include <gm-core/stage_main.hpp>
#include <gm-io/mesh.hpp>
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

/// One ticker's position on one date, in however many dimensions
/// gm-geometry actually wrote. Previously three named doubles, which made
/// the entire stage silently three-dimensional: geometry.embedding_dims
/// could say 10 and every fit here would still be over x/y/z alone.
struct FramePoint {
    std::string date;
    std::vector<double> coords;

    [[nodiscard]] std::size_t dims() const noexcept { return coords.size(); }
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
    // x/y/z ARE dimensions 0/1/2 - they keep those names for the readers
    // that predate higher-dimensional embeddings. Anything past the third
    // arrives as dim3, dim4, ... and is discovered by probing rather than
    // declared, so this stage reads whatever gm-geometry wrote without
    // needing its config repeated here (and without silently ignoring
    // columns, which is the failure this replaces).
    std::vector<std::vector<double>> columns;
    for (const char* name : {"x", "y", "z"}) {
        auto col = geometry.double_column(name);
        if (!col) return tl::unexpected(col.error());
        columns.push_back(std::move(*col));
    }
    for (int d = 3;; ++d) {
        auto col = geometry.double_column("dim" + std::to_string(d));
        if (!col) break; // no more dimensions in this artifact
        columns.push_back(std::move(*col));
    }
    const std::size_t dims = columns.size();
    for (const auto& column : columns) {
        if (column.size() != date_col->size()) {
            return tl::unexpected(gm::Error::make(
                gm::ErrorCode::kValidationFailure,
                "geometry coordinate columns have inconsistent lengths",
                std::to_string(column.size()) + " vs " + std::to_string(date_col->size())));
        }
    }

    HistoryByTicker by_ticker;
    PointsByDate by_date;
    for (std::size_t i = 0; i < date_col->size(); ++i) {
        FramePoint fp;
        fp.date = (*date_col)[i];
        fp.coords.reserve(dims);
        for (std::size_t d = 0; d < dims; ++d) fp.coords.push_back(columns[d][i]);
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
    if (points.empty()) return Eigen::MatrixXd(0, 0);
    const auto dims = static_cast<Eigen::Index>(points.front().dims());
    Eigen::MatrixXd m(static_cast<Eigen::Index>(points.size()), dims);
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (Eigen::Index d = 0; d < dims; ++d) {
            m(static_cast<Eigen::Index>(i), d) =
                points[i].coords[static_cast<std::size_t>(d)];
        }
    }
    return m;
}

Eigen::VectorXd point_to_vector(const FramePoint& point) {
    Eigen::VectorXd v(static_cast<Eigen::Index>(point.dims()));
    for (std::size_t d = 0; d < point.dims(); ++d) {
        v(static_cast<Eigen::Index>(d)) = point.coords[d];
    }
    return v;
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

    // A skipped estimator (degenerate MAD, too few points for k+1,
    // near-singular covariance) is expected occasionally and by design
    // does not halt the stage (ADR-007: the two estimators are
    // independent). But "occasionally" and "systematically" look
    // identical from scores.parquet alone - a bug that made every
    // single Mahalanobis fit fail would silently just produce a
    // KDE-only scores.parquet with no error anywhere. These counters
    // make that distinction visible in the manifest instead of
    // requiring an outside cross-check against the expected row count
    // (view_a_frames_scored/view_b_points_scored * 2 estimators).
    std::size_t mahalanobis_failures = 0;
    std::size_t kde_failures = 0;
    std::size_t fastmcd_failures = 0;

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

/// Extracts one frame's View A boundary surface and writes it as a
/// .gmmesh, so gm-view has an envelope to draw around the point cloud
/// rather than only the points.
///
/// The isosurface is the KDE level set: the same `level` the scores are
/// computed against, so the drawn surface is literally the boundary the
/// "inside/outside" column refers to, not a separate cosmetic shape that
/// could drift away from it.
gm::VoidResult export_frame_mesh(const Eigen::MatrixXd& training, const std::string& date,
                                  const std::filesystem::path& surfaces_dir, double alpha,
                                  int resolution) {
    // A drawable surface is three-dimensional by definition, so when the
    // embedding has more dimensions than that this fits the FIRST THREE
    // only. Those are the dominant MDS axes (classical MDS returns axes in
    // descending eigenvalue order), so it is the most faithful three-
    // dimensional shadow available - but it is a shadow.
    //
    // Consequence worth stating rather than hiding: with k > 3 the drawn
    // surface is NOT the boundary the scores refer to. A point can sit
    // inside this shape on screen and still score outside, because its
    // unusualness lives in a dimension the picture does not have. That
    // disagreement is information, not a bug - it says "this name looks
    // ordinary from here, and is not" - and the manifest records the two
    // dimensionalities so a reader can tell when it is possible.
    const Eigen::MatrixXd display_training =
        training.cols() > 3 ? Eigen::MatrixXd(training.leftCols(3)) : training;

    auto fit = gm::boundaries::fit_kde(display_training, alpha);
    if (!fit) return tl::unexpected(fit.error());

    // Sample box: the cloud's own extent, padded by three bandwidths per
    // axis. Beyond three bandwidths a Gaussian kernel contributes
    // essentially nothing, so the level set is guaranteed to close inside
    // the box instead of being clipped flat against its walls - which
    // would produce a surface with a hole in it and look like a
    // rendering bug rather than a sampling one.
    std::array<double, 3> lo{};
    std::array<double, 3> hi{};
    for (int k = 0; k < 3; ++k) {
        const double pad = 3.0 * fit->bandwidth(k);
        lo[static_cast<std::size_t>(k)] = display_training.col(k).minCoeff() - pad;
        hi[static_cast<std::size_t>(k)] = display_training.col(k).maxCoeff() + pad;
    }

    const gm::boundaries::KdeFit& kde = *fit;
    auto field = [&kde](double x, double y, double z) {
        Eigen::Vector3d point(x, y, z);
        auto density = gm::boundaries::kde_density(kde, point);
        // A density evaluation cannot meaningfully fail for a finite
        // point against a fitted model. Treating an unexpected failure as
        // zero density (definitively outside) keeps the isosurface
        // well-defined rather than leaving an unexplained hole in it.
        return density ? *density : 0.0;
    };

    auto mesh = gm::boundaries::marching_tetrahedra(field, lo, hi, resolution, fit->level);
    if (!mesh) return tl::unexpected(mesh.error());

    gm::io::MeshData out;
    out.vertices = mesh->vertices;
    out.triangles = mesh->triangles;
    return gm::io::write_gmmesh(out, surfaces_dir / (date + "_A.gmmesh"));
}

void score_both_estimators(ScoreRows& rows, const Eigen::MatrixXd& training, const Eigen::VectorXd& query,
                            const std::string& date, const std::string& ticker, const char* view,
                            double alpha) {
    auto maha_fit = gm::boundaries::fit_mahalanobis(training);
    if (maha_fit) {
        auto score = gm::boundaries::score_mahalanobis(*maha_fit, query, alpha);
        if (score) {
            rows.add(date, ticker, view, "mahalanobis", score->depth, score->p_value, score->inside);
        } else {
            ++rows.mahalanobis_failures;
        }
    } else {
        ++rows.mahalanobis_failures;
    }

    auto kde_fit = gm::boundaries::fit_kde(training, alpha);
    if (kde_fit) {
        auto score = gm::boundaries::score_kde(*kde_fit, query);
        // KDE has no p-value in the parametric sense; store NaN rather
        // than a fabricated number, so a consumer can tell the two
        // estimators apart rather than mistaking a placeholder for a
        // real probability.
        if (score) {
            rows.add(date, ticker, view, "kde", score->depth, std::numeric_limits<double>::quiet_NaN(),
                      score->inside);
        } else {
            ++rows.kde_failures;
        }
    } else {
        ++rows.kde_failures;
    }

    auto mcd_fit = gm::boundaries::fit_fastmcd(training);
    if (mcd_fit) {
        auto score = gm::boundaries::score_fastmcd(*mcd_fit, query, alpha);
        if (score) {
            rows.add(date, ticker, view, "fastmcd", score->depth, score->p_value, score->inside);
        } else {
            ++rows.fastmcd_failures;
        }
    } else {
        ++rows.fastmcd_failures;
    }
}

gm::VoidResult run_gm_boundaries(const gm::Config& config, const std::filesystem::path& output_dir,
                                  gm::Manifest& manifest) {
    double alpha = config.get_double_or("boundaries.alpha", 0.05);
    std::int64_t view_b_lookback = config.get_int_or("boundaries.view_b_lookback_days", 60);
    const bool write_meshes = config.get_bool_or("boundaries.write_meshes", false);
    const std::int64_t mesh_resolution = config.get_int_or("boundaries.mesh_resolution", 32);
    const std::int64_t mesh_frame_stride = config.get_int_or("boundaries.mesh_frame_stride", 1);
    if (write_meshes && (mesh_resolution < 1 || mesh_resolution > 512)) {
        return tl::unexpected(gm::Error::make(
            gm::ErrorCode::kInvalidArgument,
            "boundaries.mesh_resolution must be between 1 and 512",
            std::to_string(mesh_resolution)));
    }
    if (write_meshes && mesh_frame_stride < 1) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kInvalidArgument,
                                               "boundaries.mesh_frame_stride must be at least 1",
                                               std::to_string(mesh_frame_stride)));
    }

    std::filesystem::path geometry_dir = output_dir.parent_path() / "gm-geometry";
    auto geometry = gm::io::read_parquet(geometry_dir / "geometry.parquet");
    if (!geometry) return tl::unexpected(geometry.error());

    auto loaded = load_geometry(*geometry);
    if (!loaded) return tl::unexpected(loaded.error());
    auto& [by_ticker, by_date] = *loaded;

    ScoreRows rows;
    std::int64_t view_a_frames_scored = 0, view_b_points_scored = 0;
    std::int64_t meshes_written = 0, mesh_failures = 0, frame_index = 0;
    // How many dimensions the fits actually used, discovered from the
    // artifact rather than assumed, and published so a reader never has to
    // infer it (this stage having silently been 3-D regardless of config is
    // exactly what that inference used to get wrong).
    std::size_t scored_dims = 0;
    for (const auto& [date, ticker_points] : by_date) {
        if (!ticker_points.empty()) {
            scored_dims = ticker_points.front().second.dims();
            break;
        }
    }
    std::string first_mesh_error;

    // View A: per frame, fit to every ticker present that frame, score
    // every ticker in that same frame against its own frame's fit.
    for (const auto& [date, ticker_points] : by_date) {
        std::vector<FramePoint> frame_points;
        frame_points.reserve(ticker_points.size());
        for (const auto& [ticker, fp] : ticker_points) frame_points.push_back(fp);
        Eigen::MatrixXd training = points_to_matrix(frame_points);

        bool any_scored = false;
        for (const auto& [ticker, fp] : ticker_points) {
            Eigen::VectorXd query = point_to_vector(fp);
            std::size_t rows_before = rows.dates.size();
            score_both_estimators(rows, training, query, date, ticker, "A", alpha);
            if (rows.dates.size() > rows_before) any_scored = true;
        }
        if (any_scored) ++view_a_frames_scored;

        if (write_meshes && (frame_index % mesh_frame_stride) == 0) {
            auto exported = export_frame_mesh(training, date, output_dir / "surfaces", alpha,
                                               static_cast<int>(mesh_resolution));
            if (exported) {
                ++meshes_written;
            } else {
                // A frame whose surface cannot be extracted does not halt
                // the stage - the scores are the deliverable and they are
                // already computed. But a run that silently produced zero
                // meshes while reporting success is exactly the failure
                // this stage's other counters exist to prevent, so the
                // count and the first message both reach the manifest.
                ++mesh_failures;
                if (first_mesh_error.empty()) {
                    first_mesh_error = date + ": " + exported.error().message;
                }
            }
        }
        ++frame_index;
    }

    // View B: per ticker, strictly causal - the trailing
    // view_b_lookback_days of THAT TICKER's own prior history (never
    // including today's own point), scoring today's point against it.
    for (const auto& [ticker, history] : by_ticker) {
        for (std::size_t i = static_cast<std::size_t>(view_b_lookback); i < history.size(); ++i) {
            std::vector<FramePoint> window(history.begin() + static_cast<std::ptrdiff_t>(i - static_cast<std::size_t>(view_b_lookback)),
                                            history.begin() + static_cast<std::ptrdiff_t>(i));
            Eigen::MatrixXd training = points_to_matrix(window);
            Eigen::VectorXd query = point_to_vector(history[i]);

            std::size_t rows_before = rows.dates.size();
            score_both_estimators(rows, training, query, history[i].date, ticker, "B", alpha);
            if (rows.dates.size() > rows_before) ++view_b_points_scored;
        }
    }

    // Read out before the vectors below are moved-from into the Table -
    // these two counters are plain integers, unaffected by those moves,
    // but capturing them into locals here keeps that fact from having
    // to be re-verified by whoever edits this function next.
    std::size_t mahalanobis_failures = rows.mahalanobis_failures;
    std::size_t kde_failures = rows.kde_failures;
    std::size_t fastmcd_failures = rows.fastmcd_failures;

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
    manifest.set_int("embedding_dims_scored", static_cast<std::int64_t>(scored_dims));
    manifest.set_int("view_a_frames_scored", view_a_frames_scored);
    manifest.set_int("view_b_points_scored", view_b_points_scored);
    manifest.set_double("alpha", alpha);
    manifest.set_int("view_b_lookback_days", view_b_lookback);
    if (write_meshes) {
        manifest.set_string("mesh_export", "enabled");
        manifest.set_int("mesh_dims", 3);
        if (scored_dims > 3) {
            manifest.set_string("mesh_projection_note",
                                 "surfaces are the first three embedding dimensions; scores use " +
                                     std::to_string(scored_dims));
        }
        manifest.set_int("meshes_written", meshes_written);
        manifest.set_int("mesh_failures", mesh_failures);
        manifest.set_int("mesh_resolution", mesh_resolution);
        manifest.set_int("mesh_frame_stride", mesh_frame_stride);
        if (!first_mesh_error.empty()) manifest.set_string("mesh_first_error", first_mesh_error);
    } else {
        manifest.set_string("mesh_export", "disabled (boundaries.write_meshes = false)");
    }
    // Expected row count is (view_a_frames_scored + view_b_points_scored)
    // * 2 estimators; these counters make the gap between that and
    // rows_written attributable, instead of requiring the reader to
    // reconstruct it by hand. Zero here (the common case) is itself
    // useful confirmation, not just an absence of information.
    manifest.set_int("mahalanobis_failures", static_cast<std::int64_t>(mahalanobis_failures));
    manifest.set_int("kde_failures", static_cast<std::int64_t>(kde_failures));
    manifest.set_int("fastmcd_failures", static_cast<std::int64_t>(fastmcd_failures));

    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-boundaries", run_gm_boundaries);
}

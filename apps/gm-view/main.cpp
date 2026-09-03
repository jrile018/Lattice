// gm-view: the interactive viewer (ADR-018). Strictly read-only - it
// loads run artifacts gm-geometry already wrote and draws them; it
// computes nothing financial. This first pass covers the Manifold tab
// (3D point cloud, orbit camera) and the Evolution tab's time scrubber
// plus the structural_change strip (ImPlot); the boundary-surface mesh
// rendering, MST edges, per-point trails, and the click-to-learn panel
// are follow-up work, not attempted in this pass.

#include <glad/glad.h>  // must be included before GLFW's own gl.h pulls a conflicting one

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <gm-core/error.hpp>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdlib>
#include <limits>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <map>

#include "camera.hpp"
#include "data_loader.hpp"
#include "gl.hpp"

#include <gm-io/mesh.hpp>

#include <filesystem>

namespace {

struct AppState {
    std::vector<gm::view::RunInfo> runs;
    int selected_run = -1;
    gm::view::LoadedRun loaded;
    bool has_loaded_run = false;
    int current_frame = 0;
    bool playing = false;
    double play_accum_seconds = 0.0;

    gm::view::OrbitCamera camera;
    bool camera_initialized = false;
    // UI state for three new tabs
    int selected_tab = 0;  // 0=Manifold, 1=2D Pairs, 2=3D Sectors, 3=Evolution
    // The boundary surface is drawn when the run has one on disk. Runs made
    // without boundaries.write_meshes have no surfaces/ directory at all, in
    // which case this toggle simply has nothing to show - which the control
    // panel says explicitly rather than leaving the user wondering whether
    // the checkbox is broken.
    bool show_surface = true;
    bool surface_available = false;
    int selected_ticker1_idx = -1;
    int selected_ticker2_idx = -1;
    std::vector<const char*> available_ticker_labels;
    bool show_sectors = false;
    bool show_learn_panel = false;
    std::string learn_panel_ticker;
    // sector -> distinct RGBA color, one entry per real GICS sector
    // present in the loaded run (see assign_sector_colors below).
    // Rebuilt once per run load, not derived per-point every frame.
    std::map<std::string, std::uint32_t> sector_colors;
};

void glfw_error_callback(int error, const char* description) {
    spdlog::error("gm-view: GLFW error {}: {}", error, description);
}

/// Assigns each distinct GICS sector actually present in `state.loaded`
/// a genuinely distinct color, alphabetically: palette[i] for the i-th
/// sector in sorted order. This is collision-free by construction
/// (unlike a hash mod palette-size, which guarantees a collision for
/// GICS's 11 sectors against anything smaller than an 11-entry
/// palette) as long as the palette has at least as many entries as
/// there are distinct sectors - checked below with a fallback color
/// for any overflow, which should never trigger for real GICS data.
///
/// Palette literals are plain RGBA byte order (0xRRGGBBAA) throughout -
/// the previous palette mixed ARGB-authored literals (0xAARRGGBB, alpha
/// first) into a table that was always decoded as RGBA, so 8 of its 10
/// entries silently rendered with red=0xFF.
void assign_sector_colors(AppState& state) {
    static const std::vector<std::uint32_t> palette = {
        0xE6194BFF,  // red
        0x3CB44BFF,  // green
        0xFFE119FF,  // yellow
        0x4363D8FF,  // blue
        0xF58231FF,  // orange
        0x911EB4FF,  // purple
        0x42D4F4FF,  // cyan
        0xF032E6FF,  // magenta
        0xBFEF45FF,  // lime
        0xFABED4FF,  // pink
        0x469990FF,  // teal
        0x9A6324FF,  // brown
    };
    std::set<std::string> sectors;
    for (const auto& [ticker, meta] : state.loaded.ticker_metadata) {
        sectors.insert(meta.gics_sector);
    }
    state.sector_colors.clear();
    std::size_t i = 0;
    for (const auto& sector : sectors) {  // std::set iterates sorted ascending
        state.sector_colors[sector] = palette[i % palette.size()];
        ++i;
    }
}

std::uint32_t sector_to_color(const AppState& state, const std::string& sector) {
    auto it = state.sector_colors.find(sector);
    if (it != state.sector_colors.end()) return it->second;
    return 0x808080FF;  // neutral gray - a sector with no assigned color (shouldn't happen for real GICS data)
}

/// Ticker `ticker`'s full spread z-score series across every date
/// gm-signals scored it, in ascending date order - NOT filtered to a
/// single date. spreads.parquet has exactly one row per (ticker, date),
/// so filtering on both ticker AND date (the previous behavior) could
/// only ever return zero or one point, which is why the plot rendered
/// a single point. This also plots s.z (the actual z-score, the
/// quantity the +/-1/+/-2 sigma reference bands are calibrated
/// against) rather than s.spread (the raw log-price residual, a
/// different quantity on a different scale that happened to compile
/// but read as nonsense next to those bands).
std::vector<std::pair<std::string, double>> get_spread_series_for_ticker(
    const gm::view::LoadedRun& loaded, const std::string& ticker) {
    std::vector<std::pair<std::string, double>> result;
    for (const auto& s : loaded.spreads) {
        if (s.ticker == ticker) {
            result.push_back({s.date, s.z});
        }
    }
    // gm-signals writes spreads.parquet ticker-major/date-ascending, so
    // this is normally already sorted - sorting defensively here keeps
    // the plot correct even if that upstream ordering ever changes.
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return result;
}

const std::vector<gm::view::Excursion> kEmptyExcursions;

/// O(log n) index lookup (built once in load_run) rather than a linear
/// scan of every excursion row on every rendered frame.
const std::vector<gm::view::Excursion>& get_excursions_for_ticker(
    const gm::view::LoadedRun& loaded, const std::string& ticker) {
    auto it = loaded.excursions_by_ticker.find(ticker);
    if (it == loaded.excursions_by_ticker.end()) return kEmptyExcursions;
    return it->second;
}

/// Recolors/repositions the uploaded point cloud for `frame_idx` and
/// recenters the camera's orbit target on that frame's centroid (the
/// camera's distance/yaw/pitch stay under the user's own mouse control -
/// only the recentering follows the data, so switching frames doesn't
/// fight whatever zoom/angle the user already set).
void upload_frame(gm::view::PointCloudRenderer& renderer, gm::view::MeshRenderer& mesh_renderer,
                   AppState& state, int frame_idx) {
    if (frame_idx < 0 || static_cast<std::size_t>(frame_idx) >= state.loaded.frames.size()) return;
    const auto& frame = state.loaded.frames[static_cast<std::size_t>(frame_idx)];

    // The View A boundary surface for this exact date, if the run exported
    // one. Cleared first so a frame without a mesh shows no mesh, rather
    // than leaving the previous frame's envelope on screen around a
    // different day's points - a picture that would look plausible and be
    // wrong.
    mesh_renderer.clear();
    if (state.selected_run >= 0 &&
        static_cast<std::size_t>(state.selected_run) < state.runs.size()) {
        const auto surface_path =
            state.runs[static_cast<std::size_t>(state.selected_run)].run_dir /
            "gm-boundaries" / "surfaces" / (frame.date + "_A.gmmesh");
        if (std::filesystem::exists(surface_path)) {
            auto mesh = gm::io::read_gmmesh(surface_path);
            if (mesh) {
                mesh_renderer.upload(*mesh);
            } else {
                // A corrupt or unreadable surface is worth saying out loud
                // once; it must not take the whole viewer down, since the
                // points are still perfectly renderable without it.
                spdlog::warn("gm-view: could not read boundary surface {}: {}",
                             surface_path.string(), mesh.error().to_string());
            }
        }
    }

    state.available_ticker_labels.clear();
    for (const auto& t : frame.tickers) {
        state.available_ticker_labels.push_back(t.c_str());
    }

    std::vector<gm::view::PointVertex> verts;
    verts.reserve(frame.positions.size());

    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (const auto& p : frame.positions) {
        cx += p[0];
        cy += p[1];
        cz += p[2];
    }
    if (!frame.positions.empty()) {
        float n = static_cast<float>(frame.positions.size());
        cx /= n;
        cy /= n;
        cz /= n;
    }

    for (std::size_t i = 0; i < frame.positions.size(); ++i) {
        const auto& p = frame.positions[i];
        // Plain, single color for this first pass - color-by-depth/
        // momentum (ADR-018's toggle list) is follow-up work once
        // View A/B scores are wired into the loader. Sector coloring
        // (added here) looks the ticker up by its parallel index into
        // frame.tickers, since frame.positions carries no identity of
        // its own.
        std::uint32_t color = 0x5A85FFFF;
        if (state.show_sectors && i < frame.tickers.size()) {
            auto meta_it = state.loaded.ticker_metadata.find(frame.tickers[i]);
            if (meta_it != state.loaded.ticker_metadata.end()) {
                color = sector_to_color(state, meta_it->second.gics_sector);
            }
        }
        float r = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
        float g = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
        float b = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
        verts.push_back({p[0], p[1], p[2], r, g, b});
    }
    renderer.upload(verts);

    state.camera.target_x = cx;
    state.camera.target_y = cy;
    state.camera.target_z = cz;

    if (!state.camera_initialized) {
        float max_dist = 0.1f;
        for (const auto& p : frame.positions) {
            float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
            max_dist = std::max(max_dist, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        state.camera.distance = max_dist * 3.0f;
        state.camera_initialized = true;
    }
}

void load_selected_run(AppState& state, gm::view::PointCloudRenderer& renderer,
                        gm::view::MeshRenderer& mesh_renderer) {
    if (state.selected_run < 0 || static_cast<std::size_t>(state.selected_run) >= state.runs.size()) {
        return;
    }
    auto loaded = gm::view::load_run(state.runs[static_cast<std::size_t>(state.selected_run)].run_dir);
    if (!loaded) {
        spdlog::error("gm-view: failed to load run: {}", loaded.error().to_string());
        state.has_loaded_run = false;
        return;
    }
    state.loaded = std::move(*loaded);
    state.has_loaded_run = true;
    state.current_frame = 0;
    state.camera_initialized = false;
    // Refresh unconditionally: available_ticker_labels holds const
    // char* pointers into state.loaded's own strings (frame.tickers),
    // and state.loaded was just replaced above. Previously this only
    // happened inside upload_frame, called only when frames is
    // non-empty - a run with zero geometry frames (has_loaded_run
    // still true) left the old run's now-dangling pointers in place.
    state.available_ticker_labels.clear();
    state.selected_ticker1_idx = -1;
    state.selected_ticker2_idx = -1;
    assign_sector_colors(state);
    // Whether this run has surfaces at all, decided once per run rather
    // than probed every frame.
    state.surface_available = false;
    if (state.selected_run >= 0 &&
        static_cast<std::size_t>(state.selected_run) < state.runs.size()) {
        state.surface_available = std::filesystem::is_directory(
            state.runs[static_cast<std::size_t>(state.selected_run)].run_dir / "gm-boundaries" /
            "surfaces");
    }
    if (!state.loaded.frames.empty()) upload_frame(renderer, mesh_renderer, state, 0);
}

} // namespace

int main(int argc, char** argv) {
    std::string runs_base_dir = "runs";
    int start_frame = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--start-frame") {
            if (i + 1 >= argc) {
                spdlog::error("gm-view: --start-frame requires a numeric argument");
                return 1;
            }
            // atoi silently returns 0 on any non-numeric input ("abc",
            // or a typo'd flag like "--start-fame" that this loop would
            // otherwise swallow as runs_base_dir) - strtol with an
            // endptr check tells a genuine parse failure apart from a
            // legitimate "0".
            const char* arg_str = argv[++i];
            char* end = nullptr;
            errno = 0;
            long parsed = std::strtol(arg_str, &end, 10);
            if (end == arg_str || *end != '\0' || errno == ERANGE || parsed < 0 ||
                parsed > std::numeric_limits<int>::max()) {
                spdlog::error("gm-view: --start-frame must be a non-negative integer, got '{}'",
                               arg_str);
                return 1;
            }
            start_frame = static_cast<int>(parsed);
        } else {
            runs_base_dir = arg;
        }
    }

#if defined(__linux__)
    // On Linux, glfwInit() against neither an X11 nor a Wayland display
    // (a bare SSH session, a CI box, a container with no compositor)
    // fails in a way that's easy to misread as a build or driver
    // problem rather than "there's nowhere to put a window." Check the
    // obvious cause first and say so plainly - gm-view is genuinely a
    // GUI application with no headless mode, not a bug to work around.
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
        spdlog::error(
            "gm-view: no DISPLAY or WAYLAND_DISPLAY set - this is a GUI application and needs "
            "an X11 or Wayland session to open a window (e.g. run against a real desktop "
            "session's display, such as DISPLAY=:0)");
        return 1;
    }
#endif

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        spdlog::error("gm-view: glfwInit failed");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "gm-view", nullptr, nullptr);
    if (!window) {
        spdlog::error("gm-view: glfwCreateWindow failed");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        spdlog::error("gm-view: gladLoadGLLoader failed");
        return 1;
    }
    spdlog::info("gm-view: OpenGL {} ({})", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    auto renderer_result = gm::view::PointCloudRenderer::create();
    if (!renderer_result) {
        spdlog::error("gm-view: failed to create point cloud renderer: {}",
                      renderer_result.error().to_string());
        return 1;
    }
    gm::view::PointCloudRenderer renderer = std::move(*renderer_result);

    auto mesh_renderer_result = gm::view::MeshRenderer::create();
    if (!mesh_renderer_result) {
        spdlog::error("gm-view: failed to create mesh renderer: {}",
                      mesh_renderer_result.error().to_string());
        return 1;
    }
    gm::view::MeshRenderer mesh_renderer = std::move(*mesh_renderer_result);

    AppState state;
    auto runs = gm::view::list_available_runs(runs_base_dir);
    if (runs) {
        state.runs = std::move(*runs);
        if (!state.runs.empty()) {
            state.selected_run = 0;
            load_selected_run(state, renderer, mesh_renderer);
            if (start_frame > 0 && !state.loaded.frames.empty()) {
                state.current_frame =
                    std::min(start_frame, static_cast<int>(state.loaded.frames.size()) - 1);
                upload_frame(renderer, mesh_renderer, state, state.current_frame);
            }
        }
    } else {
        spdlog::error("gm-view: failed to list runs: {}", runs.error().to_string());
    }

    double last_time = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        double now = glfwGetTime();
        double dt = now - last_time;
        last_time = now;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                state.camera.orbit(-io.MouseDelta.x * 0.01f, io.MouseDelta.y * 0.01f);
            }
            if (io.MouseWheel != 0.0f) {
                state.camera.zoom(-io.MouseWheel * state.camera.distance * 0.1f);
            }
        }

        if (state.playing && state.has_loaded_run && !state.loaded.frames.empty()) {
            state.play_accum_seconds += dt;
            constexpr double kSecondsPerFrame = 0.15;
            if (state.play_accum_seconds >= kSecondsPerFrame) {
                state.play_accum_seconds = 0.0;
                state.current_frame =
                    (state.current_frame + 1) % static_cast<int>(state.loaded.frames.size());
                upload_frame(renderer, mesh_renderer, state, state.current_frame);
            }
        }

        // AlwaysAutoResize, not the default: ImGui only auto-fits a
        // window's size once, on its very first frame - without this
        // flag, the control panel stays frozen at whatever it measured
        // before data finished loading (just the run selector, before
        // the frame slider/Play button/regime plot exist), and the
        // rest of the content silently clips instead of the window
        // growing to show it. Found by actually screenshotting the
        // running app, not by reading the ImGui docs and assuming.
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, 750), ImGuiCond_FirstUseEver);
        ImGui::Begin("gm-view Control Panel", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Runs (%s)", runs_base_dir.c_str());
        if (state.runs.empty()) {
            ImGui::TextDisabled("No runs found - run gm-run first.");
        } else {
            std::vector<const char*> run_labels;
            run_labels.reserve(state.runs.size());
            for (const auto& r : state.runs) run_labels.push_back(r.run_id.c_str());
            if (ImGui::Combo("Run", &state.selected_run, run_labels.data(),
                              static_cast<int>(run_labels.size()))) {
                load_selected_run(state, renderer, mesh_renderer);
            }
        }

        if (state.has_loaded_run && !state.loaded.frames.empty()) {
            ImGui::Separator();
            ImGui::Text("Frame: %s (%d/%zu)",
                        state.loaded.frames[static_cast<std::size_t>(state.current_frame)].date.c_str(),
                        state.current_frame + 1, state.loaded.frames.size());
            int max_frame = static_cast<int>(state.loaded.frames.size()) - 1;
            if (ImGui::SliderInt("##frame", &state.current_frame, 0, max_frame)) {
                upload_frame(renderer, mesh_renderer, state, state.current_frame);
            }
            ImGui::SameLine();
            if (ImGui::Button(state.playing ? "Pause" : "Play")) state.playing = !state.playing;

            ImGui::Text("Tickers this frame: %zu",
                        state.loaded.frames[static_cast<std::size_t>(state.current_frame)].tickers.size());
            ImGui::TextDisabled("Left-drag to orbit, scroll to zoom.");

            // Tab selection
            ImGui::Separator();
            ImGui::BeginTabBar("ViewTabs");
            if (state.surface_available) {
                ImGui::Checkbox("Boundary surface", &state.show_surface);
                if (state.show_surface && mesh_renderer.empty()) {
                    ImGui::TextDisabled("no surface exported for this frame");
                }
            } else {
                ImGui::TextDisabled("No boundary surfaces in this run.");
                ImGui::TextDisabled("Re-run gm-boundaries with boundaries.write_meshes = true.");
            }

            if (ImGui::TabItemButton("Manifold", state.selected_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                state.selected_tab = 0;
            }
            if (ImGui::TabItemButton("2D Pairs", state.selected_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                state.selected_tab = 1;
            }
            if (ImGui::TabItemButton("3D Sectors", state.selected_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                state.selected_tab = 2;
            }
            if (ImGui::TabItemButton("Evolution", state.selected_tab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                state.selected_tab = 3;
            }
            ImGui::EndTabBar();

            // Tab-specific controls
            if (state.selected_tab == 1) {
                ImGui::Separator();
                ImGui::Text("2D Pairs - Select Two Tickers");
                ImGui::Combo("Ticker 1", &state.selected_ticker1_idx,
                             state.available_ticker_labels.data(),
                             static_cast<int>(state.available_ticker_labels.size()));
                ImGui::Combo("Ticker 2", &state.selected_ticker2_idx,
                             state.available_ticker_labels.data(),
                             static_cast<int>(state.available_ticker_labels.size()));

                // Spread with bands (ADR §9): ticker1's full z-score
                // series across all dates gm-signals scored it for the
                // currently loaded run, plotted against the standard
                // +/-1, +/-2 sigma reference bands used everywhere else
                // in this codebase for excursion thresholds (ADR §6.5's
                // entry/exit z bands). Not filtered to the current
                // scrubber frame's date - spreads.parquet has one row
                // per (ticker, date), so a per-date filter here could
                // only ever produce zero or one point.
                if (state.selected_ticker1_idx >= 0 &&
                    static_cast<std::size_t>(state.selected_ticker1_idx) < state.available_ticker_labels.size() &&
                    state.has_loaded_run) {
                    const std::string t1 = state.available_ticker_labels[static_cast<std::size_t>(state.selected_ticker1_idx)];
                    auto spread_series = get_spread_series_for_ticker(state.loaded, t1);
                    if (!spread_series.empty()) {
                        std::vector<float> spread_z;
                        spread_z.reserve(spread_series.size());
                        for (const auto& [date, z] : spread_series) spread_z.push_back(static_cast<float>(z));
                        std::vector<float> upper1(spread_z.size(), 1.0f), lower1(spread_z.size(), -1.0f);
                        std::vector<float> upper2(spread_z.size(), 2.0f), lower2(spread_z.size(), -2.0f);
                        ImGui::Separator();
                        ImGui::Text("Spread (z) for %s (%zu dates, %s to %s)", t1.c_str(), spread_series.size(),
                                    spread_series.front().first.c_str(), spread_series.back().first.c_str());
                        if (ImPlot::BeginPlot("##spread_bands", ImVec2(-1, 150))) {
                            ImPlot::PlotLine("z", spread_z.data(), static_cast<int>(spread_z.size()));
                            ImPlot::PlotLine("+-1 sigma", upper1.data(), static_cast<int>(upper1.size()));
                            ImPlot::PlotLine("-1 sigma", lower1.data(), static_cast<int>(lower1.size()));
                            ImPlot::PlotLine("+-2 sigma", upper2.data(), static_cast<int>(upper2.size()));
                            ImPlot::PlotLine("-2 sigma", lower2.data(), static_cast<int>(lower2.size()));
                            ImPlot::EndPlot();
                        }
                    } else {
                        ImGui::TextDisabled("No spread data for %s.", t1.c_str());
                    }
                }
            } else if (state.selected_tab == 2) {
                ImGui::Separator();
                ImGui::Text("3D Sectors");
                if (ImGui::Checkbox("Color by Sector", &state.show_sectors)) {
                    upload_frame(renderer, mesh_renderer, state, state.current_frame);
                }
                ImGui::Text("Sectors in universe:");
                std::set<std::string> sectors_seen;
                std::map<std::string, int> sector_ticker_count;
                for (const auto& [ticker, meta] : state.loaded.ticker_metadata) {
                    sectors_seen.insert(meta.gics_sector);
                    sector_ticker_count[meta.gics_sector]++;
                }
                ImGui::Text("  %zu sectors, %zu tickers", sectors_seen.size(), state.loaded.ticker_metadata.size());
                for (const auto& sector : sectors_seen) {
                    ImGui::Text("  - %s (%d tickers)", sector.c_str(), sector_ticker_count[sector]);
                }
                ImGui::Separator();
                ImGui::Text("Sector Analysis (current frame):");
                if (state.has_loaded_run && !state.loaded.frames.empty()) {
                    const auto& frame = state.loaded.frames[static_cast<std::size_t>(state.current_frame)];
                    std::map<std::string, int> frame_sector_counts;
                    for (const auto& ticker : frame.tickers) {
                        auto meta_it = state.loaded.ticker_metadata.find(ticker);
                        if (meta_it != state.loaded.ticker_metadata.end()) {
                            frame_sector_counts[meta_it->second.gics_sector]++;
                        }
                    }
                    for (const auto& [sector, count] : frame_sector_counts) {
                        ImGui::Text("  %s: %d points", sector.c_str(), count);
                    }
                }
            } else if (state.selected_tab == 3) {
                ImGui::Separator();
                ImGui::Text("Structural change (Evolution)");
            }

            if (!state.loaded.structural_change.empty() && state.selected_tab == 3) {
                ImGui::Separator();
                if (ImPlot::BeginPlot("##regime", ImVec2(-1, 150))) {
                    ImPlot::PlotLine("structural_change", state.loaded.structural_change.data(),
                                     static_cast<int>(state.loaded.structural_change.size()));
                    ImPlot::EndPlot();
                }
            }

            // Learn panel - triggered by clicking 2D Pairs table or from manual selection
            if (state.show_learn_panel && !state.learn_panel_ticker.empty()) {
                ImGui::Separator();
                ImGui::Text("Learn Panel: %s", state.learn_panel_ticker.c_str());

                auto meta_it = state.loaded.ticker_metadata.find(state.learn_panel_ticker);
                if (meta_it != state.loaded.ticker_metadata.end()) {
                    ImGui::Text("Company: %s", meta_it->second.security_name.c_str());
                    ImGui::Text("Sector: %s", meta_it->second.gics_sector.c_str());
                }

                // SEC EDGAR profile (meta/profiles.json, ADR §8.2) - not
                // every run has this (older runs predate gm-profiles,
                // or a given ticker's fetch failed), so this degrades
                // gracefully to a plain status line rather than assuming
                // the entry exists.
                auto profile_it = state.loaded.profiles.find(state.learn_panel_ticker);
                if (profile_it != state.loaded.profiles.end()) {
                    const auto& profile = profile_it->second;
                    ImGui::Separator();
                    ImGui::Text("SEC Profile:");
                    if (!profile.company_name.empty()) {
                        ImGui::Text("  Entity: %s", profile.company_name.c_str());
                    }
                    if (!profile.sic_code.empty() || !profile.sic_description.empty()) {
                        ImGui::Text("  SIC: %s%s%s", profile.sic_code.c_str(),
                                    profile.sic_description.empty() ? "" : " - ",
                                    profile.sic_description.c_str());
                    }
                    if (!profile.edgar_url.empty()) {
                        ImGui::TextWrapped("  EDGAR: %s", profile.edgar_url.c_str());
                    }
                } else if (state.has_loaded_run) {
                    ImGui::Separator();
                    ImGui::TextDisabled("No SEC profile data for %s.", state.learn_panel_ticker.c_str());
                }

                if (state.has_loaded_run && !state.loaded.frames.empty()) {
                    const auto& frame = state.loaded.frames[static_cast<std::size_t>(state.current_frame)];

                    // The "Learn about <pair ticker>" jump buttons below
                    // refer back to whatever pair is currently selected
                    // on the 2D Pairs tab, so the panel for one ticker
                    // can jump straight to the panel for its comparison
                    // partner. Guarded against an empty/out-of-range
                    // selection (no run loaded yet, or an empty universe).
                    const std::string ticker1 =
                        (state.selected_ticker1_idx >= 0 &&
                         static_cast<std::size_t>(state.selected_ticker1_idx) < state.available_ticker_labels.size())
                            ? state.available_ticker_labels[static_cast<std::size_t>(state.selected_ticker1_idx)]
                            : "";
                    const std::string ticker2 =
                        (state.selected_ticker2_idx >= 0 &&
                         static_cast<std::size_t>(state.selected_ticker2_idx) < state.available_ticker_labels.size())
                            ? state.available_ticker_labels[static_cast<std::size_t>(state.selected_ticker2_idx)]
                            : "";

                    // Renders the two "Learn about <ticker>" jump buttons
                    // shared by every section below. `scope` gives each
                    // call site's button pair its own ImGui ID scope, so
                    // the six buttons across the three sections (which,
                    // with at most two distinct visible labels - or all
                    // six identical and empty when no 2D-Pairs pair is
                    // selected yet - previously collided onto the first
                    // button's ID) no longer alias each other. Each
                    // ticker's own button is also skipped entirely when
                    // that ticker string is empty, rather than rendering
                    // a "Learn about " button that, if clicked, would set
                    // learn_panel_ticker to "" and immediately close this
                    // panel via the !empty() guard above.
                    auto learn_jump_buttons = [&](const char* scope) {
                        ImGui::PushID(scope);
                        if (!ticker1.empty()) {
                            ImGui::PushID(0);
                            if (ImGui::Button(("Learn about " + ticker1).c_str(), ImVec2(-1, 0))) {
                                state.show_learn_panel = true;
                                state.learn_panel_ticker = ticker1;
                            }
                            ImGui::PopID();
                        }
                        if (!ticker2.empty()) {
                            ImGui::PushID(1);
                            if (ImGui::Button(("Learn about " + ticker2).c_str(), ImVec2(-1, 0))) {
                                state.show_learn_panel = true;
                                state.learn_panel_ticker = ticker2;
                            }
                            ImGui::PopID();
                        }
                        ImGui::PopID();
                    };

                    // Scores (Position & Depth in all views) - indexed
                    // lookup (built once in load_run) rather than a
                    // linear scan of all 1.82M+ real scores rows on
                    // every rendered frame (ADR-9's <1ms decode budget).
                    std::map<std::string, std::vector<std::pair<std::string, double>>> scores_by_view;
                    auto scores_it =
                        state.loaded.scores_by_ticker_date.find({state.learn_panel_ticker, frame.date});
                    if (scores_it != state.loaded.scores_by_ticker_date.end()) {
                        for (const auto& score : scores_it->second) {
                            scores_by_view[score.view].push_back({score.estimator, score.depth});
                        }
                    }

                    if (!scores_by_view.empty()) {
                        ImGui::Separator();
                        ImGui::Text("Position & Depth (View A/B/C):");
                        if (ImGui::BeginTable("scores_tbl", 3, ImGuiTableFlags_Borders)) {
                            ImGui::TableSetupColumn("View");
                            ImGui::TableSetupColumn("Estimator");
                            ImGui::TableSetupColumn("Depth");
                            ImGui::TableHeadersRow();
                            for (const auto& [view, depths] : scores_by_view) {
                                for (const auto& [est, depth] : depths) {
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", view.c_str());
                                    ImGui::TableSetColumnIndex(1); ImGui::Text("%s", est.c_str());
                                    ImGui::TableSetColumnIndex(2); ImGui::Text("%.4f", depth);
                                }
                            }
                            ImGui::EndTable();
                        }
                        learn_jump_buttons("scores_learn_buttons");
                    }

                    // Peer basket - indexed lookup rather than a linear
                    // scan of all 3.92M+ real basket rows every frame.
                    std::vector<std::pair<std::string, double>> basket;
                    auto basket_it =
                        state.loaded.baskets_by_ticker_date.find({state.learn_panel_ticker, frame.date});
                    if (basket_it != state.loaded.baskets_by_ticker_date.end()) {
                        for (const auto& b : basket_it->second) {
                            basket.push_back({b.neighbor_ticker, b.weight});
                        }
                    }
                    if (!basket.empty()) {
                        ImGui::Separator();
                        ImGui::Text("Peer Basket (Weights):");
                        if (ImGui::BeginTable("basket_tbl", 2, ImGuiTableFlags_Borders)) {
                            ImGui::TableSetupColumn("Ticker");
                            ImGui::TableSetupColumn("Weight");
                            ImGui::TableHeadersRow();
                            for (const auto& [ticker, weight] : basket) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", ticker.c_str());
                                ImGui::TableSetColumnIndex(1); ImGui::Text("%.4f", weight);
                            }
                            ImGui::EndTable();
                        }
                        learn_jump_buttons("basket_learn_buttons");
                    }

                    // Excursion history - indexed lookup, built once in load_run.
                    const auto& excursions = get_excursions_for_ticker(state.loaded, state.learn_panel_ticker);
                    if (!excursions.empty()) {
                        ImGui::Separator();
                        ImGui::Text("Excursion History (%zu total):", excursions.size());
                        if (ImGui::BeginTable("exc_hist", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders)) {
                            ImGui::TableSetupColumn("Start");
                            ImGui::TableSetupColumn("End");
                            ImGui::TableSetupColumn("Duration");
                            ImGui::TableSetupColumn("Peak Depth");
                            ImGui::TableSetupColumn("Reverted");
                            ImGui::TableHeadersRow();
                            for (const auto& exc : excursions) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", exc.start_date.c_str());
                                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", exc.end_date.c_str());
                                ImGui::TableSetColumnIndex(2); ImGui::Text("%d", exc.duration_days);
                                ImGui::TableSetColumnIndex(3); ImGui::Text("%.4f", exc.peak_depth);
                                ImGui::TableSetColumnIndex(4); ImGui::Text("%s", exc.reverted ? "Yes" : "No");
                            }
                            ImGui::EndTable();
                        }
                        learn_jump_buttons("excursion_learn_buttons");
                    }
                }
            }
        }
        ImGui::End();

        int fb_width = 0, fb_height = 0;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        glViewport(0, 0, fb_width, fb_height);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        if (fb_height > 0) {
            float aspect = static_cast<float>(fb_width) / static_cast<float>(fb_height);
            gm::view::Mat4 projection = gm::view::mat4_perspective(0.9f, aspect, 0.01f, 1000.0f);
            gm::view::Mat4 view = state.camera.view_matrix();
            gm::view::Mat4 mvp = gm::view::mat4_multiply(projection, view);
            // Surface first, points second: the points are the subject and
            // must win the depth test against the envelope around them.
            if (state.show_surface) {
                mesh_renderer.draw(mvp, 0.24f, 0.85f, 0.36f, 0.28f);
            }
            renderer.draw(mvp, 8.0f);
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

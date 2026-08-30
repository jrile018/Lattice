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

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "camera.hpp"
#include "data_loader.hpp"
#include "gl.hpp"

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
};

void glfw_error_callback(int error, const char* description) {
    spdlog::error("gm-view: GLFW error {}: {}", error, description);
}

/// Recolors/repositions the uploaded point cloud for `frame_idx` and
/// recenters the camera's orbit target on that frame's centroid (the
/// camera's distance/yaw/pitch stay under the user's own mouse control -
/// only the recentering follows the data, so switching frames doesn't
/// fight whatever zoom/angle the user already set).
void upload_frame(gm::view::PointCloudRenderer& renderer, AppState& state, int frame_idx) {
    if (frame_idx < 0 || static_cast<std::size_t>(frame_idx) >= state.loaded.frames.size()) return;
    const auto& frame = state.loaded.frames[static_cast<std::size_t>(frame_idx)];

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

    for (const auto& p : frame.positions) {
        // Plain, single color for this first pass - color-by-depth/
        // sector/momentum (ADR-018's toggle list) is follow-up work
        // once View A/B scores are wired into the loader.
        verts.push_back({p[0], p[1], p[2], 0.35f, 0.65f, 1.0f});
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

void load_selected_run(AppState& state, gm::view::PointCloudRenderer& renderer) {
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
    if (!state.loaded.frames.empty()) upload_frame(renderer, state, 0);
}

} // namespace

int main(int argc, char** argv) {
    std::string runs_base_dir = "runs";
    if (argc > 1) runs_base_dir = argv[1];

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

    AppState state;
    auto runs = gm::view::list_available_runs(runs_base_dir);
    if (runs) {
        state.runs = std::move(*runs);
        if (!state.runs.empty()) {
            state.selected_run = 0;
            load_selected_run(state, renderer);
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
                upload_frame(renderer, state, state.current_frame);
            }
        }

        ImGui::Begin("gm-view");
        ImGui::Text("Runs (%s)", runs_base_dir.c_str());
        if (state.runs.empty()) {
            ImGui::TextDisabled("No runs found - run gm-run first.");
        } else {
            std::vector<const char*> run_labels;
            run_labels.reserve(state.runs.size());
            for (const auto& r : state.runs) run_labels.push_back(r.run_id.c_str());
            if (ImGui::Combo("Run", &state.selected_run, run_labels.data(),
                              static_cast<int>(run_labels.size()))) {
                load_selected_run(state, renderer);
            }
        }

        if (state.has_loaded_run && !state.loaded.frames.empty()) {
            ImGui::Separator();
            ImGui::Text("Frame: %s (%d/%zu)",
                        state.loaded.frames[static_cast<std::size_t>(state.current_frame)].date.c_str(),
                        state.current_frame + 1, state.loaded.frames.size());
            int max_frame = static_cast<int>(state.loaded.frames.size()) - 1;
            if (ImGui::SliderInt("##frame", &state.current_frame, 0, max_frame)) {
                upload_frame(renderer, state, state.current_frame);
            }
            ImGui::SameLine();
            if (ImGui::Button(state.playing ? "Pause" : "Play")) state.playing = !state.playing;

            ImGui::Text("Tickers this frame: %zu",
                        state.loaded.frames[static_cast<std::size_t>(state.current_frame)].tickers.size());
            ImGui::TextDisabled("Left-drag to orbit, scroll to zoom.");

            if (!state.loaded.structural_change.empty()) {
                ImGui::Separator();
                ImGui::Text("Structural change (Evolution)");
                if (ImPlot::BeginPlot("##regime", ImVec2(-1, 150))) {
                    ImPlot::PlotLine("structural_change", state.loaded.structural_change.data(),
                                     static_cast<int>(state.loaded.structural_change.size()));
                    ImPlot::EndPlot();
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

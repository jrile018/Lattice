#include "gl.hpp"

#include <glad/glad.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace gm::view {

namespace {

constexpr const char* kVertexShaderSrc = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
uniform float uPointSize;
out vec3 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
    vColor = aColor;
}
)glsl";

constexpr const char* kFragmentShaderSrc = R"glsl(
#version 330 core
in vec3 vColor;
out vec4 FragColor;
void main() {
    // Round point sprites: discard the corners of the square point quad.
    vec2 coord = gl_PointCoord - vec2(0.5);
    if (dot(coord, coord) > 0.25) discard;
    FragColor = vec4(vColor, 1.0);
}
)glsl";

Result<std::uint32_t> compile_stage(GLenum stage, const std::string& src) {
    std::uint32_t shader = glCreateShader(stage);
    const char* src_ptr = src.c_str();
    glShaderSource(shader, 1, &src_ptr, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        glDeleteShader(shader);
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "GLSL shader compilation failed",
                                               std::string(log, static_cast<std::size_t>(len))));
    }
    return shader;
}

} // namespace

Result<std::uint32_t> compile_shader_program(const std::string& vertex_src,
                                              const std::string& fragment_src) {
    auto vs = compile_stage(GL_VERTEX_SHADER, vertex_src);
    if (!vs) return tl::unexpected(vs.error());
    auto fs = compile_stage(GL_FRAGMENT_SHADER, fragment_src);
    if (!fs) {
        glDeleteShader(*vs);
        return tl::unexpected(fs.error());
    }

    std::uint32_t program = glCreateProgram();
    glAttachShader(program, *vs);
    glAttachShader(program, *fs);
    glLinkProgram(program);

    glDeleteShader(*vs);
    glDeleteShader(*fs);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(program, sizeof(log), &len, log);
        glDeleteProgram(program);
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kNumericFailure,
                                               "GLSL shader program link failed",
                                               std::string(log, static_cast<std::size_t>(len))));
    }
    return program;
}

Result<PointCloudRenderer> PointCloudRenderer::create() {
    auto program = compile_shader_program(kVertexShaderSrc, kFragmentShaderSrc);
    if (!program) return tl::unexpected(program.error());

    PointCloudRenderer renderer;
    renderer.program_ = *program;

    glGenVertexArrays(1, &renderer.vao_);
    glGenBuffers(1, &renderer.vbo_);

    glBindVertexArray(renderer.vao_);
    glBindBuffer(GL_ARRAY_BUFFER, renderer.vbo_);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PointVertex),
                           reinterpret_cast<void*>(offsetof(PointVertex, x)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(PointVertex),
                           reinterpret_cast<void*>(offsetof(PointVertex, r)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    renderer.mvp_uniform_loc_ =
        static_cast<std::uint32_t>(glGetUniformLocation(renderer.program_, "uMVP"));
    renderer.point_size_uniform_loc_ =
        static_cast<std::uint32_t>(glGetUniformLocation(renderer.program_, "uPointSize"));

    return renderer;
}

PointCloudRenderer::~PointCloudRenderer() { release(); }

void PointCloudRenderer::release() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    vbo_ = vao_ = program_ = 0;
}

PointCloudRenderer::PointCloudRenderer(PointCloudRenderer&& other) noexcept
    : program_(other.program_),
      vao_(other.vao_),
      vbo_(other.vbo_),
      mvp_uniform_loc_(other.mvp_uniform_loc_),
      point_size_uniform_loc_(other.point_size_uniform_loc_),
      vertex_count_(other.vertex_count_) {
    other.program_ = other.vao_ = other.vbo_ = 0;
    other.vertex_count_ = 0;
}

PointCloudRenderer& PointCloudRenderer::operator=(PointCloudRenderer&& other) noexcept {
    if (this != &other) {
        release();
        program_ = other.program_;
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        mvp_uniform_loc_ = other.mvp_uniform_loc_;
        point_size_uniform_loc_ = other.point_size_uniform_loc_;
        vertex_count_ = other.vertex_count_;
        other.program_ = other.vao_ = other.vbo_ = 0;
        other.vertex_count_ = 0;
    }
    return *this;
}

void PointCloudRenderer::upload(const std::vector<PointVertex>& points) {
    vertex_count_ = points.size();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(points.size() * sizeof(PointVertex)), points.data(),
                 GL_DYNAMIC_DRAW);
}

void PointCloudRenderer::draw(const Mat4& mvp, float point_size) const {
    if (vertex_count_ == 0) return;
    glUseProgram(program_);
    glUniformMatrix4fv(static_cast<GLint>(mvp_uniform_loc_), 1, GL_FALSE, mvp.data());
    glUniform1f(static_cast<GLint>(point_size_uniform_loc_), point_size);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vertex_count_));
    glBindVertexArray(0);
}

} // namespace gm::view

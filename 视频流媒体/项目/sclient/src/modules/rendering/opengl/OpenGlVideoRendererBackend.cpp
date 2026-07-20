#include "modules/rendering/opengl/OpenGlVideoRendererBackend.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "common/log/Logger.h"
#include "modules/rendering/opengl/OpenGlVideoRendererHud.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace sclient {

namespace {

const std::size_t kPixelUploadBufferCount = 2;

const char *kEmbeddedVertexShaderSource =
        "#version 330 core\n"
        "\n"
        "layout (location = 0) in vec2 a_position;\n"
        "layout (location = 1) in vec2 a_tex_coord;\n"
        "\n"
        "uniform vec2 u_scale;\n"
        "\n"
        "out vec2 v_tex_coord;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = vec4(a_position * u_scale, 0.0, 1.0);\n"
        "    v_tex_coord = a_tex_coord;\n"
        "}\n";

const char *kEmbeddedFragmentShaderSource =
        "#version 330 core\n"
        "\n"
        "in vec2 v_tex_coord;\n"
        "\n"
        "uniform sampler2D u_plane0;\n"
        "uniform sampler2D u_plane1;\n"
        "uniform sampler2D u_plane2;\n"
        "uniform int u_color_mode;\n"
        "\n"
        "out vec4 frag_color;\n"
        "\n"
        "vec3 yuv_to_rgb(float y, float u, float v) {\n"
        "    float u_shifted = u - 0.5;\n"
        "    float v_shifted = v - 0.5;\n"
        "    return vec3(\n"
        "        y + 1.402 * v_shifted,\n"
        "        y - 0.344136 * u_shifted - 0.714136 * v_shifted,\n"
        "        y + 1.772 * u_shifted\n"
        "    );\n"
        "}\n"
        "\n"
        "void main() {\n"
        "    if (u_color_mode == 1) {\n"
        "        float y = texture(u_plane0, v_tex_coord).r;\n"
        "        float u = texture(u_plane1, v_tex_coord).r;\n"
        "        float v = texture(u_plane2, v_tex_coord).r;\n"
        "        frag_color = vec4(yuv_to_rgb(y, u, v), 1.0);\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    if (u_color_mode == 2) {\n"
        "        float y = texture(u_plane0, v_tex_coord).r;\n"
        "        vec2 uv = texture(u_plane1, v_tex_coord).rg;\n"
        "        frag_color = vec4(yuv_to_rgb(y, uv.x, uv.y), 1.0);\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    frag_color = vec4(texture(u_plane0, v_tex_coord).rgb, 1.0);\n"
        "}\n";

enum class NativeColorMode {
    kBgr = 0,
    kYuv420p = 1,
    kNv12 = 2,
};

std::uint64_t MonotonicNowNs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string TrimTrailingWhitespace(const std::string &value) {
    std::size_t end = value.size();
    while (end > 0 && (value[end - 1] == '\n' || value[end - 1] == '\r' || value[end - 1] == ' ')) {
        --end;
    }
    return value.substr(0, end);
}

bool LoadShaderSources(
        std::string *vertex_source,
        std::string *fragment_source,
        std::string *resolved_path,
        std::string *error_message) {
    if (vertex_source == nullptr || fragment_source == nullptr) {
        if (error_message != nullptr) {
            *error_message = "shader source output pointers are null";
        }
        return false;
    }

    *vertex_source = kEmbeddedVertexShaderSource;
    *fragment_source = kEmbeddedFragmentShaderSource;

    if (resolved_path != nullptr) {
        *resolved_path = "(embedded)";
    }
    return true;
}

bool ResolveNativeColorMode(DecodedPixelFormat pixel_format, NativeColorMode *mode) {
    if (mode == nullptr) {
        return false;
    }

    switch (pixel_format) {
        case DecodedPixelFormat::kYuv420p:
            *mode = NativeColorMode::kYuv420p;
            return true;
        case DecodedPixelFormat::kNv12:
            *mode = NativeColorMode::kNv12;
            return true;
        default:
            *mode = NativeColorMode::kBgr;
            return false;
    }
}

bool CompileShader(GLenum shader_type, const std::string &source, GLuint *shader_id, std::string *error_message) {
    if (shader_id == nullptr) {
        if (error_message != nullptr) {
            *error_message = "shader output pointer is null";
        }
        return false;
    }

    *shader_id = glCreateShader(shader_type);
    if (*shader_id == 0) {
        if (error_message != nullptr) {
            *error_message = "glCreateShader failed";
        }
        return false;
    }

    const char *source_data = source.c_str();
    glShaderSource(*shader_id, 1, &source_data, nullptr);
    glCompileShader(*shader_id);

    GLint compile_status = GL_FALSE;
    glGetShaderiv(*shader_id, GL_COMPILE_STATUS, &compile_status);
    if (compile_status == GL_TRUE) {
        return true;
    }

    GLint log_length = 0;
    glGetShaderiv(*shader_id, GL_INFO_LOG_LENGTH, &log_length);
    std::vector<char> log_buffer(static_cast<std::size_t>(std::max(1, log_length)));
    glGetShaderInfoLog(*shader_id, log_length, nullptr, log_buffer.data());

    if (error_message != nullptr) {
        const std::string stage = shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment";
        *error_message = "failed to compile " + stage + " shader: " + TrimTrailingWhitespace(std::string(log_buffer.data()));
    }

    glDeleteShader(*shader_id);
    *shader_id = 0;
    return false;
}

bool CreateShaderProgram(
        GLuint *program_id,
        GLint *scale_uniform_location,
        GLint *color_mode_uniform_location,
        std::string *resolved_shader_path,
        std::string *error_message) {
    if (program_id == nullptr || scale_uniform_location == nullptr || color_mode_uniform_location == nullptr) {
        if (error_message != nullptr) {
            *error_message = "shader program output pointers are null";
        }
        return false;
    }

    std::string vertex_shader_source;
    std::string fragment_shader_source;
    if (!LoadShaderSources(&vertex_shader_source, &fragment_shader_source, resolved_shader_path, error_message)) {
        return false;
    }

    GLuint vertex_shader_id = 0;
    GLuint fragment_shader_id = 0;
    if (!CompileShader(GL_VERTEX_SHADER, vertex_shader_source, &vertex_shader_id, error_message)) {
        return false;
    }
    if (!CompileShader(GL_FRAGMENT_SHADER, fragment_shader_source, &fragment_shader_id, error_message)) {
        glDeleteShader(vertex_shader_id);
        return false;
    }

    *program_id = glCreateProgram();
    if (*program_id == 0) {
        if (error_message != nullptr) {
            *error_message = "glCreateProgram failed";
        }
        glDeleteShader(vertex_shader_id);
        glDeleteShader(fragment_shader_id);
        return false;
    }

    glAttachShader(*program_id, vertex_shader_id);
    glAttachShader(*program_id, fragment_shader_id);
    glLinkProgram(*program_id);

    glDeleteShader(vertex_shader_id);
    glDeleteShader(fragment_shader_id);

    GLint link_status = GL_FALSE;
    glGetProgramiv(*program_id, GL_LINK_STATUS, &link_status);
    if (link_status != GL_TRUE) {
        GLint log_length = 0;
        glGetProgramiv(*program_id, GL_INFO_LOG_LENGTH, &log_length);
        std::vector<char> log_buffer(static_cast<std::size_t>(std::max(1, log_length)));
        glGetProgramInfoLog(*program_id, log_length, nullptr, log_buffer.data());

        if (error_message != nullptr) {
            *error_message = "failed to link shader program: " + TrimTrailingWhitespace(std::string(log_buffer.data()));
        }

        glDeleteProgram(*program_id);
        *program_id = 0;
        return false;
    }

    glUseProgram(*program_id);
    glUniform1i(glGetUniformLocation(*program_id, "u_plane0"), 0);
    glUniform1i(glGetUniformLocation(*program_id, "u_plane1"), 1);
    glUniform1i(glGetUniformLocation(*program_id, "u_plane2"), 2);
    *scale_uniform_location = glGetUniformLocation(*program_id, "u_scale");
    *color_mode_uniform_location = glGetUniformLocation(*program_id, "u_color_mode");
    return true;
}

void ConfigureTexture(GLuint texture_id) {
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void UploadPlaneTexture(
        GLuint texture_id,
        int plane_index,
        int width,
        int height,
        const std::uint8_t *data,
        int linesize,
        GLint internal_format,
        GLenum format,
        GLuint (&pixel_unpack_buffer_ids)[2],
        std::size_t (&pixel_unpack_buffer_sizes)[2],
        std::size_t &next_pixel_unpack_buffer_index,
        int &cached_width,
        int &cached_height) {
    if (data == nullptr || width <= 0 || height <= 0 || linesize <= 0) {
        return;
    }

    const std::size_t bytes_per_pixel = format == GL_RG ? 2U : 1U;
    const std::size_t packed_row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
    const std::size_t buffer_size = packed_row_bytes * static_cast<std::size_t>(height);
    const std::size_t pbo_index = next_pixel_unpack_buffer_index % kPixelUploadBufferCount;
    const GLuint pbo_id = pixel_unpack_buffer_ids[pbo_index];

    glActiveTexture(GL_TEXTURE0 + plane_index);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    bool uploaded_with_pbo = false;
    if (pbo_id != 0 && buffer_size > 0) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_id);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(buffer_size), nullptr, GL_STREAM_DRAW);
        pixel_unpack_buffer_sizes[pbo_index] = buffer_size;

        void *mapped_buffer = glMapBufferRange(
                GL_PIXEL_UNPACK_BUFFER,
                0,
                static_cast<GLsizeiptr>(buffer_size),
                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (mapped_buffer != nullptr) {
            std::uint8_t *target = reinterpret_cast<std::uint8_t *>(mapped_buffer);
            for (int row = 0; row < height; ++row) {
                std::memcpy(
                        target + static_cast<std::size_t>(row) * packed_row_bytes,
                        data + static_cast<std::size_t>(row) * static_cast<std::size_t>(linesize),
                        packed_row_bytes);
            }

            if (glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_TRUE) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                if (cached_width != width || cached_height != height) {
                    cached_width = width;
                    cached_height = height;
                    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, nullptr);
                }
                uploaded_with_pbo = true;
            }
        }
    }

    if (!uploaded_with_pbo) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize / std::max<int>(1, static_cast<int>(bytes_per_pixel)));
        if (cached_width != width || cached_height != height) {
            cached_width = width;
            cached_height = height;
            glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
        }
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    next_pixel_unpack_buffer_index = (pbo_index + 1) % kPixelUploadBufferCount;
}

void InitializeImGuiStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.09f, 0.12f, 0.88f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.48f, 0.32f, 0.85f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.60f, 0.39f, 0.95f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.69f, 0.45f, 1.0f);
}

class OpenGlVideoRendererBackend final : public VideoRendererBackend {
public:
    ~OpenGlVideoRendererBackend() override {
        Shutdown();
    }

    bool Initialize(const std::string &window_title, bool enable_vsync, std::string *error_message) override {
        Shutdown();

        window_title_ = window_title;
        vsync_enabled_ = enable_vsync;
        hud_visible_ = true;
        h_key_was_down_ = false;
        last_render_timestamp_ns_ = 0;
        displayed_fps_ = 0.0;
        window_sized_to_frame_ = false;

        if (!glfwInit()) {
            if (error_message != nullptr) {
                *error_message = "glfwInit failed";
            }
            ResetState();
            return false;
        }
        glfw_initialized_ = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        window_ = glfwCreateWindow(640, 480, window_title_.c_str(), nullptr, nullptr);
        if (window_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "glfwCreateWindow failed for OpenGL 3.3 core profile";
            }
            Shutdown();
            return false;
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(vsync_enabled_ ? 1 : 0);
        glfwSetWindowUserPointer(window_, this);
        glfwSetWindowSizeCallback(window_, [](GLFWwindow *win, int width, int height) {
            auto *self = static_cast<OpenGlVideoRendererBackend *>(glfwGetWindowUserPointer(win));
            if (self != nullptr && !self->is_fullscreen_ && width > 0 && height > 0) {
                self->windowed_width_ = width;
                self->windowed_height_ = height;
            }
        });

        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            if (error_message != nullptr) {
                *error_message = "gladLoadGLLoader failed";
            }
            Shutdown();
            return false;
        }

        if (!CreateShaderProgram(
                    &shader_program_id_,
                    &scale_uniform_location_,
                    &color_mode_uniform_location_,
                    nullptr,
                    error_message)) {
            Shutdown();
            return false;
        }

        const float vertices[] = {
                -1.0f, -1.0f, 0.0f, 1.0f,
                 1.0f, -1.0f, 1.0f, 1.0f,
                 1.0f,  1.0f, 1.0f, 0.0f,
                -1.0f,  1.0f, 0.0f, 0.0f,
        };
        const unsigned int indices[] = {
                0, 1, 2,
                2, 3, 0,
        };

        glGenVertexArrays(1, &vertex_array_id_);
        glGenBuffers(1, &vertex_buffer_id_);
        glGenBuffers(1, &element_buffer_id_);
        glGenTextures(3, texture_ids_);
        glGenBuffers(6, &pixel_unpack_buffer_ids_[0][0]);
        ConfigureTexture(texture_ids_[0]);
        ConfigureTexture(texture_ids_[1]);
        ConfigureTexture(texture_ids_[2]);
        ResetPlaneDimensions();

        glBindVertexArray(vertex_array_id_);
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_id_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer_id_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        InitializeImGuiStyle();

        if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
            if (error_message != nullptr) {
                *error_message = "ImGui_ImplGlfw_InitForOpenGL failed";
            }
            Shutdown();
            return false;
        }
        if (!ImGui_ImplOpenGL3_Init("#version 330")) {
            if (error_message != nullptr) {
                *error_message = "ImGui_ImplOpenGL3_Init failed";
            }
            Shutdown();
            return false;
        }
        imgui_initialized_ = true;
        return true;
    }

    bool SupportsNativeFrame(const DecodedFrame &frame) const override {
        if (frame.empty()) {
            return false;
        }

        NativeColorMode mode = NativeColorMode::kBgr;
        return ResolveNativeColorMode(frame.pixel_format, &mode);
    }

    bool Render(const DecodedFrame &frame, const RenderFrameInfo &frame_info, std::string *error_message) override {
        if (frame.empty()) {
            if (error_message != nullptr) {
                *error_message = "renderer received an empty decoded frame";
            }
            return false;
        }
        if (window_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "native frame rendering requires the OpenGL backend";
            }
            return false;
        }

        NativeColorMode color_mode = NativeColorMode::kBgr;
        if (!ResolveNativeColorMode(frame.pixel_format, &color_mode)) {
            if (error_message != nullptr) {
                *error_message = "decoded frame format is not supported for native OpenGL rendering";
            }
            return false;
        }

        const std::uint64_t now_ns = MonotonicNowNs();
        if (last_render_timestamp_ns_ != 0 && now_ns > last_render_timestamp_ns_) {
            const double instantaneous_fps = 1000000000.0 / static_cast<double>(now_ns - last_render_timestamp_ns_);
            displayed_fps_ = displayed_fps_ == 0.0
                    ? instantaneous_fps
                    : displayed_fps_ * 0.90 + instantaneous_fps * 0.10;
        }
        last_render_timestamp_ns_ = now_ns;

        if (!window_sized_to_frame_) {
            glfwGetWindowSize(window_, &windowed_width_, &windowed_height_);
            window_sized_to_frame_ = true;
        }

        UploadFrameTextures(frame, color_mode);

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window_, &framebuffer_width, &framebuffer_height);
        glViewport(0, 0, framebuffer_width, framebuffer_height);
        glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        const double framebuffer_aspect = framebuffer_height > 0
                ? static_cast<double>(framebuffer_width) / static_cast<double>(framebuffer_height)
                : 1.0;
        const double image_aspect = frame.height > 0
                ? static_cast<double>(frame.width) / static_cast<double>(frame.height)
                : 1.0;

        float scale_x = 1.0f;
        float scale_y = 1.0f;
        if (image_aspect > framebuffer_aspect) {
            scale_y = static_cast<float>(framebuffer_aspect / image_aspect);
        } else {
            scale_x = static_cast<float>(image_aspect / framebuffer_aspect);
        }

        glUseProgram(shader_program_id_);
        glUniform2f(scale_uniform_location_, scale_x, scale_y);
        glUniform1i(color_mode_uniform_location_, static_cast<int>(color_mode));
        glBindVertexArray(vertex_array_id_);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        RenderFrameInfo hud_info = frame_info;
        hud_info.frame_width = frame.width;
        hud_info.frame_height = frame.height;
        RenderOpenGlHudPanel(hud_info, displayed_fps_, &hud_visible_);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
        has_rendered_frame_ = true;
        return true;
    }

    int PollKey(int delay_ms) override {
        if (window_ == nullptr) {
            return 27;
        }

        glfwPollEvents();
        if (glfwWindowShouldClose(window_)) {
            return 27;
        }

        const bool h_key_down = glfwGetKey(window_, GLFW_KEY_H) == GLFW_PRESS;
        if (h_key_down && !h_key_was_down_) {
            hud_visible_ = !hud_visible_;
        }
        h_key_was_down_ = h_key_down;

        if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            return 27;
        }
        if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) {
            return 'q';
        }

        const bool space_down = glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_down && !space_key_was_down_) {
            space_key_was_down_ = space_down;
            return ' ';
        }
        space_key_was_down_ = space_down;

        const bool f_key_down = glfwGetKey(window_, GLFW_KEY_F) == GLFW_PRESS;
        if (f_key_down && !f_key_was_down_) {
            f_key_was_down_ = f_key_down;
            return 'f';
        }
        f_key_was_down_ = f_key_down;

        const bool s_key_down = glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS;
        if (s_key_down && !s_key_was_down_) {
            s_key_was_down_ = s_key_down;
            return 's';
        }
        s_key_was_down_ = s_key_down;

        const bool r_key_down = glfwGetKey(window_, GLFW_KEY_R) == GLFW_PRESS;
        if (r_key_down && !r_key_was_down_) {
            r_key_was_down_ = r_key_down;
            return 'r';
        }
        r_key_was_down_ = r_key_down;

        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        return -1;
    }

    void UpdateWindowTitle(const std::string &title) override {
        if (window_ != nullptr) {
            glfwSetWindowTitle(window_, title.c_str());
        }
    }

    void ToggleFullscreen() override {
        if (window_ == nullptr) {
            return;
        }

        if (is_fullscreen_) {
            glfwSetWindowMonitor(window_, nullptr, windowed_x_, windowed_y_, windowed_width_, windowed_height_, 0);
            is_fullscreen_ = false;
        } else {
            glfwGetWindowPos(window_, &windowed_x_, &windowed_y_);
            glfwGetWindowSize(window_, &windowed_width_, &windowed_height_);
            GLFWmonitor *monitor = glfwGetPrimaryMonitor();
            if (monitor != nullptr) {
                const GLFWvidmode *mode = glfwGetVideoMode(monitor);
                if (mode != nullptr) {
                    glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
                    is_fullscreen_ = true;
                }
            }
        }
    }

    bool SaveScreenshot(const std::string &path, std::string *error_message) override {
        if (window_ == nullptr || !has_rendered_frame_) {
            if (error_message != nullptr) {
                *error_message = "no frame available for screenshot";
            }
            return false;
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        if (width <= 0 || height <= 0) {
            if (error_message != nullptr) {
                *error_message = "invalid framebuffer size for screenshot";
            }
            return false;
        }

        glfwMakeContextCurrent(window_);

        // Reset GL state to avoid PBO interference
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        const std::size_t row_bytes = static_cast<std::size_t>(width) * 3;
        std::vector<std::uint8_t> pixels(row_bytes * static_cast<std::size_t>(height));

        // Read bottom to top, then flip
        for (int y = 0; y < height; ++y) {
            glReadPixels(0, y, width, 1, GL_RGB, GL_UNSIGNED_BYTE,
                         pixels.data() + static_cast<std::size_t>(height - 1 - y) * row_bytes);
        }

        if (stbi_write_png(path.c_str(), width, height, 3, pixels.data(), static_cast<int>(row_bytes)) == 0) {
            if (error_message != nullptr) {
                *error_message = "failed to write screenshot: " + path;
            }
            return false;
        }

        common::log::Logger::Info(std::string("screenshot saved: ") + path);
        return true;
    }

    void Shutdown() override {
        if (window_ != nullptr) {
            glfwMakeContextCurrent(window_);

            if (imgui_initialized_) {
                ImGui_ImplOpenGL3_Shutdown();
                ImGui_ImplGlfw_Shutdown();
                ImGui::DestroyContext();
                imgui_initialized_ = false;
            }

            if (pixel_unpack_buffer_ids_[0][0] != 0 ||
                pixel_unpack_buffer_ids_[0][1] != 0 ||
                pixel_unpack_buffer_ids_[1][0] != 0 ||
                pixel_unpack_buffer_ids_[1][1] != 0 ||
                pixel_unpack_buffer_ids_[2][0] != 0 ||
                pixel_unpack_buffer_ids_[2][1] != 0) {
                glDeleteBuffers(6, &pixel_unpack_buffer_ids_[0][0]);
            }

            if (shader_program_id_ != 0) {
                glDeleteProgram(shader_program_id_);
            }
            if (element_buffer_id_ != 0) {
                glDeleteBuffers(1, &element_buffer_id_);
            }
            if (vertex_buffer_id_ != 0) {
                glDeleteBuffers(1, &vertex_buffer_id_);
            }
            if (vertex_array_id_ != 0) {
                glDeleteVertexArrays(1, &vertex_array_id_);
            }
            if (texture_ids_[0] != 0 || texture_ids_[1] != 0 || texture_ids_[2] != 0) {
                glDeleteTextures(3, texture_ids_);
            }

            glfwDestroyWindow(window_);
        }

        if (glfw_initialized_) {
            glfwTerminate();
        }

        ResetState();
    }

    RenderBackend backend() const override {
        return RenderBackend::kOpenGl;
    }

    const std::string &backend_name() const override {
        return backend_name_;
    }

private:
    void ResetPlaneDimensions() {
        for (int index = 0; index < 3; ++index) {
            texture_widths_[index] = 0;
            texture_heights_[index] = 0;
        }
    }

    void UploadFrameTextures(const DecodedFrame &frame, NativeColorMode color_mode) {
        if (color_mode == NativeColorMode::kYuv420p) {
            UploadPlaneTexture(
                    texture_ids_[0],
                    0,
                    frame.width,
                    frame.height,
                    frame.data[0],
                    frame.linesize[0],
                    GL_R8,
                    GL_RED,
                    pixel_unpack_buffer_ids_[0],
                    pixel_unpack_buffer_sizes_[0],
                    next_pixel_unpack_buffer_index_[0],
                    texture_widths_[0],
                    texture_heights_[0]);
            UploadPlaneTexture(
                    texture_ids_[1],
                    1,
                    std::max(1, frame.width / 2),
                    std::max(1, frame.height / 2),
                    frame.data[1],
                    frame.linesize[1],
                    GL_R8,
                    GL_RED,
                    pixel_unpack_buffer_ids_[1],
                    pixel_unpack_buffer_sizes_[1],
                    next_pixel_unpack_buffer_index_[1],
                    texture_widths_[1],
                    texture_heights_[1]);
            UploadPlaneTexture(
                    texture_ids_[2],
                    2,
                    std::max(1, frame.width / 2),
                    std::max(1, frame.height / 2),
                    frame.data[2],
                    frame.linesize[2],
                    GL_R8,
                    GL_RED,
                    pixel_unpack_buffer_ids_[2],
                    pixel_unpack_buffer_sizes_[2],
                    next_pixel_unpack_buffer_index_[2],
                    texture_widths_[2],
                    texture_heights_[2]);
            return;
        }

        UploadPlaneTexture(
                texture_ids_[0],
                0,
                frame.width,
                frame.height,
                frame.data[0],
                frame.linesize[0],
                GL_R8,
                GL_RED,
                pixel_unpack_buffer_ids_[0],
                pixel_unpack_buffer_sizes_[0],
                next_pixel_unpack_buffer_index_[0],
                texture_widths_[0],
                texture_heights_[0]);
        UploadPlaneTexture(
                texture_ids_[1],
                1,
                std::max(1, frame.width / 2),
                std::max(1, frame.height / 2),
                frame.data[1],
                frame.linesize[1],
                GL_RG8,
                GL_RG,
                pixel_unpack_buffer_ids_[1],
                pixel_unpack_buffer_sizes_[1],
                next_pixel_unpack_buffer_index_[1],
                texture_widths_[1],
                texture_heights_[1]);
    }

    void ResetState() {
        window_ = nullptr;
        for (int plane = 0; plane < 3; ++plane) {
            texture_ids_[plane] = 0;
            texture_widths_[plane] = 0;
            texture_heights_[plane] = 0;
            next_pixel_unpack_buffer_index_[plane] = 0;
            for (std::size_t slot = 0; slot < kPixelUploadBufferCount; ++slot) {
                pixel_unpack_buffer_ids_[plane][slot] = 0;
                pixel_unpack_buffer_sizes_[plane][slot] = 0;
            }
        }
        vertex_array_id_ = 0;
        vertex_buffer_id_ = 0;
        element_buffer_id_ = 0;
        shader_program_id_ = 0;
        scale_uniform_location_ = -1;
        color_mode_uniform_location_ = -1;
        glfw_initialized_ = false;
        imgui_initialized_ = false;
        window_sized_to_frame_ = false;
        window_title_.clear();
        vsync_enabled_ = false;
        hud_visible_ = true;
        h_key_was_down_ = false;
        space_key_was_down_ = false;
        f_key_was_down_ = false;
        s_key_was_down_ = false;
        r_key_was_down_ = false;
        has_rendered_frame_ = false;
        last_render_timestamp_ns_ = 0;
        displayed_fps_ = 0.0;
        is_fullscreen_ = false;
        windowed_x_ = 0;
        windowed_y_ = 0;
        windowed_width_ = 640;
        windowed_height_ = 480;
    }

    GLFWwindow *window_ = nullptr;
    GLuint texture_ids_[3] = {0, 0, 0};
    GLuint pixel_unpack_buffer_ids_[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    std::size_t pixel_unpack_buffer_sizes_[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    std::size_t next_pixel_unpack_buffer_index_[3] = {0, 0, 0};
    GLuint vertex_array_id_ = 0;
    GLuint vertex_buffer_id_ = 0;
    GLuint element_buffer_id_ = 0;
    GLuint shader_program_id_ = 0;
    GLint scale_uniform_location_ = -1;
    GLint color_mode_uniform_location_ = -1;
    int texture_widths_[3] = {0, 0, 0};
    int texture_heights_[3] = {0, 0, 0};
    bool glfw_initialized_ = false;
    bool imgui_initialized_ = false;
    bool window_sized_to_frame_ = false;
    std::string window_title_;
    const std::string backend_name_ = "opengl";
    bool vsync_enabled_ = false;
    bool hud_visible_ = true;
    bool h_key_was_down_ = false;
    bool space_key_was_down_ = false;
    bool f_key_was_down_ = false;
    bool s_key_was_down_ = false;
    bool r_key_was_down_ = false;
    bool has_rendered_frame_ = false;
    std::uint64_t last_render_timestamp_ns_ = 0;
    double displayed_fps_ = 0.0;
    bool is_fullscreen_ = false;
    int windowed_x_ = 0;
    int windowed_y_ = 0;
    int windowed_width_ = 640;
    int windowed_height_ = 480;
};

}  // namespace

std::unique_ptr<VideoRendererBackend> CreateOpenGlVideoRendererBackend() {
    return std::unique_ptr<VideoRendererBackend>(new OpenGlVideoRendererBackend());
}

}  // namespace sclient

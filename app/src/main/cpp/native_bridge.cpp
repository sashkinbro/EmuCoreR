// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
//
// EmuCoreR libretro frontend for the SwanStation PlayStation 1 core.
//
// The core is linked directly (libswanstation_libretro_android.so) and driven
// through the libretro API. This file owns:
//   * environment negotiation (options, directories, logging, AV info),
//   * software framebuffer presentation into an ANativeWindow,
//   * PCM capture from retro_audio_sample_batch + AAudio output,
//   * controller state forwarding into retro_input_state,
//   * save-state (retro_serialize) file IO.
//
// Hardware renderers (OpenGL ES / Vulkan) are negotiated through
// RETRO_ENVIRONMENT_SET_HW_RENDER. When the frontend cannot provide a context
// the core transparently falls back to its software renderer.

#include <jni.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libretro.h"

#define LOG_TAG "EmuCoreR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Core option keys understood by the SwanStation libretro core.
// ---------------------------------------------------------------------------
namespace option_key {
constexpr const char* kRenderer = "swanstation_GPU_Renderer";
constexpr const char* kAspectRatio = "swanstation_Display_AspectRatio";
constexpr const char* kBiosNtscJ = "swanstation_BIOS_PathNTSCJ";
constexpr const char* kBiosNtscU = "swanstation_BIOS_PathNTSCU";
constexpr const char* kBiosPal = "swanstation_BIOS_PathPAL";
}  // namespace option_key

// GTK/desktop style renderer integer used by the Kotlin layer.
namespace core_renderer {
constexpr int kSoftware = 0;
constexpr int kVulkan = 1;
constexpr int kOpenGl = 2;
}  // namespace core_renderer

namespace {

constexpr size_t kAudioRingCapacityFrames = 48000;  // ~1s at 48 kHz stereo.

// ---------------------------------------------------------------------------
// Frontend state. The libretro core is a process singleton, so this state is
// global and only accessed from the emulation thread plus short JNI calls.
// ---------------------------------------------------------------------------
struct FrontendState {
    std::mutex mutex;
    // Serialises retro_init/retro_deinit/retro_load_game. Never taken by the
    // environment callback, so core-initiated environment queries cannot
    // deadlock against a caller that owns [mutex].
    std::mutex core_mutex;

    // Directories handed to the core through the environment callback.
    std::string system_dir;
    std::string save_dir;
    std::string core_assets_dir;

    // Presentation surface.
    ANativeWindow* window = nullptr;
    int requested_renderer = core_renderer::kSoftware;
    int current_window_width = 0;
    int current_window_height = 0;
    // Bumped on every attach/detach so the frame thread can rebuild its EGL
    // surface on the correct thread.
    uint32_t window_generation = 0;

    // Core options (overrides only). GET_VARIABLE falls back to the core default
    // when a key is absent, which is what we want for untouched settings.
    std::unordered_map<std::string, std::string> options;
    // Set whenever an option changes after boot so the core re-reads them
    // through GET_VARIABLE_UPDATE on its next frame.
    std::atomic<bool> options_dirty{false};

    // Last frame geometry reported by the core.
    unsigned frame_width = 0;
    unsigned frame_height = 0;

    // Software frame format requested through SET_PIXEL_FORMAT.
    int pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

    // Audio ring buffer (interleaved stereo int16 frames).
    std::mutex audio_mutex;
    std::vector<int16_t> audio_ring;
    size_t audio_read_frame = 0;
    size_t audio_write_frame = 0;

    // Input: active-high bitmask per port plus analog axes.
    std::atomic<uint16_t> pad_buttons[2]{{0xFFFF}, {0xFFFF}};
    std::atomic<int16_t> pad_analog[2][4]{};  // lx, ly, rx, ry in -32768..32767
    std::atomic<bool> pad_analog_mode[2]{{false}, {false}};

    std::atomic<bool> core_initialized{false};
    std::atomic<bool> game_loaded{false};

    // Rumble latched from the core, read back by the UI.
    std::atomic<uint8_t> rumble_strong[2]{{0}, {0}};
    std::atomic<uint8_t> rumble_weak[2]{{0}, {0}};
};

FrontendState g_frontend;

// ---------------------------------------------------------------------------
// OpenGL ES hardware renderer state. Created and used exclusively on the frame
// worker thread, because EGL contexts are thread-affine.
// ---------------------------------------------------------------------------
struct GlRenderState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    retro_hw_render_callback* callback = nullptr;
    bool pending = false;
    bool ready = false;
    bool failed = false;
    uint32_t window_generation = 0;

    // Offscreen framebuffer the core renders into. The frontend then blits the
    // active region to the window, scaled and letterboxed.
    GLuint fbo = 0;
    GLuint fbo_texture = 0;
    GLuint fbo_depth = 0;
    GLsizei fbo_width = 0;
    GLsizei fbo_height = 0;
};

GlRenderState g_gl;

// True when the user selected the frontend "Stretch" (fill) mode. The
// SwanStation core does not understand this value and falls back to Auto, so
// the presenter honours it directly by disabling aspect correction.
bool AspectRatioStretchRequested() {
    std::lock_guard<std::mutex> lock(g_frontend.mutex);
    const auto it = g_frontend.options.find(option_key::kAspectRatio);
    return it != g_frontend.options.end() && it->second == "Stretch";
}

struct PresentRect {
    int x;
    int y;
    int width;
    int height;
};

// Aspect-corrected fit of a display of [display_aspect] inside [win_width] x
// [win_height], letterboxed or pillarboxed and centred.
PresentRect FitDisplayRect(int win_width, int win_height, double display_aspect) {
    if (display_aspect <= 0.0 || !std::isfinite(display_aspect)) display_aspect = 4.0 / 3.0;
    const double window_aspect = static_cast<double>(win_width) / static_cast<double>(win_height);
    int width;
    int height;
    if (window_aspect > display_aspect) {
        height = win_height;
        width = static_cast<int>(win_height * display_aspect + 0.5);
    } else {
        width = win_width;
        height = static_cast<int>(win_width / display_aspect + 0.5);
    }
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (width > win_width) width = win_width;
    if (height > win_height) height = win_height;
    PresentRect rect{};
    rect.x = (win_width - width) / 2;
    rect.y = (win_height - height) / 2;
    rect.width = width;
    rect.height = height;
    return rect;
}

bool CreatePresentFramebuffer() {
    retro_system_av_info info{};
    retro_get_system_av_info(&info);
    const GLsizei width = static_cast<GLsizei>(info.geometry.max_width > 0 ? info.geometry.max_width : 1024);
    const GLsizei height = static_cast<GLsizei>(info.geometry.max_height > 0 ? info.geometry.max_height : 512);
    if (g_gl.fbo != 0 && g_gl.fbo_width == width && g_gl.fbo_height == height) return true;

    if (g_gl.fbo != 0) { glDeleteFramebuffers(1, &g_gl.fbo); g_gl.fbo = 0; }
    if (g_gl.fbo_texture != 0) { glDeleteTextures(1, &g_gl.fbo_texture); g_gl.fbo_texture = 0; }
    if (g_gl.fbo_depth != 0) { glDeleteRenderbuffers(1, &g_gl.fbo_depth); g_gl.fbo_depth = 0; }

    glGenTextures(1, &g_gl.fbo_texture);
    glBindTexture(GL_TEXTURE_2D, g_gl.fbo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &g_gl.fbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_gl.fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    glGenFramebuffers(1, &g_gl.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gl.fbo_texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_gl.fbo_depth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("EGL: present framebuffer incomplete (0x%x)", status);
        return false;
    }
    g_gl.fbo_width = width;
    g_gl.fbo_height = height;
    retro_system_av_info avi{};
    retro_get_system_av_info(&avi);
    LOGI("Present framebuffer %dx%d (internal render %ux%u)", width, height,
         avi.geometry.base_width, avi.geometry.base_height);
    return true;
}

void PresentHardwareFrame() {
    if (g_gl.fbo == 0) return;
    ANativeWindow* window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_frontend.mutex);
        window = g_frontend.window;
    }
    if (window == nullptr) return;
    const int win_width = ANativeWindow_getWidth(window);
    const int win_height = ANativeWindow_getHeight(window);
    if (win_width <= 0 || win_height <= 0) return;

    retro_system_av_info info{};
    retro_get_system_av_info(&info);
    GLsizei src_width = static_cast<GLsizei>(info.geometry.base_width > 0 ? info.geometry.base_width : g_gl.fbo_width);
    GLsizei src_height = static_cast<GLsizei>(info.geometry.base_height > 0 ? info.geometry.base_height : g_gl.fbo_height);
    if (src_width > g_gl.fbo_width) src_width = g_gl.fbo_width;
    if (src_height > g_gl.fbo_height) src_height = g_gl.fbo_height;

    const PresentRect dst = AspectRatioStretchRequested()
        ? PresentRect{0, 0, win_width, win_height}
        : FitDisplayRect(win_width, win_height, info.geometry.aspect_ratio);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, win_width, win_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, src_width, src_height,
                      dst.x, dst.y, dst.x + dst.width, dst.y + dst.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool CreateEglContext(ANativeWindow* window) {
    if (g_gl.display == EGL_NO_DISPLAY) {
        g_gl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_gl.display == EGL_NO_DISPLAY || eglInitialize(g_gl.display, nullptr, nullptr) != EGL_TRUE) {
            LOGE("EGL: eglInitialize failed (0x%x)", eglGetError());
            return false;
        }
        const EGLint config_attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_NONE,
        };
        EGLint config_count = 0;
        if (eglChooseConfig(g_gl.display, config_attribs, &g_gl.config, 1, &config_count) != EGL_TRUE ||
            config_count < 1) {
            LOGE("EGL: eglChooseConfig failed (0x%x)", eglGetError());
            return false;
        }
        const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_gl.context = eglCreateContext(g_gl.display, g_gl.config, EGL_NO_CONTEXT, context_attribs);
        if (g_gl.context == EGL_NO_CONTEXT) {
            LOGE("EGL: eglCreateContext failed (0x%x)", eglGetError());
            return false;
        }
    }
    if (g_gl.surface != EGL_NO_SURFACE) {
        eglMakeCurrent(g_gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_gl.display, g_gl.surface);
        g_gl.surface = EGL_NO_SURFACE;
    }
    g_gl.surface = eglCreateWindowSurface(g_gl.display, g_gl.config, window, nullptr);
    if (g_gl.surface == EGL_NO_SURFACE) {
        LOGE("EGL: eglCreateWindowSurface failed (0x%x)", eglGetError());
        return false;
    }
    if (eglMakeCurrent(g_gl.display, g_gl.surface, g_gl.surface, g_gl.context) != EGL_TRUE) {
        LOGE("EGL: eglMakeCurrent failed (0x%x)", eglGetError());
        return false;
    }
    return true;
}

// Runs on the frame worker thread. Creates the EGL surface for the current
// window and tells the core its context is ready (which also rebuilds GL
// resources whenever the surface is recreated).
bool EnsureHardwareContext() {
    if (g_gl.callback == nullptr || g_gl.failed || g_gl.pending == false) return g_gl.ready;

    ANativeWindow* window = nullptr;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_frontend.mutex);
        window = g_frontend.window;
        generation = g_frontend.window_generation;
    }
    if (window == nullptr) return false;
    if (g_gl.ready && generation == g_gl.window_generation) return true;

    if (!CreateEglContext(window)) {
        g_gl.failed = true;
        return false;
    }
    // The core renders into an offscreen framebuffer; allocate it while the
    // context is current and before the core builds its resources.
    CreatePresentFramebuffer();
    g_gl.window_generation = generation;
    g_gl.ready = true;
    g_gl.pending = false;
    if (g_gl.callback->context_reset != nullptr) g_gl.callback->context_reset();
    return true;
}

void DestroyHardwareContext() {    if (g_gl.display != EGL_NO_DISPLAY) {
        if (g_gl.surface != EGL_NO_SURFACE && g_gl.context != EGL_NO_CONTEXT)
            eglMakeCurrent(g_gl.display, g_gl.surface, g_gl.surface, g_gl.context);
        if (g_gl.fbo != 0) glDeleteFramebuffers(1, &g_gl.fbo);
        if (g_gl.fbo_texture != 0) glDeleteTextures(1, &g_gl.fbo_texture);
        if (g_gl.fbo_depth != 0) glDeleteRenderbuffers(1, &g_gl.fbo_depth);
        eglMakeCurrent(g_gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_gl.surface != EGL_NO_SURFACE) eglDestroySurface(g_gl.display, g_gl.surface);
        if (g_gl.context != EGL_NO_CONTEXT) eglDestroyContext(g_gl.display, g_gl.context);
        eglTerminate(g_gl.display);
    }
    g_gl = GlRenderState{};
}

// ---------------------------------------------------------------------------
// Logging bridge.
// ---------------------------------------------------------------------------
void RetroLogCallback(enum retro_log_level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int priority = ANDROID_LOG_DEBUG;
    switch (level) {
        case RETRO_LOG_DEBUG: priority = ANDROID_LOG_DEBUG; break;
        case RETRO_LOG_INFO: priority = ANDROID_LOG_INFO; break;
        case RETRO_LOG_WARN: priority = ANDROID_LOG_WARN; break;
        case RETRO_LOG_ERROR: priority = ANDROID_LOG_ERROR; break;
        default: priority = ANDROID_LOG_DEBUG; break;
    }
    __android_log_vprint(priority, LOG_TAG "-Core", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Environment callback.
// ---------------------------------------------------------------------------
// The core changes geometry.max_width/max_height when the internal resolution
// scale changes and announces it through SET_SYSTEM_AV_INFO. The core queries
// get_current_framebuffer() every frame, so resizing our backing FBO here makes
// the new resolution take effect at runtime without a full context reset.
void ResizePresentFramebufferIfCurrent() {
    if (g_gl.display == EGL_NO_DISPLAY || g_gl.context == EGL_NO_CONTEXT) return;
    if (eglGetCurrentContext() != g_gl.context) return;
    CreatePresentFramebuffer();
}

bool HandleHardwareRender(void* data) {
    auto* cb = static_cast<retro_hw_render_callback*>(data);
    if (cb == nullptr) return false;

    // Only OpenGL ES is negotiated on Android. Vulkan/D3D requests are
    // rejected so the core keeps its software renderer until a frontend
    // Vulkan implementation exists.
    if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
        cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION) {
        LOGI("Rejecting hardware context type %u (software fallback)", cb->context_type);
        return false;
    }

    cb->get_proc_address = [](const char* name) -> void (*)() {
        return reinterpret_cast<void (*)()>(eglGetProcAddress(name));
    };
    cb->get_current_framebuffer = []() -> uintptr_t {
        return static_cast<uintptr_t>(g_gl.fbo);
    };
    cb->depth = true;
    cb->stencil = false;

    // The frame worker has not started yet at this point (SET_HW_RENDER fires
    // inside retro_load_game), so publishing here is safe.
    g_gl.callback = cb;
    g_gl.pending = true;
    g_gl.failed = false;
    LOGI("Accepted OpenGL ES hardware renderer request");
    return true;
}

bool EnvironmentCallback(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            if (data == nullptr) return false;
            const auto format = *static_cast<const enum retro_pixel_format*>(data);
            if (format == RETRO_PIXEL_FORMAT_0RGB1555 || format == RETRO_PIXEL_FORMAT_RGB565 ||
                format == RETRO_PIXEL_FORMAT_XRGB8888) {
                std::lock_guard<std::mutex> lock(g_frontend.mutex);
                g_frontend.pixel_format = static_cast<int>(format);
                return true;
            }
            return false;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* var = static_cast<retro_variable*>(data);
            if (var == nullptr || var->key == nullptr) return false;
            std::lock_guard<std::mutex> lock(g_frontend.mutex);
            const auto it = g_frontend.options.find(var->key);
            if (it == g_frontend.options.end()) return false;
            var->value = it->second.c_str();
            return true;
        }

        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;

        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: {
            if (data == nullptr) return false;
            *static_cast<unsigned*>(data) = 2;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            if (data == nullptr) return false;
            // Report "changed" exactly once after a runtime option write so the
            // core's UpdateSettings() re-reads the options, then clear it.
            *static_cast<bool*>(data) = g_frontend.options_dirty.exchange(false);
            return true;
        }

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            if (data == nullptr) return false;
            std::lock_guard<std::mutex> lock(g_frontend.mutex);
            *static_cast<const char**>(data) = g_frontend.system_dir.c_str();
            return true;
        }

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            if (data == nullptr) return false;
            std::lock_guard<std::mutex> lock(g_frontend.mutex);
            *static_cast<const char**>(data) = g_frontend.save_dir.c_str();
            return true;
        }

        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: {
            if (data == nullptr) return false;
            std::lock_guard<std::mutex> lock(g_frontend.mutex);
            *static_cast<const char**>(data) = g_frontend.core_assets_dir.c_str();
            return true;
        }

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            if (data == nullptr) return false;
            static_cast<retro_log_callback*>(data)->log = RetroLogCallback;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: {
            if (data == nullptr) return false;
            *static_cast<unsigned*>(data) = 1;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;

        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
            if (data == nullptr) return false;
            // bit0 = video, bit1 = audio.
            *static_cast<int*>(data) = 3;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            return HandleHardwareRender(data);

        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
            if (data == nullptr) return false;
            *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGLES3;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: {
            if (data == nullptr) return false;
            auto* rumble = static_cast<retro_rumble_interface*>(data);
            rumble->set_rumble_state = [](unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
                if (port > 1u) return false;
                if (effect == RETRO_RUMBLE_STRONG) {
                    g_frontend.rumble_strong[port].store(static_cast<uint8_t>(strength >> 8));
                } else {
                    g_frontend.rumble_weak[port].store(static_cast<uint8_t>(strength >> 8));
                }
                return true;
            };
            return true;
        }

        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            ResizePresentFramebufferIfCurrent();
            return true;

        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
            return true;

        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Video callback: convert + present the software framebuffer.
// ---------------------------------------------------------------------------
int32_t WindowWidth(ANativeWindow* window) {
    return window != nullptr ? ANativeWindow_getWidth(window) : 0;
}

int32_t WindowHeight(ANativeWindow* window) {
    return window != nullptr ? ANativeWindow_getHeight(window) : 0;
}

void PresentSoftwareFrame(const void* data, unsigned width, unsigned height, size_t pitch) {
    ANativeWindow* window = nullptr;
    int pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    {
        std::lock_guard<std::mutex> lock(g_frontend.mutex);
        window = g_frontend.window;
        pixel_format = g_frontend.pixel_format;
    }
    if (window == nullptr || data == nullptr || width == 0 || height == 0) return;

    const int32_t win_width = WindowWidth(window);
    const int32_t win_height = WindowHeight(window);
    if (win_width <= 0 || win_height <= 0) return;

    if (ANativeWindow_setBuffersGeometry(window, win_width, win_height, WINDOW_FORMAT_RGBA_8888) != 0) {
        return;
    }

    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(window, &buffer, nullptr) != 0) return;

    auto* dst = static_cast<uint32_t*>(buffer.bits);
    const int dst_stride = buffer.stride;
    const int dst_width = buffer.width;
    const int dst_height = buffer.height;

    // Aspect-preserving fit with letterboxing/pillarboxing. The core reports
    // the intended display aspect ratio separately from the raw pixel size.
    retro_system_av_info av_info{};
    retro_get_system_av_info(&av_info);
    const double display_aspect = AspectRatioStretchRequested()
        ? static_cast<double>(dst_width) / static_cast<double>(dst_height)
        : av_info.geometry.aspect_ratio;
    const PresentRect fit = FitDisplayRect(dst_width, dst_height, display_aspect);
    const int fit_w = fit.width;
    const int fit_h = fit.height;
    const int offset_x = fit.x;
    const int offset_y = fit.y;

    // Clear to black.
    for (int y = 0; y < dst_height; ++y) {
        uint32_t* row = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < dst_width; ++x) row[x] = 0xFF000000u;
    }

    const auto* src_bytes = static_cast<const uint8_t*>(data);
    for (int y = 0; y < fit_h; ++y) {
        const unsigned src_y = static_cast<unsigned>((static_cast<int64_t>(y) * height) / fit_h);
        uint32_t* dst_row = dst + static_cast<size_t>(offset_y + y) * dst_stride + offset_x;
        const uint8_t* src_row = src_bytes + static_cast<size_t>(src_y) * pitch;
        for (int x = 0; x < fit_w; ++x) {
            const unsigned src_x = static_cast<unsigned>((static_cast<int64_t>(x) * width) / fit_w);
            // WINDOW_FORMAT_RGBA_8888 stores bytes R,G,B,A, i.e. on a
            // little-endian host the packed word is A<<24 | B<<16 | G<<8 | R.
            uint32_t rgba = 0xFF000000u;
            if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
                const auto px = reinterpret_cast<const uint16_t*>(src_row)[src_x];
                const uint32_t r = ((px >> 11) & 0x1F) << 3;
                const uint32_t g = ((px >> 5) & 0x3F) << 2;
                const uint32_t b = (px & 0x1F) << 3;
                rgba |= (b << 16) | (g << 8) | r;
            } else if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
                const uint32_t px = reinterpret_cast<const uint32_t*>(src_row)[src_x];
                const uint32_t r = (px >> 16) & 0xFF;
                const uint32_t g = (px >> 8) & 0xFF;
                const uint32_t b = px & 0xFF;
                rgba |= (b << 16) | (g << 8) | r;
            } else {  // 0RGB1555
                const auto px = reinterpret_cast<const uint16_t*>(src_row)[src_x];
                const uint32_t r = ((px >> 10) & 0x1F) << 3;
                const uint32_t g = ((px >> 5) & 0x1F) << 3;
                const uint32_t b = (px & 0x1F) << 3;
                rgba |= (b << 16) | (g << 8) | r;
            }
            dst_row[x] = rgba;
        }
    }

    ANativeWindow_unlockAndPost(window);
}

void RetroVideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (data == nullptr) {
        if (g_gl.ready && g_gl.display != EGL_NO_DISPLAY && g_gl.surface != EGL_NO_SURFACE)
            eglSwapBuffers(g_gl.display, g_gl.surface);
        return;
    }
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        // Hardware path: the core rendered into the frontend framebuffer; blit
        // it to the window, then present.
        PresentHardwareFrame();
        if (g_gl.ready && g_gl.display != EGL_NO_DISPLAY && g_gl.surface != EGL_NO_SURFACE)
            eglSwapBuffers(g_gl.display, g_gl.surface);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_frontend.mutex);
        g_frontend.frame_width = width;
        g_frontend.frame_height = height;
    }
    PresentSoftwareFrame(data, width, height, pitch);
}

// ---------------------------------------------------------------------------
// Audio callback: append into the ring buffer consumed by runFrame().
// ---------------------------------------------------------------------------
void AudioRingEnsureCapacity(size_t additional_frames) {
    const size_t used = (g_frontend.audio_write_frame + kAudioRingCapacityFrames -
                         g_frontend.audio_read_frame) % kAudioRingCapacityFrames;
    if (used + additional_frames <= kAudioRingCapacityFrames) return;
    // Drop the oldest frames to make room rather than stalling the core.
    const size_t drop = std::min(used + additional_frames - kAudioRingCapacityFrames, used);
    g_frontend.audio_read_frame = (g_frontend.audio_read_frame + drop) % kAudioRingCapacityFrames;
}

size_t RetroAudioSampleBatch(const int16_t* data, size_t frames) {
    if (data == nullptr || frames == 0) return frames;
    std::lock_guard<std::mutex> lock(g_frontend.audio_mutex);
    AudioRingEnsureCapacity(frames);
    for (size_t i = 0; i < frames; ++i) {
        const size_t slot = (g_frontend.audio_write_frame + i) % kAudioRingCapacityFrames;
        g_frontend.audio_ring[slot * 2 + 0] = data[i * 2 + 0];
        g_frontend.audio_ring[slot * 2 + 1] = data[i * 2 + 1];
    }
    g_frontend.audio_write_frame = (g_frontend.audio_write_frame + frames) % kAudioRingCapacityFrames;
    return frames;
}

void RetroAudioSample(int16_t left, int16_t right) {
    const int16_t frame[2] = {left, right};
    RetroAudioSampleBatch(frame, 1);
}

// ---------------------------------------------------------------------------
// Input callbacks.
// ---------------------------------------------------------------------------
void RetroInputPoll() {
    // Kotlin pushes state asynchronously; nothing to sample here.
}

int16_t RetroInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port > 1u) return 0;
    switch (device) {
        case RETRO_DEVICE_JOYPAD: {
            if (index != 0) return 0;
            // Map libretro joypad ids onto the active-low PS1 bitmask the
            // Kotlin layer produces (0 = pressed).
            static const int8_t ps1_bit_for_id[16] = {
                14,  // B      -> Cross
                15,  // Y      -> Square
                0,   // SELECT -> Select
                3,   // START  -> Start
                4,   // UP     -> Up
                6,   // DOWN   -> Down
                7,   // LEFT   -> Left
                5,   // RIGHT  -> Right
                13,  // A      -> Circle
                12,  // X      -> Triangle
                10,  // L      -> L1
                11,  // R      -> R1
                8,   // L2     -> L2
                9,   // R2     -> R2
                1,   // L3     -> L3
                2,   // R3     -> R3
            };
            const uint16_t buttons = g_frontend.pad_buttons[port].load();
            uint16_t active = 0;
            for (unsigned b = 0; b < 16; ++b) {
                if ((buttons & (1u << ps1_bit_for_id[b])) == 0) active |= static_cast<uint16_t>(1u << b);
            }
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(active);
            if (id >= 16u) return 0;
            return (active & (1u << id)) != 0 ? 1 : 0;
        }
        case RETRO_DEVICE_ANALOG: {
            if (id == RETRO_DEVICE_ID_ANALOG_X) {
                const int axis = (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) ? 2 : 0;
                return g_frontend.pad_analog[port][axis].load();
            }
            if (id == RETRO_DEVICE_ID_ANALOG_Y) {
                const int axis = (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) ? 3 : 1;
                return g_frontend.pad_analog[port][axis].load();
            }
            return 0;
        }
        case RETRO_DEVICE_LIGHTGUN:
            return 0;
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// AAudio output. Kept compatible with the Kotlin NativeAudioPcmSink contract.
// ---------------------------------------------------------------------------
struct AudioOutput {
    AAudioStream* stream = nullptr;
    std::atomic<bool> started{false};
    std::atomic<int32_t> sample_rate{44100};
    std::atomic<int64_t> last_error{0};
    std::atomic<uint64_t> queued_frames{0};
    std::atomic<uint64_t> accepted_frames{0};
    std::atomic<uint64_t> callback_frames{0};
    std::atomic<uint64_t> silence_frames{0};
    std::atomic<int32_t> state{0};
    std::mutex mutex;
};

std::string RendererOptionForInteger(int renderer) {
    switch (renderer) {
        case core_renderer::kVulkan:
            return "Vulkan";
        case core_renderer::kOpenGl:
            return "OpenGL";
        case core_renderer::kSoftware:
        default:
            return "Software";
    }
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// JNI: lifecycle.
// ---------------------------------------------------------------------------
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    LOGI("SwanStation libretro frontend loaded (api=%u)", RETRO_API_VERSION);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_nativeInit(JNIEnv* env, jobject, jstring systemDir,
                                                         jstring saveDir, jstring coreAssetsDir) {
    std::lock_guard<std::mutex> lock(g_frontend.mutex);
    auto to_string = [&](jstring value) -> std::string {
        if (value == nullptr) return {};
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string result = chars != nullptr ? chars : "";
        if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
        return result;
    };
    g_frontend.system_dir = to_string(systemDir);
    g_frontend.save_dir = to_string(saveDir);
    g_frontend.core_assets_dir = to_string(coreAssetsDir);
    if (g_frontend.audio_ring.empty()) {
        g_frontend.audio_ring.assign(kAudioRingCapacityFrames * 2, 0);
    }
    LOGI("nativeInit system=%s save=%s assets=%s", g_frontend.system_dir.c_str(),
         g_frontend.save_dir.c_str(), g_frontend.core_assets_dir.c_str());
}

JNIEXPORT jlong JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_createSession(JNIEnv*, jobject) {
    {
        std::lock_guard<std::mutex> lock(g_frontend.core_mutex);
        if (!g_frontend.core_initialized) {
            retro_set_environment(EnvironmentCallback);
            retro_set_video_refresh(RetroVideoRefresh);
            retro_set_audio_sample(RetroAudioSample);
            retro_set_audio_sample_batch(RetroAudioSampleBatch);
            retro_set_input_poll(RetroInputPoll);
            retro_set_input_state(RetroInputState);
            retro_init();
            g_frontend.core_initialized = true;
        }
    }
    // Default both controller ports to a Digital Controller; Kotlin promotes
    // a port to an analog DualShock when the game requests it.
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    // Non-zero opaque handle. The core itself is a singleton.
    return reinterpret_cast<jlong>(&g_frontend);
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_destroySession(JNIEnv*, jobject, jlong handle) {
    if (handle == 0) return;
    std::lock_guard<std::mutex> lock(g_frontend.core_mutex);
    if (g_frontend.game_loaded) {
        retro_unload_game();
        g_frontend.game_loaded = false;
    }
    if (g_frontend.core_initialized) {
        retro_deinit();
        g_frontend.core_initialized = false;
    }
    DestroyHardwareContext();
}

// ---------------------------------------------------------------------------
// JNI: configuration.
// ---------------------------------------------------------------------------
JNIEXPORT void JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_nativeSetOption(JNIEnv* env, jobject, jstring key,
                                                              jstring value) {
    if (key == nullptr || value == nullptr) return;
    const char* key_chars = env->GetStringUTFChars(key, nullptr);
    const char* value_chars = env->GetStringUTFChars(value, nullptr);
    if (key_chars != nullptr && value_chars != nullptr) {
        std::lock_guard<std::mutex> lock(g_frontend.mutex);
        g_frontend.options[key_chars] = value_chars;
        g_frontend.options_dirty.store(true);
    }
    if (value_chars != nullptr) env->ReleaseStringUTFChars(value, value_chars);
    if (key_chars != nullptr) env->ReleaseStringUTFChars(key, key_chars);
}

JNIEXPORT jstring JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_nativeGetOption(JNIEnv* env, jobject, jstring key) {
    if (key == nullptr) return nullptr;
    const char* key_chars = env->GetStringUTFChars(key, nullptr);
    std::string value;
    bool found = false;
    if (key_chars != nullptr) {
        std::lock_guard<std::mutex> lock(g_frontend.mutex);
        const auto it = g_frontend.options.find(key_chars);
        if (it != g_frontend.options.end()) {
            value = it->second;
            found = true;
        }
    }
    if (key_chars != nullptr) env->ReleaseStringUTFChars(key, key_chars);
    return found ? env->NewStringUTF(value.c_str()) : nullptr;
}

// BIOS is handled by the core's directory scanner: the Kotlin layer places the
// selected image in the system directory before calling this.
JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_loadBios(JNIEnv* env, jobject, jlong handle,
                                                       jstring path) {
    if (handle == 0 || path == nullptr) return -1;
    const char* chars = env->GetStringUTFChars(path, nullptr);
    const std::string bios_path = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(path, chars);
    if (bios_path.empty()) return -2;
    std::lock_guard<std::mutex> lock(g_frontend.mutex);
    // Expose the exact file to the core's BIOS auto-detection by pointing all
    // three region options at the same basename within the system directory.
    const size_t slash = bios_path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? bios_path : bios_path.substr(slash + 1);
    g_frontend.options[option_key::kBiosNtscU] = name;
    g_frontend.options[option_key::kBiosNtscJ] = name;
    g_frontend.options[option_key::kBiosPal] = name;
    LOGI("BIOS configured: %s", bios_path.c_str());
    return 0;
}

// Boots the core with no content. SwanStation treats an empty/none path as a
// BIOS-only boot (System::Boot handles an empty filename), which creates the
// GPU and display so retro_run() is safe. Without this the frame loop runs
// before retro_load_game and segfaults on the null GPU.
JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_loadBiosOnly(JNIEnv*, jobject, jlong handle) {
    if (handle == 0) return -1;
    std::lock_guard<std::mutex> lock(g_frontend.core_mutex);
    if (g_frontend.game_loaded) {
        retro_unload_game();
        g_frontend.game_loaded = false;
    }
    retro_game_info info{};
    info.path = nullptr;
    info.data = nullptr;
    info.size = 0;
    info.meta = nullptr;
    if (!retro_load_game(&info)) {
        LOGE("retro_load_game (BIOS only) failed");
        return -3;
    }
    g_frontend.game_loaded = true;
    LOGI("BIOS-only boot ok");
    return 0;
}

// ---------------------------------------------------------------------------
// JNI: content.
// ---------------------------------------------------------------------------
JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_loadDisc(JNIEnv* env, jobject, jlong handle,
                                                       jstring path) {
    if (handle == 0 || path == nullptr) return -1;
    const char* chars = env->GetStringUTFChars(path, nullptr);
    const std::string game_path = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(path, chars);
    if (game_path.empty()) return -2;

    std::lock_guard<std::mutex> lock(g_frontend.core_mutex);
    if (g_frontend.game_loaded) {
        retro_unload_game();
        g_frontend.game_loaded = false;
    }
    retro_game_info info{};
    info.path = game_path.c_str();
    info.data = nullptr;
    info.size = 0;
    info.meta = nullptr;
    if (!retro_load_game(&info)) {
        LOGE("retro_load_game failed for %s", game_path.c_str());
        return -3;
    }
    g_frontend.game_loaded = true;
    LOGI("retro_load_game ok: %s", game_path.c_str());
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_loadDiscFd(JNIEnv*, jobject, jlong handle, jint fd,
                                                         jlong offset, jlong size) {
    if (handle == 0 || fd < 0) return -1;
    (void)offset;
    (void)size;
    // /proc/self/fd/<fd> stays valid as long as the caller keeps the descriptor
    // open, which the Kotlin layer guarantees for the duration of the session.
    const std::string game_path = "/proc/self/fd/" + std::to_string(fd);
    std::lock_guard<std::mutex> lock(g_frontend.core_mutex);
    if (g_frontend.game_loaded) {
        retro_unload_game();
        g_frontend.game_loaded = false;
    }
    retro_game_info info{};
    info.path = game_path.c_str();
    if (!retro_load_game(&info)) {
        LOGE("retro_load_game failed for fd %d (%s)", fd, game_path.c_str());
        return -3;
    }
    g_frontend.game_loaded = true;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_reset(JNIEnv*, jobject, jlong handle) {
    if (handle == 0) return -1;
    retro_reset();
    return 0;
}

// ---------------------------------------------------------------------------
// JNI: per-frame execution.
// ---------------------------------------------------------------------------
JNIEXPORT jshortArray JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_runFrame(JNIEnv* env, jobject, jlong handle) {
    if (handle == 0) return nullptr;
    EnsureHardwareContext();
    retro_run();

    std::lock_guard<std::mutex> lock(g_frontend.audio_mutex);
    size_t frames = (g_frontend.audio_write_frame + kAudioRingCapacityFrames -
                     g_frontend.audio_read_frame) % kAudioRingCapacityFrames;
    if (frames == 0) return nullptr;
    if (frames > 8192) frames = 8192;
    jshortArray result = env->NewShortArray(static_cast<jsize>(frames * 2));
    if (result == nullptr) return nullptr;
    // The ring may wrap; copy in up to two linear spans.
    std::vector<int16_t> linear(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const size_t slot = (g_frontend.audio_read_frame + i) % kAudioRingCapacityFrames;
        linear[i * 2 + 0] = g_frontend.audio_ring[slot * 2 + 0];
        linear[i * 2 + 1] = g_frontend.audio_ring[slot * 2 + 1];
    }
    g_frontend.audio_read_frame = (g_frontend.audio_read_frame + frames) % kAudioRingCapacityFrames;
    env->SetShortArrayRegion(result, 0, static_cast<jsize>(frames * 2),
                             reinterpret_cast<const jshort*>(linear.data()));
    return result;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_setSurface(JNIEnv* env, jobject, jlong handle,
                                                         jobject surface, jint renderer) {
    if (handle == 0) return -1;
    std::lock_guard<std::mutex> lock(g_frontend.mutex);
    if (g_frontend.window != nullptr) {
        ANativeWindow_release(g_frontend.window);
        g_frontend.window = nullptr;
    }
    if (surface != nullptr) {
        g_frontend.window = ANativeWindow_fromSurface(env, surface);
    }
    g_frontend.window_generation++;
    g_frontend.requested_renderer = renderer;
    g_frontend.options[option_key::kRenderer] = RendererOptionForInteger(renderer);
    LOGI("setSurface renderer=%d window=%s", renderer,
         g_frontend.window != nullptr ? "attached" : "detached");
    return 0;
}

// ---------------------------------------------------------------------------
// JNI: input.
// ---------------------------------------------------------------------------
JNIEXPORT void JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_setPadButtons(JNIEnv*, jobject, jlong handle,
                                                            jint port, jint buttons) {
    if (handle == 0 || port < 0 || port > 1) return;
    g_frontend.pad_buttons[port].store(static_cast<uint16_t>(buttons & 0xFFFF));
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_setPadAnalog(JNIEnv*, jobject, jlong handle, jint port,
                                                           jint lx, jint ly, jint rx, jint ry) {
    if (handle == 0 || port < 0 || port > 1) return;
    auto to_axis = [](jint value) -> int16_t {
        const int centered = static_cast<int>(value) - 128;
        int scaled = centered * 256;
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        return static_cast<int16_t>(scaled);
    };
    g_frontend.pad_analog[port][0].store(to_axis(lx));
    g_frontend.pad_analog[port][1].store(to_axis(ly));
    g_frontend.pad_analog[port][2].store(to_axis(rx));
    g_frontend.pad_analog[port][3].store(to_axis(ry));
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_setPadAnalogMode(JNIEnv*, jobject, jlong handle,
                                                               jint port, jboolean enabled) {
    if (handle == 0 || port < 0 || port > 1) return;
    g_frontend.pad_analog_mode[port].store(enabled == JNI_TRUE);
    retro_set_controller_port_device(static_cast<unsigned>(port),
                                     enabled == JNI_TRUE ? RETRO_DEVICE_ANALOG : RETRO_DEVICE_JOYPAD);
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_getPadState(JNIEnv*, jobject, jlong handle, jint port) {
    if (handle == 0 || port < 0 || port > 1) return -1;
    const int analog = g_frontend.pad_analog_mode[port].load() ? 1 : 0;
    const int large = g_frontend.rumble_strong[port].load();
    const int small = g_frontend.rumble_weak[port].load();
    return (analog << 16) | (large << 8) | small;
}

// ---------------------------------------------------------------------------
// JNI: save states (libretro serialization).
// ---------------------------------------------------------------------------
JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_saveState(JNIEnv* env, jobject, jlong handle,
                                                        jstring path) {
    if (handle == 0 || path == nullptr) return -1;
    const size_t size = retro_serialize_size();
    if (size == 0) return -2;
    std::vector<uint8_t> buffer(size);
    if (!retro_serialize(buffer.data(), buffer.size())) return -3;
    const char* chars = env->GetStringUTFChars(path, nullptr);
    int result = -4;
    if (chars != nullptr) {
        FILE* file = fopen(chars, "wb");
        if (file != nullptr) {
            const size_t written = fwrite(buffer.data(), 1, buffer.size(), file);
            fclose(file);
            result = written == buffer.size() ? 0 : -5;
        }
        env->ReleaseStringUTFChars(path, chars);
    }
    return result;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_loadState(JNIEnv* env, jobject, jlong handle,
                                                        jstring path) {
    if (handle == 0 || path == nullptr) return -1;
    const char* chars = env->GetStringUTFChars(path, nullptr);
    int result = -2;
    if (chars != nullptr) {
        FILE* file = fopen(chars, "rb");
        if (file != nullptr) {
            fseek(file, 0, SEEK_END);
            const long length = ftell(file);
            fseek(file, 0, SEEK_SET);
            if (length > 0) {
                std::vector<uint8_t> buffer(static_cast<size_t>(length));
                const size_t read = fread(buffer.data(), 1, buffer.size(), file);
                if (read == buffer.size() && retro_unserialize(buffer.data(), buffer.size())) {
                    result = 0;
                } else {
                    result = -3;
                }
            }
            fclose(file);
        }
        env->ReleaseStringUTFChars(path, chars);
    }
    return result;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_createMemoryCard(JNIEnv* env, jobject, jstring path) {
    if (path == nullptr) return -1;
    const char* chars = env->GetStringUTFChars(path, nullptr);
    int result = -2;
    if (chars != nullptr) {
        FILE* file = fopen(chars, "rb");
        if (file != nullptr) {
            fclose(file);
            result = 0;
        } else {
            file = fopen(chars, "wb");
            if (file != nullptr) {
                std::vector<uint8_t> blank(128 * 1024, 0);
                fwrite(blank.data(), 1, blank.size(), file);
                fclose(file);
                result = 0;
            }
        }
        env->ReleaseStringUTFChars(path, chars);
    }
    return result;
}

// ---------------------------------------------------------------------------
// JNI: diagnostics.
// ---------------------------------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_getSystemInfo(JNIEnv* env, jobject) {
    retro_system_info info{};
    retro_get_system_info(&info);
    std::string name = info.library_name != nullptr ? info.library_name : "SwanStation";
    // AGP's debug native build defines _DEBUG, which the core surfaces as
    // "SwanStation Debug"/"SwanStation DebugFast". Present the stable name.
    const size_t debug_suffix = name.find(" Debug");
    if (debug_suffix != std::string::npos) name.erase(debug_suffix);
    std::string text = name;
    text += " ";
    text += info.library_version != nullptr ? info.library_version : "unknown";
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_getDiagnostics(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_frontend.mutex);
    char buffer[512];
    snprintf(buffer, sizeof(buffer),
             "{\"core\":\"SwanStation\",\"initialized\":%d,\"game\":%d,\"renderer\":%d,\"w\":%u,\"h\":%u}",
             g_frontend.core_initialized ? 1 : 0, g_frontend.game_loaded ? 1 : 0,
             g_frontend.requested_renderer, g_frontend.frame_width, g_frontend.frame_height);
    return env->NewStringUTF(buffer);
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_apiVersion(JNIEnv*, jobject) {
    return static_cast<jint>(RETRO_API_VERSION);
}

JNIEXPORT jintArray JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_getDisplayRect(JNIEnv* env, jobject, jlong handle) {
    if (handle == 0) return nullptr;
    retro_system_av_info info{};
    retro_get_system_av_info(&info);
    std::lock_guard<std::mutex> lock(g_frontend.mutex);
    const unsigned width = g_frontend.frame_width != 0 ? g_frontend.frame_width : info.geometry.base_width;
    const unsigned height = g_frontend.frame_height != 0 ? g_frontend.frame_height : info.geometry.base_height;
    jintArray result = env->NewIntArray(4);
    if (result == nullptr) return nullptr;
    const jint values[4] = {0, 0, static_cast<jint>(width), static_cast<jint>(height)};
    env->SetIntArrayRegion(result, 0, 4, values);
    return result;
}

JNIEXPORT jlongArray JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_getAvInfo(JNIEnv* env, jobject, jlong handle) {
    if (handle == 0) return nullptr;
    retro_system_av_info info{};
    retro_get_system_av_info(&info);
    const jlong values[6] = {
        static_cast<jlong>(info.geometry.base_width),
        static_cast<jlong>(info.geometry.base_height),
        static_cast<jlong>(info.geometry.max_width),
        static_cast<jlong>(info.geometry.max_height),
        static_cast<jlong>(info.timing.fps),
        static_cast<jlong>(info.timing.sample_rate),
    };
    jlongArray result = env->NewLongArray(6);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, 6, values);
    return result;
}

// ---------------------------------------------------------------------------
// JNI: AAudio output (NativeAudioPcmSink contract).
// ---------------------------------------------------------------------------
JNIEXPORT jlong JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_createAudioOutput(JNIEnv*, jobject) {
    auto* output = new AudioOutput();
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        delete output;
        return 0;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, 44100);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &output->stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK || output->stream == nullptr) {
        LOGE("AAudio open failed: %d", opened);
        delete output;
        return 0;
    }
    output->sample_rate.store(AAudioStream_getSampleRate(output->stream));
    return reinterpret_cast<jlong>(output);
}

JNIEXPORT void JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_destroyAudioOutput(JNIEnv*, jobject, jlong handle) {
    auto* output = reinterpret_cast<AudioOutput*>(handle);
    if (output == nullptr) return;
    std::lock_guard<std::mutex> lock(output->mutex);
    if (output->stream != nullptr) {
        AAudioStream_requestStop(output->stream);
        AAudioStream_close(output->stream);
        output->stream = nullptr;
    }
    delete output;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_startAudioOutput(JNIEnv*, jobject, jlong handle) {
    auto* output = reinterpret_cast<AudioOutput*>(handle);
    if (output == nullptr || output->stream == nullptr) return -1;
    const aaudio_result_t result = AAudioStream_requestStart(output->stream);
    output->started.store(result == AAUDIO_OK);
    output->state.store(result == AAUDIO_OK ? 1 : 0);
    return result == AAUDIO_OK ? 0 : -2;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_pauseAudioOutput(JNIEnv*, jobject, jlong handle) {
    auto* output = reinterpret_cast<AudioOutput*>(handle);
    if (output == nullptr || output->stream == nullptr) return -1;
    const aaudio_result_t result = AAudioStream_requestPause(output->stream);
    output->started.store(false);
    output->state.store(result == AAUDIO_OK ? 2 : 0);
    return result == AAUDIO_OK ? 0 : -2;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_flushAudioOutput(JNIEnv*, jobject, jlong handle) {
    auto* output = reinterpret_cast<AudioOutput*>(handle);
    if (output == nullptr || output->stream == nullptr) return -1;
    const aaudio_result_t result = AAudioStream_requestFlush(output->stream);
    output->queued_frames.store(0);
    return result == AAUDIO_OK ? 0 : -2;
}

JNIEXPORT jint JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_writeAudioOutput(JNIEnv* env, jobject, jlong handle,
                                                              jshortArray data, jint offset,
                                                              jint count) {
    auto* output = reinterpret_cast<AudioOutput*>(handle);
    if (output == nullptr || output->stream == nullptr || data == nullptr) return -1;
    if (offset < 0 || count < 0 || (count & 1) != 0) return -1;
    const jsize length = env->GetArrayLength(data);
    if (count > length || offset > length - count) return -1;
    jshort* samples = env->GetShortArrayElements(data, nullptr);
    if (samples == nullptr) return -1;
    const int32_t frames = count / 2;
    const aaudio_result_t result =
        AAudioStream_write(output->stream, samples + offset, frames, 100000000LL /* 100 ms */);
    env->ReleaseShortArrayElements(data, samples, JNI_ABORT);
    if (result < 0) {
        output->last_error.store(result);
        return -3;
    }
    output->accepted_frames.fetch_add(static_cast<uint64_t>(result));
    output->queued_frames.store(0);
    return static_cast<jint>(result * 2);
}

JNIEXPORT jlongArray JNICALL
Java_com_sbro_emucorer_core_NativeCoreBridge_audioOutputStats(JNIEnv* env, jobject, jlong handle) {
    auto* output = reinterpret_cast<AudioOutput*>(handle);
    if (output == nullptr) return nullptr;
    const jlong values[8] = {
        static_cast<jlong>(output->state.load()),
        output->last_error.load(),
        static_cast<jlong>(output->sample_rate.load()),
        0,
        static_cast<jlong>(output->queued_frames.load()),
        static_cast<jlong>(output->accepted_frames.load()),
        static_cast<jlong>(output->callback_frames.load()),
        static_cast<jlong>(output->silence_frames.load()),
    };
    jlongArray result = env->NewLongArray(8);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, 8, values);
    return result;
}

}  // extern "C"

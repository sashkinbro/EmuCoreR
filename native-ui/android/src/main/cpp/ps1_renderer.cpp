#include "ps1_renderer.h"
#include "ps1_shaders.h"

#include <android/log.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

#define LOG_TAG "Ps1Renderer"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace ps1ui {

namespace {

constexpr size_t kMaxTextCache = 256;
constexpr size_t kMaxImageCache = 48;
constexpr int kSettingsCount = 15;

float Clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

float SmoothStep(float a, float b, float x) {
    float t = Clamp01((x - a) / std::max(0.0001f, b - a));
    return t * t * (3.0f - 2.0f * t);
}

float EaseOutCubic(float t) {
    t = Clamp01(t);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

std::string MakeTextCacheKey(
    const std::string& text,
    float fontSize,
    int style,
    float maxWidth,
    int maxLines,
    int align) {
    char buf[96];
    std::snprintf(
        buf,
        sizeof(buf),
        "|%.1f|%d|%.0f|%d|%d",
        fontSize,
        style,
        maxWidth,
        maxLines,
        align);
    return text + buf;
}

} // namespace

bool Ps1Renderer::Init() {
    if (gl_ready_) {
        Shutdown();
    }
    if (!CompileShaders()) {
        LOGE("Failed to compile PS1 UI shaders");
        return false;
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, 4096 * sizeof(Vertex), nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(4 * sizeof(float)));

    const float fsQuad[8] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    glGenVertexArrays(1, &fs_vao_);
    glGenBuffers(1, &fs_vbo_);
    glBindVertexArray(fs_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, fs_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fsQuad), fsQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

    const uint8_t whitePx[4] = {255, 255, 255, 255};
    glGenTextures(1, &white_tex_);
    glBindTexture(GL_TEXTURE_2D, white_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePx);

    has_frame_tp_ = false;
    gl_ready_ = true;
    LOGI("PS1 native UI GLES3 renderer initialized");
    return true;
}

void Ps1Renderer::Shutdown() {
    ClearTextCache();
    ClearImageCache();
    if (white_tex_) {
        glDeleteTextures(1, &white_tex_);
        white_tex_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (fs_vbo_) {
        glDeleteBuffers(1, &fs_vbo_);
        fs_vbo_ = 0;
    }
    if (fs_vao_) {
        glDeleteVertexArrays(1, &fs_vao_);
        fs_vao_ = 0;
    }
    auto delProg = [](GLuint& p) {
        if (p) {
            glDeleteProgram(p);
            p = 0;
        }
    };
    delProg(prog_quad_);
    delProg(prog_bg_);
    delProg(prog_graffiti_);
    delProg(prog_panel_);
    delProg(prog_disc_);
    delProg(prog_scan_);
    gl_ready_ = false;
}

GLuint Ps1Renderer::CompileProgram(const char* vsSrc, const char* fsSrc) {
    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char logBuf[1024] = {};
            glGetShaderInfoLog(s, sizeof(logBuf), nullptr, logBuf);
            LOGE("Shader compile error: %s", logBuf);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char logBuf[1024] = {};
        glGetProgramInfoLog(prog, sizeof(logBuf), nullptr, logBuf);
        LOGE("Program link error: %s", logBuf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool Ps1Renderer::CompileShaders() {
    prog_quad_ = CompileProgram(kQuadVertexShader, kQuadFragmentShader);
    prog_bg_ = CompileProgram(kBgVertexShader, kBgFragmentShader);
    prog_graffiti_ = CompileProgram(kGraffitiVertexShader, kGraffitiFragmentShader);
    prog_panel_ = CompileProgram(kPanelVertexShader, kPanelFragmentShader);
    prog_disc_ = CompileProgram(kDiscVertexShader, kDiscFragmentShader);
    prog_scan_ = CompileProgram(kBgVertexShader, kScanlineFragmentShader);

    if (!prog_quad_ || !prog_bg_ || !prog_graffiti_ || !prog_panel_ || !prog_disc_ || !prog_scan_) {
        return false;
    }

    quad_u_screen_ = glGetUniformLocation(prog_quad_, "u_screen");
    quad_u_texture_ = glGetUniformLocation(prog_quad_, "u_texture");

    bg_u_screen_ = glGetUniformLocation(prog_bg_, "u_screen");
    bg_u_time_ = glGetUniformLocation(prog_bg_, "u_time");
    bg_u_motion_ = glGetUniformLocation(prog_bg_, "u_motion_scale");
    bg_u_theme_ = glGetUniformLocation(prog_bg_, "u_theme");
    bg_u_parallax_ = glGetUniformLocation(prog_bg_, "u_parallax");

    graf_u_screen_ = glGetUniformLocation(prog_graffiti_, "u_screen");
    graf_u_seed_ = glGetUniformLocation(prog_graffiti_, "u_seed");
    graf_u_focus_ = glGetUniformLocation(prog_graffiti_, "u_focus");
    graf_u_alpha_ = glGetUniformLocation(prog_graffiti_, "u_alpha");
    graf_u_time_ = glGetUniformLocation(prog_graffiti_, "u_time");
    graf_u_theme_ = glGetUniformLocation(prog_graffiti_, "u_theme");

    pan_u_screen_ = glGetUniformLocation(prog_panel_, "u_screen");
    pan_u_size_ = glGetUniformLocation(prog_panel_, "u_size");
    pan_u_radius_ = glGetUniformLocation(prog_panel_, "u_radius");
    pan_u_border_px_ = glGetUniformLocation(prog_panel_, "u_border_px");
    pan_u_top_col_ = glGetUniformLocation(prog_panel_, "u_top_col");
    pan_u_bot_col_ = glGetUniformLocation(prog_panel_, "u_bot_col");
    pan_u_border_col_ = glGetUniformLocation(prog_panel_, "u_border_col");
    pan_u_glow_col_ = glGetUniformLocation(prog_panel_, "u_glow_col");

    disc_u_screen_ = glGetUniformLocation(prog_disc_, "u_screen");
    disc_u_angle_ = glGetUniformLocation(prog_disc_, "u_angle");
    disc_u_alpha_ = glGetUniformLocation(prog_disc_, "u_alpha");
    disc_u_theme_ = glGetUniformLocation(prog_disc_, "u_theme");

    scan_u_viewport_ = glGetUniformLocation(prog_scan_, "u_viewport_px");
    scan_u_strength_ = glGetUniformLocation(prog_scan_, "u_strength");

    return true;
}

void Ps1Renderer::Resize(int width, int height) {
    surface_width_ = std::max(1, width);
    surface_height_ = std::max(1, height);
}

void Ps1Renderer::ClearTextCache() {
    for (auto& kv : text_cache_) {
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    }
    text_cache_.clear();
    text_lru_.clear();
}

void Ps1Renderer::ClearImageCache() {
    for (auto& kv : image_cache_) {
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    }
    image_cache_.clear();
    image_lru_.clear();
}

void Ps1Renderer::EvictTextCacheIfNeeded() {
    while (text_cache_.size() >= kMaxTextCache && !text_lru_.empty()) {
        const std::string oldest = text_lru_.back();
        text_lru_.pop_back();
        auto it = text_cache_.find(oldest);
        if (it != text_cache_.end()) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            text_cache_.erase(it);
        }
    }
}

void Ps1Renderer::EvictImageCacheIfNeeded() {
    while (image_cache_.size() >= kMaxImageCache && !image_lru_.empty()) {
        const std::string oldest = image_lru_.back();
        image_lru_.pop_back();
        auto it = image_cache_.find(oldest);
        if (it != image_cache_.end()) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            image_cache_.erase(it);
        }
    }
}

const Ps1Renderer::TextCacheEntry& Ps1Renderer::GetOrCreateText(
    const std::string& text,
    float fontSize,
    int style,
    float maxWidth,
    int maxLines,
    int align) {
    static const TextCacheEntry kEmpty{};
    if (text.empty()) return kEmpty;

    const std::string key = MakeTextCacheKey(text, fontSize, style, maxWidth, maxLines, align);
    auto it = text_cache_.find(key);
    if (it != text_cache_.end()) {
        text_lru_.remove(key);
        text_lru_.push_front(key);
        return it->second;
    }

    int texW = 0;
    int texH = 0;
    float logW = 0.0f;
    float logH = 0.0f;
    std::vector<uint8_t> rgba;
    if (!HostRasterizeText(
            text, fontSize, style, maxWidth, maxLines, align, raster_scale_,
            texW, texH, logW, logH, rgba)) {
        return kEmpty;
    }

    EvictTextCacheIfNeeded();

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    TextCacheEntry entry{tex, logW, logH};
    text_cache_[key] = entry;
    text_lru_.push_front(key);
    return text_cache_[key];
}

const Ps1Renderer::ImageCacheEntry& Ps1Renderer::GetOrCreateImage(const std::string& path, int maxDim) {
    static const ImageCacheEntry kEmpty{};
    if (path.empty()) return kEmpty;

    char keyBuf[32];
    std::snprintf(keyBuf, sizeof(keyBuf), "|%d", maxDim);
    const std::string key = path + keyBuf;
    auto it = image_cache_.find(key);
    if (it != image_cache_.end()) {
        image_lru_.remove(key);
        image_lru_.push_front(key);
        return it->second;
    }

    EvictImageCacheIfNeeded();

    ImageCacheEntry entry{};
    entry.attempted = true;
    std::vector<uint8_t> rgba;
    if (HostDecodeImage(path, maxDim, entry.w, entry.h, rgba) && entry.w > 0 && entry.h > 0) {
        glGenTextures(1, &entry.tex);
        glBindTexture(GL_TEXTURE_2D, entry.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8, entry.w, entry.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    image_cache_[key] = entry;
    image_lru_.push_front(key);
    return image_cache_[key];
}

void Ps1Renderer::MeasureText(
    const std::string& text,
    float fontSize,
    int style,
    float maxWidth,
    int maxLines,
    float& outW,
    float& outH) {
    const auto& entry = GetOrCreateText(text, fontSize, style, maxWidth, maxLines, 0);
    outW = entry.logW;
    outH = entry.logH;
}

void Ps1Renderer::DrawText(
    const std::string& text,
    float x,
    float y,
    float fontSize,
    int style,
    const RgbaColor& color,
    float maxWidth,
    int maxLines,
    int align,
    float boxW) {
    if (text.empty() || color.a <= 0.005f) return;
    const auto& entry = GetOrCreateText(text, fontSize, style, maxWidth, maxLines, align);
    if (!entry.tex || entry.logW <= 0.0f || entry.logH <= 0.0f) return;

    float drawX = x;
    if (align == 1 && boxW > 0.0f) {
        drawX = x + (boxW - entry.logW) * 0.5f;
    } else if (align == 2 && boxW > 0.0f) {
        drawX = x + (boxW - entry.logW);
    }
    PushQuad(entry.tex, drawX, y, entry.logW, entry.logH, color);
}

void Ps1Renderer::BeginQuadBatch() {
    batch_verts_.clear();
    batch_tex_ = 0;
}

void Ps1Renderer::FlushQuadBatch() {
    if (batch_verts_.empty() || !batch_tex_) {
        batch_verts_.clear();
        return;
    }
    glUseProgram(prog_quad_);
    glUniform2f(quad_u_screen_, screen_width_, kScreenHeight);
    glUniform1i(quad_u_texture_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batch_tex_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const size_t byteSize = batch_verts_.size() * sizeof(Vertex);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(byteSize), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(byteSize), batch_verts_.data());
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_verts_.size()));
    batch_verts_.clear();
}

void Ps1Renderer::PushQuad(
    GLuint tex,
    float x, float y, float w, float h,
    const RgbaColor& cTop,
    const RgbaColor& cBot,
    float u0, float v0, float u1, float v1) {
    if (!tex || w <= 0.0f || h <= 0.0f) return;
    if (batch_tex_ != tex || batch_verts_.size() >= 3900) {
        FlushQuadBatch();
        batch_tex_ = tex;
    }
    const float x2 = x + w;
    const float y2 = y + h;
    batch_verts_.push_back({x,  y,  u0, v0, cTop.r, cTop.g, cTop.b, cTop.a});
    batch_verts_.push_back({x2, y,  u1, v0, cTop.r, cTop.g, cTop.b, cTop.a});
    batch_verts_.push_back({x2, y2, u1, v1, cBot.r, cBot.g, cBot.b, cBot.a});
    batch_verts_.push_back({x,  y,  u0, v0, cTop.r, cTop.g, cTop.b, cTop.a});
    batch_verts_.push_back({x2, y2, u1, v1, cBot.r, cBot.g, cBot.b, cBot.a});
    batch_verts_.push_back({x,  y2, u0, v1, cBot.r, cBot.g, cBot.b, cBot.a});
}

void Ps1Renderer::PushQuad(GLuint tex, float x, float y, float w, float h, const RgbaColor& c) {
    PushQuad(tex, x, y, w, h, c, c);
}

void Ps1Renderer::PushTriangle(
    float x0, float y0, const RgbaColor& c0,
    float x1, float y1, const RgbaColor& c1,
    float x2, float y2, const RgbaColor& c2) {
    if (batch_tex_ != white_tex_ || batch_verts_.size() >= 3900) {
        FlushQuadBatch();
        batch_tex_ = white_tex_;
    }
    batch_verts_.push_back({x0, y0, 0.5f, 0.5f, c0.r, c0.g, c0.b, c0.a});
    batch_verts_.push_back({x1, y1, 0.5f, 0.5f, c1.r, c1.g, c1.b, c1.a});
    batch_verts_.push_back({x2, y2, 0.5f, 0.5f, c2.r, c2.g, c2.b, c2.a});
}

void Ps1Renderer::DrawPanel(
    float x, float y, float w, float h,
    float radius,
    float borderPx,
    const RgbaColor& topCol,
    const RgbaColor& botCol,
    const RgbaColor& borderCol,
    const RgbaColor& glowCol) {
    if (w <= 0.5f || h <= 0.5f) return;
    FlushQuadBatch();

    const Vertex verts[6] = {
        {x,     y,     0.0f, 0.0f, 1, 1, 1, 1},
        {x + w, y,     1.0f, 0.0f, 1, 1, 1, 1},
        {x + w, y + h, 1.0f, 1.0f, 1, 1, 1, 1},
        {x,     y,     0.0f, 0.0f, 1, 1, 1, 1},
        {x + w, y + h, 1.0f, 1.0f, 1, 1, 1, 1},
        {x,     y + h, 0.0f, 1.0f, 1, 1, 1, 1},
    };
    glUseProgram(prog_panel_);
    glUniform2f(pan_u_screen_, screen_width_, kScreenHeight);
    glUniform2f(pan_u_size_, w, h);
    glUniform1f(pan_u_radius_, radius);
    glUniform1f(pan_u_border_px_, borderPx);
    glUniform4f(pan_u_top_col_, topCol.r, topCol.g, topCol.b, topCol.a);
    glUniform4f(pan_u_bot_col_, botCol.r, botCol.g, botCol.b, botCol.a);
    glUniform4f(pan_u_border_col_, borderCol.r, borderCol.g, borderCol.b, borderCol.a);
    glUniform4f(pan_u_glow_col_, glowCol.r, glowCol.g, glowCol.b, glowCol.a);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Ps1Renderer::DrawGraffitiSplash(
    float x, float y, float w, float h, float seed, float focus, float alpha) {
    if (w <= 1.0f || h <= 1.0f || alpha <= 0.005f) return;
    FlushQuadBatch();

    const Vertex verts[6] = {
        {x,     y,     0.0f, 0.0f, 1, 1, 1, 1},
        {x + w, y,     1.0f, 0.0f, 1, 1, 1, 1},
        {x + w, y + h, 1.0f, 1.0f, 1, 1, 1, 1},
        {x,     y,     0.0f, 0.0f, 1, 1, 1, 1},
        {x + w, y + h, 1.0f, 1.0f, 1, 1, 1, 1},
        {x,     y + h, 0.0f, 1.0f, 1, 1, 1, 1},
    };
    glUseProgram(prog_graffiti_);
    glUniform2f(graf_u_screen_, screen_width_, kScreenHeight);
    glUniform1f(graf_u_seed_, seed);
    glUniform1f(graf_u_focus_, focus);
    glUniform1f(graf_u_alpha_, alpha);
    glUniform1f(graf_u_time_, time_sec_);
    glUniform1i(graf_u_theme_, settings_.theme);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Ps1Renderer::DrawOpticalDisc(float cx, float cy, float radius, float angleRad, float alpha) {
    if (radius <= 1.0f || alpha <= 0.005f) return;
    FlushQuadBatch();

    const float x = cx - radius;
    const float y = cy - radius;
    const float d = radius * 2.0f;
    const Vertex verts[6] = {
        {x,     y,     0.0f, 0.0f, 1, 1, 1, 1},
        {x + d, y,     1.0f, 0.0f, 1, 1, 1, 1},
        {x + d, y + d, 1.0f, 1.0f, 1, 1, 1, 1},
        {x,     y,     0.0f, 0.0f, 1, 1, 1, 1},
        {x + d, y + d, 1.0f, 1.0f, 1, 1, 1, 1},
        {x,     y + d, 0.0f, 1.0f, 1, 1, 1, 1},
    };
    glUseProgram(prog_disc_);
    glUniform2f(disc_u_screen_, screen_width_, kScreenHeight);
    glUniform1f(disc_u_angle_, angleRad);
    glUniform1f(disc_u_alpha_, alpha);
    glUniform1i(disc_u_theme_, settings_.theme);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Ps1Renderer::DrawTriangleCursor(float cx, float cy, float size, float alpha) {
    if (alpha <= 0.01f) return;
    const float pulse = 1.0f + 0.08f * std::sin(time_sec_ * 4.2f);
    const float tilt = 0.22f * std::sin(time_sec_ * 2.5f);
    const float s = size * pulse;

    // 3D faceted magenta-pink pyramid arrow (matches classic PS1 BIOS cursor silhouette)
    const float tipX = cx - s * 0.62f;
    const float tipY = cy - s * 0.42f;
    const float topX = cx + s * 0.72f;
    const float topY = cy - s * (0.28f + tilt * 0.35f);
    const float botX = cx + s * 0.40f;
    const float botY = cy + s * (0.74f - tilt * 0.30f);
    const float apexX = cx + s * 0.16f;
    const float apexY = cy + s * 0.02f;

    // Drop shadow
    const RgbaColor sh{0.0f, 0.0f, 0.05f, 0.60f * alpha};
    PushTriangle(tipX + 3.0f, tipY + 3.5f, sh, topX + 3.0f, topY + 3.5f, sh, botX + 3.0f, botY + 3.5f, sh);

    RgbaColor bright, mid, dark;
    if (settings_.theme == 1) {
        bright = {0.38f, 0.78f, 1.0f, alpha};
        mid = {0.14f, 0.46f, 0.96f, alpha};
        dark = {0.06f, 0.24f, 0.72f, alpha};
    } else if (settings_.theme == 2) {
        bright = {1.0f, 0.88f, 0.42f, alpha};
        mid = {0.92f, 0.62f, 0.14f, alpha};
        dark = {0.58f, 0.32f, 0.06f, alpha};
    } else {
        // Classic SCPH-7001 vibrant magenta-pink faceted cursor
        bright = {1.0f, 0.64f, 0.94f, alpha};
        mid = {0.90f, 0.26f, 0.76f, alpha};
        dark = {0.50f, 0.08f, 0.42f, alpha};
    }

    PushTriangle(tipX, tipY, bright, topX, topY, mid, apexX, apexY, bright);
    PushTriangle(tipX, tipY, mid, apexX, apexY, bright, botX, botY, dark);
    PushTriangle(topX, topY, mid, botX, botY, dark, apexX, apexY, mid);
}

void Ps1Renderer::DrawImageAspectFit(
    const ImageCacheEntry& img,
    float x, float y, float w, float h,
    float alpha,
    bool fillCrop) {
    if (!img.tex || img.w <= 0 || img.h <= 0 || alpha <= 0.005f) return;
    const float srcAspect = static_cast<float>(img.w) / static_cast<float>(img.h);
    const float dstAspect = w / std::max(1.0f, h);

    if (!fillCrop) {
        float drawW = w;
        float drawH = h;
        if (srcAspect > dstAspect) {
            drawH = w / srcAspect;
        } else {
            drawW = h * srcAspect;
        }
        const float dx = x + (w - drawW) * 0.5f;
        const float dy = y + (h - drawH) * 0.5f;
        PushQuad(img.tex, dx, dy, drawW, drawH, {1.0f, 1.0f, 1.0f, alpha});
    } else {
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        if (srcAspect > dstAspect) {
            float keep = dstAspect / srcAspect;
            u0 = (1.0f - keep) * 0.5f;
            u1 = 1.0f - u0;
        } else {
            float keep = srcAspect / dstAspect;
            v0 = (1.0f - keep) * 0.5f;
            v1 = 1.0f - v0;
        }
        PushQuad(img.tex, x, y, w, h, {1.0f, 1.0f, 1.0f, alpha}, {1.0f, 1.0f, 1.0f, alpha}, u0, v0, u1, v1);
    }
}

RgbaColor Ps1Renderer::PanelTopColor(float alpha) const {
    if (settings_.theme == 1) return {0.90f, 0.93f, 0.98f, alpha};
    if (settings_.theme == 2) return {0.07f, 0.11f, 0.21f, alpha};
    return {0.04f, 0.07f, 0.27f, alpha};
}

RgbaColor Ps1Renderer::PanelBotColor(float alpha) const {
    if (settings_.theme == 1) return {0.78f, 0.84f, 0.94f, alpha};
    if (settings_.theme == 2) return {0.03f, 0.06f, 0.13f, alpha};
    return {0.015f, 0.03f, 0.16f, alpha};
}

RgbaColor Ps1Renderer::PanelBorderColor(float alpha) const {
    if (settings_.theme == 1) return {0.26f, 0.44f, 0.84f, alpha};
    if (settings_.theme == 2) return {0.26f, 0.68f, 0.82f, alpha};
    return {0.32f, 0.46f, 1.0f, alpha};
}

RgbaColor Ps1Renderer::AccentColor(float alpha) const {
    if (settings_.theme == 1) return {0.14f, 0.40f, 0.92f, alpha};
    if (settings_.theme == 2) return {0.18f, 0.66f, 0.84f, alpha};
    return {0.22f, 0.44f, 1.0f, alpha};
}

RgbaColor Ps1Renderer::PrimaryTextColor(float alpha) const {
    if (settings_.theme == 1) return {0.10f, 0.16f, 0.34f, alpha};
    return {0.98f, 0.99f, 1.0f, alpha};
}

RgbaColor Ps1Renderer::SecondaryTextColor(float alpha) const {
    if (settings_.theme == 1) return {0.24f, 0.34f, 0.56f, alpha};
    return {0.74f, 0.82f, 1.0f, alpha};
}

Ps1Renderer::LayoutMetrics Ps1Renderer::ComputeMetrics() const {
    LayoutMetrics m;
    m.padX = std::clamp(screen_width_ * 0.032f, 22.0f, 54.0f);
    m.topBarH = 44.0f;
    m.bottomBarH = 40.0f;
    m.bodyTop = 66.0f;
    m.bodyBot = kScreenHeight - 58.0f;
    m.bodyH = m.bodyBot - m.bodyTop;
    return m;
}

std::string Ps1Renderer::TextOr(const std::string& key, const char* fallback) const {
    auto it = strings_.find(key);
    if (it != strings_.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback ? std::string(fallback) : std::string();
}

void Ps1Renderer::DrawHeaderPlaque(const std::string& title, float alpha) {
    const LayoutMetrics m = ComputeMetrics();
    float tw = 0.0f, th = 0.0f;
    MeasureText(title, 24.0f, STYLE_HEADER, 400.0f, 1, tw, th);
    const float boxW = std::clamp(tw + 52.0f, 210.0f, 420.0f);
    const float boxH = 42.0f;
    const float bx = screen_width_ - m.padX - boxW;
    const float by = 12.0f;

    if (settings_.theme == 0) {
        // Authentic SCPH-7001 framed navy header with concentric cobalt glow
        DrawPanel(
            bx, by, boxW, boxH, 0.0f, 2.0f,
            {0.01f, 0.02f, 0.14f, 0.96f * alpha},
            {0.01f, 0.02f, 0.14f, 0.96f * alpha},
            {0.34f, 0.46f, 1.0f, alpha},
            {0.16f, 0.30f, 0.94f, 0.95f});
    } else if (settings_.theme == 1) {
        DrawPanel(
            bx, by, boxW, boxH, 10.0f, 2.0f,
            {0.18f, 0.40f, 0.88f, 0.96f * alpha},
            {0.08f, 0.22f, 0.62f, 0.98f * alpha},
            {1.0f, 1.0f, 1.0f, 0.92f * alpha},
            {0.34f, 0.64f, 1.0f, 0.55f});
    } else {
        DrawPanel(
            bx, by, boxW, boxH, 4.0f, 2.0f,
            {0.06f, 0.13f, 0.26f, 0.94f * alpha},
            {0.03f, 0.07f, 0.16f, 0.96f * alpha},
            {0.28f, 0.72f, 0.88f, 0.95f * alpha},
            {0.12f, 0.46f, 0.68f, 0.72f});
    }
    DrawText(
        title,
        bx, by + (boxH - th) * 0.5f,
        24.0f, STYLE_HEADER,
        {1.0f, 1.0f, 1.0f, alpha},
        boxW - 14.0f, 1, 1, boxW);
}

std::string Ps1Renderer::FormatClockText() const {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf{};
    localtime_r(&now, &tmBuf);
    char buf[32];
    if (settings_.time24h) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tmBuf.tm_hour, tmBuf.tm_min);
    } else {
        int h = tmBuf.tm_hour % 12;
        if (h == 0) h = 12;
        const char* ampm = tmBuf.tm_hour >= 12 ? "PM" : "AM";
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", h, tmBuf.tm_min, ampm);
    }
    return std::string(buf);
}

std::string Ps1Renderer::FormatCurrentDateText() const {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf{};
    localtime_r(&now, &tmBuf);
    char buf[32];
    int y = tmBuf.tm_year + 1900;
    int m = tmBuf.tm_mon + 1;
    int d = tmBuf.tm_mday;
    if (settings_.dateFormat == 1) {
        std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d", m, d, y);
    } else if (settings_.dateFormat == 2) {
        std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d", y, m, d);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d", d, m, y);
    }
    return std::string(buf);
}

void Ps1Renderer::DrawStatusBar(float alpha) {
    const LayoutMetrics m = ComputeMetrics();
    const std::string clockStr = FormatClockText();
    const std::string dateStr = FormatCurrentDateText();

    char statusBuf[128];
    std::snprintf(
        statusBuf, sizeof(statusBuf), "%s  •  %s  •  %s%d%%",
        dateStr.c_str(),
        clockStr.c_str(),
        status_.batteryCharging ? "⚡ " : "",
        std::clamp(status_.batteryLevel, 0, 100));

    float tw = 0.0f, th = 0.0f;
    MeasureText(statusBuf, 16.0f, STYLE_MONO, 440.0f, 1, tw, th);
    const float pillW = std::clamp(tw + 30.0f, 190.0f, 410.0f);
    const float pillH = 34.0f;
    const float px = m.padX;
    const float py = 16.0f;

    DrawPanel(
        px, py, pillW, pillH, 7.0f, 1.2f,
        PanelTopColor(0.68f * alpha),
        PanelBotColor(0.76f * alpha),
        PanelBorderColor(0.55f * alpha));

    RgbaColor fg = (settings_.theme == 1)
        ? RgbaColor{0.10f, 0.16f, 0.34f, alpha}
        : RgbaColor{0.92f, 0.95f, 1.0f, alpha};
    DrawText(statusBuf, px + 15.0f, py + (pillH - th) * 0.5f, 16.0f, STYLE_MONO, fg, pillW - 24.0f, 1, 0);

    // Compact wireless / audio dot indicators to the right of the pill
    float dotX = px + pillW + 10.0f;
    const float dotY = py + pillH * 0.5f;
    auto drawDot = [&](bool active, const RgbaColor& onCol, const char* lbl) {
        if (!active) return;
        DrawPanel(dotX, dotY - 11.0f, 48.0f, 22.0f, 5.0f, 1.0f,
                  {onCol.r * 0.35f, onCol.g * 0.35f, onCol.b * 0.45f, 0.80f * alpha},
                  {onCol.r * 0.25f, onCol.g * 0.25f, onCol.b * 0.35f, 0.86f * alpha},
                  {onCol.r, onCol.g, onCol.b, 0.78f * alpha});
        float lw = 0, lh = 0;
        MeasureText(lbl, 12.5f, STYLE_MONO, 44.0f, 1, lw, lh);
        DrawText(lbl, dotX, dotY - lh * 0.5f, 12.5f, STYLE_MONO, {1, 1, 1, alpha}, 44.0f, 1, 1, 48.0f);
        dotX += 54.0f;
    };
    drawDot(status_.wifiConnected, {0.35f, 0.72f, 1.0f, 1.0f}, "NET");
    drawDot(status_.bluetoothConnected, {0.42f, 0.54f, 1.0f, 1.0f}, "BT");
    drawDot(status_.audioMuted, {1.0f, 0.45f, 0.45f, 1.0f}, "MUTE");
}

void Ps1Renderer::DrawBottomBar(float alpha) {
    const LayoutMetrics m = ComputeMetrics();
    const float barH = 38.0f;
    const float barY = kScreenHeight - barH - 10.0f;

    struct HintBtn {
        std::string badge;
        std::string label;
        int key;
        RgbaColor badgeCol;
    };
    std::vector<HintBtn> btns;
    const std::string confirmSym = settings_.swapConfirm ? "○" : "✕";
    const std::string backSym = settings_.swapConfirm ? "✕" : "○";
    const RgbaColor confirmCol = settings_.swapConfirm
        ? RgbaColor{0.96f, 0.36f, 0.44f, 1.0f}
        : RgbaColor{0.44f, 0.68f, 1.0f, 1.0f};
    const RgbaColor backCol = settings_.swapConfirm
        ? RgbaColor{0.44f, 0.68f, 1.0f, 1.0f}
        : RgbaColor{0.96f, 0.36f, 0.44f, 1.0f};

    if (screen_ == SCREEN_MAIN_MENU) {
        btns.push_back({confirmSym, TextOr("ps1ui_hint_select", "Open"), KEY_CONFIRM, confirmCol});
        btns.push_back({backSym, TextOr("ps1ui_hint_exit", "Exit"), KEY_BACK, backCol});
    } else if (screen_ == SCREEN_GAMES) {
        btns.push_back({confirmSym, TextOr("ps1ui_btn_start", "Start"), KEY_CONFIRM, confirmCol});
        if (!games_.empty() && game_index_ < static_cast<int>(games_.size()) &&
            games_[game_index_].latestSaveSlot >= 0) {
            btns.push_back({"□", TextOr("ps1ui_btn_continue", "Continue"), KEY_SECONDARY, {0.88f, 0.48f, 0.92f, 1.0f}});
        }
        btns.push_back({"△", TextOr("ps1ui_btn_options", "Options"), KEY_OPTIONS, {0.28f, 0.86f, 0.56f, 1.0f}});
        btns.push_back({backSym, TextOr("ps1ui_hint_back", "Back"), KEY_BACK, backCol});
    } else if (screen_ == SCREEN_MEMORY_CARD) {
        btns.push_back({"L1/R1", TextOr("ps1ui_hint_tab", "Switch Tab"), KEY_NEXT_TAB, {0.58f, 0.72f, 1.0f, 1.0f}});
        if (mc_tab_ != 2) {
            btns.push_back({confirmSym, TextOr("ps1ui_mc_load", "Load"), KEY_CONFIRM, confirmCol});
            btns.push_back({"□", TextOr("ps1ui_mc_delete", "Delete"), KEY_SECONDARY, {0.96f, 0.48f, 0.36f, 1.0f}});
        }
        btns.push_back({backSym, TextOr("ps1ui_hint_back", "Back"), KEY_BACK, backCol});
    } else if (screen_ == SCREEN_SETTINGS) {
        btns.push_back({confirmSym, TextOr("ps1ui_hint_change", "Change"), KEY_CONFIRM, confirmCol});
        btns.push_back({backSym, TextOr("ps1ui_hint_back", "Back"), KEY_BACK, backCol});
    }

    std::vector<float> widths;
    float totalW = 0.0f;
    for (const auto& b : btns) {
        float bw = 0, bh = 0, lw = 0, lh = 0;
        MeasureText(b.badge, 15.0f, STYLE_MONO, 90.0f, 1, bw, bh);
        MeasureText(b.label, 15.5f, STYLE_BODY_BOLD, 180.0f, 1, lw, lh);
        float w = bw + lw + 38.0f;
        widths.push_back(w);
        totalW += w;
    }
    if (!btns.empty()) totalW += 10.0f * static_cast<float>(btns.size() - 1);

    float curX = (screen_width_ - totalW) * 0.5f;
    if (curX < m.padX) curX = m.padX;

    for (size_t i = 0; i < btns.size(); ++i) {
        const float w = widths[i];
        const bool touchInteractive = (touch_mode_active_ && settings_.touchControls);
        DrawPanel(
            curX, barY, w, barH, 8.0f, touchInteractive ? 1.4f : 1.0f,
            PanelTopColor((touchInteractive ? 0.84f : 0.65f) * alpha),
            PanelBotColor((touchInteractive ? 0.90f : 0.72f) * alpha),
            PanelBorderColor((touchInteractive ? 0.78f : 0.45f) * alpha));

        float bw = 0, bh = 0, lw = 0, lh = 0;
        MeasureText(btns[i].badge, 15.0f, STYLE_MONO, 90.0f, 1, bw, bh);
        MeasureText(btns[i].label, 15.5f, STYLE_BODY_BOLD, 180.0f, 1, lw, lh);
        RgbaColor badgeFg = btns[i].badgeCol;
        badgeFg.a *= alpha;
        DrawText(btns[i].badge, curX + 13.0f, barY + (barH - bh) * 0.5f, 15.0f, STYLE_MONO, badgeFg, 90.0f, 1, 0);
        DrawText(btns[i].label, curX + 20.0f + bw, barY + (barH - lh) * 0.5f, 15.5f, STYLE_BODY_BOLD,
                 PrimaryTextColor(alpha), 180.0f, 1, 0);

        if (touchInteractive && overlay_ == OVERLAY_NONE) {
            RegisterHit(curX, barY, w, barH, HIT_BOTTOM_ACTION, btns[i].key);
        }
        curX += w + 10.0f;
    }
}

void Ps1Renderer::DrawBootIntro(float alpha) {
    // Crisp high-res pearl canvas fading cleanly into the midnight BIOS backdrop
    const float fadeOut = (boot_t_ > 0.76f) ? SmoothStep(0.76f, 1.0f, boot_t_) : 0.0f;
    const float bgAlpha = (1.0f - fadeOut) * alpha;
    if (bgAlpha <= 0.01f) return;

    PushQuad(white_tex_, 0.0f, 0.0f, screen_width_, kScreenHeight,
             {0.96f, 0.96f, 0.985f, bgAlpha},
             {0.89f, 0.91f, 0.96f, bgAlpha});

    const float cx = screen_width_ * 0.5f;
    const float cy = kScreenHeight * 0.41f;
    const float scaleIn = 0.72f + 0.28f * EaseOutCubic(std::min(1.0f, boot_t_ * 2.4f));
    const float prismR = 96.0f * scaleIn;

    // 3D faceted amber-vermilion twin diamond emblem
    const float split = (1.0f - EaseOutCubic(std::min(1.0f, boot_t_ * 2.6f))) * 26.0f;
    const RgbaColor amberTop{0.98f, 0.74f, 0.14f, bgAlpha};
    const RgbaColor orangeMid{0.96f, 0.42f, 0.08f, bgAlpha};
    const RgbaColor redDeep{0.78f, 0.10f, 0.10f, bgAlpha};
    const RgbaColor lightEdge{1.0f, 0.92f, 0.48f, bgAlpha};

    // Upper-left diamond half
    PushTriangle(cx, cy - prismR - split, amberTop,
                 cx - prismR * 0.85f, cy - split * 0.3f, orangeMid,
                 cx, cy - split * 0.2f, lightEdge);
    PushTriangle(cx - prismR * 0.85f, cy - split * 0.3f, orangeMid,
                 cx, cy + prismR * 0.85f - split * 0.2f, redDeep,
                 cx, cy - split * 0.2f, orangeMid);

    // Lower-right diamond half
    PushTriangle(cx, cy - prismR * 0.85f + split * 0.2f, orangeMid,
                 cx + prismR * 0.85f, cy + split * 0.3f, amberTop,
                 cx, cy + split * 0.2f, lightEdge);
    PushTriangle(cx + prismR * 0.85f, cy + split * 0.3f, orangeMid,
                 cx, cy + prismR + split, redDeep,
                 cx, cy + split * 0.2f, orangeMid);

    // Inner bright diamond cutout
    const float ir = prismR * 0.40f;
    PushTriangle(cx, cy - ir, {0.96f, 0.96f, 0.99f, bgAlpha},
                 cx - ir * 0.86f, cy, {0.93f, 0.94f, 0.98f, bgAlpha},
                 cx + ir * 0.86f, cy, {0.93f, 0.94f, 0.98f, bgAlpha});
    PushTriangle(cx - ir * 0.86f, cy, {0.93f, 0.94f, 0.98f, bgAlpha},
                 cx, cy + ir, {0.90f, 0.92f, 0.97f, bgAlpha},
                 cx + ir * 0.86f, cy, {0.93f, 0.94f, 0.98f, bgAlpha});

    const float textReveal = SmoothStep(0.18f, 0.48f, boot_t_) * bgAlpha;
    if (textReveal > 0.01f) {
        const std::string brand = "EMUCORE R";
        const std::string sub = TextOr("ps1ui_boot_subtitle", "NATIVE SYSTEM SHELL");
        float bw = 0, bh = 0, sw = 0, sh = 0;
        MeasureText(brand, 38.0f, STYLE_HEADER, 520.0f, 1, bw, bh);
        MeasureText(sub, 18.0f, STYLE_MONO, 520.0f, 1, sw, sh);
        DrawText(brand, cx - bw * 0.5f, cy + prismR + 28.0f, 38.0f, STYLE_HEADER,
                 {0.12f, 0.14f, 0.24f, textReveal}, 520.0f, 1, 0);
        DrawText(sub, cx - sw * 0.5f, cy + prismR + 76.0f, 18.0f, STYLE_MONO,
                 {0.30f, 0.34f, 0.52f, textReveal}, 520.0f, 1, 0);
    }
}

void Ps1Renderer::DrawMainMenu(float alpha) {
    const LayoutMetrics m = ComputeMetrics();
    DrawHeaderPlaque(TextOr("ps1ui_main_menu", "MAIN MENU"), alpha);

    struct MainMenuItem {
        std::string label;
        std::string badge;
        float seed;
        float staggerX;
    };

    char gamesBadge[32];
    std::snprintf(gamesBadge, sizeof(gamesBadge), "%zu", games_.size());
    char savesBadge[32];
    std::snprintf(savesBadge, sizeof(savesBadge), "%zu", saves_.size());

    std::vector<MainMenuItem> items = {
        {TextOr("ps1ui_menu_games", "GAMES"), games_.empty() ? "" : std::string(gamesBadge), 1.37f, 0.0f},
        {TextOr("ps1ui_menu_memory_card", "MEMORY CARD"), saves_.empty() ? "" : std::string(savesBadge), 4.91f, 40.0f},
        {TextOr("ps1ui_menu_settings", "SETTINGS"), "", 8.23f, 80.0f},
    };

    const float baseX = m.padX + 58.0f;
    const float rowSpacing = 112.0f;
    const float firstY = 124.0f;

    float targetCurX = 0.0f;
    float targetCurY = 0.0f;

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const bool selected = (i == main_index_);
        const float focus = selected ? 1.0f : 0.0f;
        const float x = baseX + items[i].staggerX;
        const float y = firstY + static_cast<float>(i) * rowSpacing;

        float lw = 0.0f, lh = 0.0f;
        const float fontSize = selected ? 40.0f : 35.0f;
        const bool hasBadge = !items[i].badge.empty();
        const float maxLblW = std::max(180.0f, screen_width_ - x - m.padX - (hasBadge ? 72.0f : 18.0f));
        MeasureText(items[i].label, fontSize, STYLE_GRAFFITI, maxLblW, 1, lw, lh);

        const float splashW = std::max(340.0f, lw + 114.0f);
        const float splashH = 88.0f;
        DrawGraffitiSplash(x - 38.0f, y - 16.0f, splashW, splashH, items[i].seed, focus, alpha);

        const RgbaColor txtCol = (settings_.theme == 1 && !selected)
            ? RgbaColor{0.96f, 0.98f, 1.0f, 0.94f * alpha}
            : RgbaColor{1.0f, 1.0f, 1.0f, (selected ? 1.0f : 0.88f) * alpha};
        DrawText(items[i].label, x, y + (54.0f - lh) * 0.5f, fontSize, STYLE_GRAFFITI,
                 txtCol, maxLblW, 1, 0);

        if (hasBadge) {
            float bw = 0.0f, bh = 0.0f;
            MeasureText(items[i].badge, 16.0f, STYLE_MONO, 80.0f, 1, bw, bh);
            const float pillW = std::max(40.0f, bw + 20.0f);
            const float pillH = 28.0f;
            const float bx = x + lw + 18.0f;
            const float by = y + 13.0f;
            DrawPanel(
                bx, by, pillW, pillH, 14.0f, 1.2f,
                PanelTopColor(0.84f * alpha),
                PanelBotColor(0.92f * alpha),
                PanelBorderColor(selected ? alpha : 0.65f * alpha));
            DrawText(items[i].badge, bx, by + (pillH - bh) * 0.5f, 16.0f, STYLE_MONO,
                     PrimaryTextColor(alpha), pillW, 1, 1, pillW);
        }

        if (selected) {
            targetCurX = x - 28.0f;
            targetCurY = y + 27.0f;
        }

        if (overlay_ == OVERLAY_NONE) {
            RegisterHit(x - 42.0f, y - 14.0f, splashW + 24.0f, 84.0f, HIT_MAIN_ROW, i);
        }
    }

    if (!cursor_pos_init_) {
        cursor_x_ = targetCurX;
        cursor_y_ = targetCurY;
        cursor_pos_init_ = true;
    } else {
        cursor_x_ += (targetCurX - cursor_x_) * 0.28f;
        cursor_y_ += (targetCurY - cursor_y_) * 0.28f;
    }
    if (overlay_ == OVERLAY_NONE) {
        DrawTriangleCursor(cursor_x_, cursor_y_, 21.0f, alpha);
    }
}

void Ps1Renderer::DrawGamesScreen(float alpha) {
    const LayoutMetrics m = ComputeMetrics();
    DrawHeaderPlaque(TextOr("ps1ui_menu_games", "GAMES"), alpha);

    const float gap = 18.0f;
    const float usableW = screen_width_ - 2.0f * m.padX;
    const float leftW = std::round(usableW * 0.55f);
    const float rightW = usableW - leftW - gap;
    const float leftX = m.padX;
    const float rightX = leftX + leftW + gap;
    const float topY = m.bodyTop;
    const float paneH = m.bodyH;

    // Left Pane: Game Library List
    DrawPanel(
        leftX, topY, leftW, paneH, 8.0f, 1.6f,
        PanelTopColor(0.84f * alpha),
        PanelBotColor(0.90f * alpha),
        PanelBorderColor(0.85f * alpha));

    if (games_.empty()) {
        const std::string msg = games_loading_
            ? TextOr("ps1ui_loading_games", "Scanning library...")
            : TextOr("ps1ui_no_games", "No PS1 games found");
        float tw = 0, th = 0;
        MeasureText(msg, 18.0f, STYLE_BODY_BOLD, leftW - 40.0f, 2, tw, th);
        DrawText(msg, leftX + 20.0f, topY + (paneH - th) * 0.5f, 18.0f, STYLE_BODY_BOLD,
                 PrimaryTextColor(alpha), leftW - 40.0f, 2, 1, leftW - 40.0f);
        return;
    }

    // List header strip
    char countBuf[48];
    std::snprintf(countBuf, sizeof(countBuf), "%d / %zu", game_index_ + 1, games_.size());
    DrawText(countBuf, leftX + 16.0f, topY + 9.0f, 15.0f, STYLE_MONO,
             SecondaryTextColor(alpha), leftW - 32.0f, 1, 2, leftW - 32.0f);

    const float listTop = topY + 34.0f;
    const float listH = paneH - 44.0f;
    const float rowH = 54.0f;
    const float rowGap = 5.0f;
    const float rowStep = rowH + rowGap;

    const float totalContentH = static_cast<float>(games_.size()) * rowStep - rowGap;
    const float maxScroll = std::max(0.0f, totalContentH - listH);
    game_scroll_target_y_ = std::clamp(game_scroll_target_y_, 0.0f, maxScroll);
    game_scroll_y_ += (game_scroll_target_y_ - game_scroll_y_) * 0.25f;
    if (std::fabs(game_scroll_target_y_ - game_scroll_y_) < 0.3f) {
        game_scroll_y_ = game_scroll_target_y_;
    }

    // Scissor list interior to physical viewport pixels
    FlushQuadBatch();
    const float sxScale = static_cast<float>(view_width_) / screen_width_;
    const float syScale = static_cast<float>(view_height_) / kScreenHeight;
    const int scX = view_x_ + static_cast<int>(std::floor((leftX + 4.0f) * sxScale));
    const int scY = view_y_ + static_cast<int>(std::floor((kScreenHeight - (listTop + listH)) * syScale));
    const int scW = std::max(1, static_cast<int>(std::ceil((leftW - 14.0f) * sxScale)));
    const int scH = std::max(1, static_cast<int>(std::ceil(listH * syScale)));
    glEnable(GL_SCISSOR_TEST);
    glScissor(scX, scY, scW, scH);

    for (int i = 0; i < static_cast<int>(games_.size()); ++i) {
        const float ry = listTop + static_cast<float>(i) * rowStep - game_scroll_y_;
        if (ry + rowH < listTop - 4.0f || ry > listTop + listH + 4.0f) continue;

        const bool selected = (i == game_index_);
        const float rx = leftX + 10.0f;
        const float rw = leftW - 26.0f;
        if (selected) {
            DrawPanel(
                rx, ry, rw, rowH, 7.0f, 1.6f,
                AccentColor(0.90f * alpha),
                {AccentColor(1.0f).r * 0.65f, AccentColor(1.0f).g * 0.68f, AccentColor(1.0f).b * 0.85f, 0.94f * alpha},
                {0.82f, 0.90f, 1.0f, 0.96f * alpha});
        } else {
            DrawPanel(
                rx, ry, rw, rowH, 7.0f, 0.0f,
                PanelTopColor(0.32f * alpha),
                PanelBotColor(0.36f * alpha),
                {0, 0, 0, 0});
        }

        // Dedicated left cursor gutter so cover art thumbnail stays completely unobstructed
        const float thumbS = 40.0f;
        const float tx = rx + 30.0f;
        const float ty = ry + (rowH - thumbS) * 0.5f;
        const auto& cov = GetOrCreateImage(games_[i].coverPath, 160);
        if (cov.tex) {
            DrawImageAspectFit(cov, tx, ty, thumbS, thumbS, alpha, true);
        } else {
            DrawOpticalDisc(tx + thumbS * 0.5f, ty + thumbS * 0.5f, thumbS * 0.46f,
                            selected ? disc_spin_angle_ : 0.4f, alpha);
        }

        // Badges on right side of row
        const float textStartX = tx + thumbS + 10.0f;
        float badgeRightX = rx + rw - 10.0f;
        if (games_[i].saveCount > 0) {
            char sb[16];
            std::snprintf(sb, sizeof(sb), "★%d", games_[i].saveCount);
            float bw = 0, bh = 0;
            MeasureText(sb, 13.5f, STYLE_MONO, 70.0f, 1, bw, bh);
            const float pw = bw + 14.0f;
            badgeRightX -= pw;
            DrawPanel(badgeRightX, ry + (rowH - 24.0f) * 0.5f, pw, 24.0f, 6.0f, 1.0f,
                      {0.18f, 0.46f, 0.92f, 0.88f * alpha},
                      {0.10f, 0.28f, 0.68f, 0.92f * alpha},
                      {0.58f, 0.82f, 1.0f, 0.90f * alpha});
            DrawText(sb, badgeRightX, ry + (rowH - bh) * 0.5f, 13.5f, STYLE_MONO,
                     {1, 1, 1, alpha}, pw, 1, 1, pw);
            badgeRightX -= 6.0f;
        }
        if (games_[i].discCount > 1) {
            char db[16];
            std::snprintf(db, sizeof(db), "%dD", games_[i].discCount);
            float bw = 0, bh = 0;
            MeasureText(db, 13.5f, STYLE_MONO, 60.0f, 1, bw, bh);
            const float pw = bw + 14.0f;
            badgeRightX -= pw;
            DrawPanel(badgeRightX, ry + (rowH - 24.0f) * 0.5f, pw, 24.0f, 6.0f, 1.0f,
                      {0.34f, 0.26f, 0.68f, 0.88f * alpha},
                      {0.20f, 0.14f, 0.48f, 0.92f * alpha},
                      {0.76f, 0.68f, 1.0f, 0.90f * alpha});
            DrawText(db, badgeRightX, ry + (rowH - bh) * 0.5f, 13.5f, STYLE_MONO,
                     {1, 1, 1, alpha}, pw, 1, 1, pw);
            badgeRightX -= 6.0f;
        }

        const float rightAvail = std::max(60.0f, badgeRightX - textStartX - 6.0f);
        const RgbaColor rowTextCol = (selected && settings_.theme == 1)
            ? RgbaColor{1.0f, 1.0f, 1.0f, alpha}
            : PrimaryTextColor(alpha);
        float tw = 0, th = 0;
        MeasureText(games_[i].title, 18.5f, selected ? STYLE_BODY_BOLD : STYLE_BODY,
                    rightAvail, 1, tw, th);
        DrawText(games_[i].title, textStartX, ry + (rowH - th) * 0.5f,
                 18.5f, selected ? STYLE_BODY_BOLD : STYLE_BODY, rowTextCol,
                 rightAvail, 1, 0);

        if (selected && overlay_ == OVERLAY_NONE) {
            DrawTriangleCursor(rx + 15.0f, ry + rowH * 0.5f, 12.5f, alpha);
        }

        if (overlay_ == OVERLAY_NONE) {
            const float hitTop = std::max(listTop, ry);
            const float hitBot = std::min(listTop + listH, ry + rowH);
            if (hitBot > hitTop + 4.0f) {
                RegisterHit(rx, hitTop, rw, hitBot - hitTop, HIT_GAME_ROW, i);
            }
        }
    }
    FlushQuadBatch();
    glDisable(GL_SCISSOR_TEST);

    // Scrollbar
    if (maxScroll > 1.0f) {
        const float sbX = leftX + leftW - 11.0f;
        const float sbY = listTop + 2.0f;
        const float sbH = listH - 4.0f;
        DrawPanel(sbX, sbY, 4.0f, sbH, 2.0f, 0.0f,
                  {0.1f, 0.14f, 0.35f, 0.5f * alpha}, {0.1f, 0.14f, 0.35f, 0.5f * alpha}, {0, 0, 0, 0});
        const float thumbH = std::max(24.0f, sbH * (listH / totalContentH));
        const float thumbY = sbY + (sbH - thumbH) * (game_scroll_y_ / maxScroll);
        DrawPanel(sbX, thumbY, 4.0f, thumbH, 2.0f, 0.0f,
                  PanelBorderColor(alpha), AccentColor(alpha), {0, 0, 0, 0});
    }

    // Right Pane: Authentic PS1 Jewel Case + Spinning Black-Bottom Optical Disc + Metadata + Actions
    const GameEntry& g = games_[game_index_];
    DrawPanel(
        rightX, topY, rightW, paneH, 8.0f, 1.6f,
        PanelTopColor(0.84f * alpha),
        PanelBotColor(0.90f * alpha),
        PanelBorderColor(0.85f * alpha));

    const float caseSize = std::min(rightW * 0.52f, 182.0f);
    const float caseX = rightX + (rightW - caseSize * 1.24f) * 0.5f;
    const float caseY = topY + 14.0f;

    // Peeking spinning PS1 black-bottom optical disc on the right of the jewel case
    const float discRadius = caseSize * 0.44f;
    const float discCx = caseX + caseSize * 0.94f;
    const float discCy = caseY + caseSize * 0.50f;
    DrawOpticalDisc(discCx, discCy, discRadius, disc_spin_angle_, alpha);

    // Jewel Case shadow + shell + left spine + cover art + acrylic highlight
    DrawPanel(caseX + 4.0f, caseY + 5.0f, caseSize, caseSize, 4.0f, 0.0f,
              {0.0f, 0.0f, 0.04f, 0.52f * alpha}, {0.0f, 0.0f, 0.04f, 0.52f * alpha}, {0, 0, 0, 0});
    DrawPanel(caseX, caseY, caseSize, caseSize, 3.0f, 1.4f,
              {0.06f, 0.08f, 0.16f, 0.96f * alpha}, {0.03f, 0.04f, 0.10f, 0.98f * alpha},
              {0.62f, 0.72f, 0.94f, 0.85f * alpha});

    const float spineW = std::max(12.0f, caseSize * 0.10f);
    DrawPanel(caseX + 2.0f, caseY + 2.0f, spineW, caseSize - 4.0f, 1.0f, 0.0f,
              {0.08f, 0.09f, 0.14f, 0.98f * alpha}, {0.04f, 0.05f, 0.09f, 0.98f * alpha}, {0, 0, 0, 0});

    const float artX = caseX + spineW + 2.0f;
    const float artY = caseY + 3.0f;
    const float artW = caseSize - spineW - 5.0f;
    const float artH = caseSize - 6.0f;
    const auto& coverImg = GetOrCreateImage(g.coverPath, 512);
    if (coverImg.tex) {
        DrawImageAspectFit(coverImg, artX, artY, artW, artH, alpha, true);
    } else {
        DrawPanel(artX, artY, artW, artH, 2.0f, 0.0f,
                  {0.09f, 0.14f, 0.36f, 0.95f * alpha},
                  {0.03f, 0.05f, 0.18f, 0.98f * alpha}, {0, 0, 0, 0});
        DrawOpticalDisc(artX + artW * 0.5f, artY + artH * 0.44f, artW * 0.34f, disc_spin_angle_, alpha * 0.95f);
        DrawText(g.title, artX + 8.0f, artY + artH - 44.0f, 15.0f, STYLE_BODY_BOLD,
                 {1, 1, 1, 0.92f * alpha}, artW - 16.0f, 2, 1, artW - 16.0f);
    }
    // Glossy diagonal sheen across jewel case
    PushQuad(white_tex_, artX, artY, artW, artH * 0.32f,
             {1.0f, 1.0f, 1.0f, 0.12f * alpha}, {1.0f, 1.0f, 1.0f, 0.0f});

    // Metadata box below jewel case (dynamically flows below 1- or 2-line title)
    const float metaY = caseY + caseSize + 12.0f;
    const float metaX = rightX + 16.0f;
    const float metaW = rightW - 32.0f;
    float titleW = 0.0f, titleH = 0.0f;
    MeasureText(g.title, 19.5f, STYLE_BODY_BOLD, metaW, 2, titleW, titleH);
    DrawText(g.title, metaX, metaY, 19.5f, STYLE_BODY_BOLD, PrimaryTextColor(alpha), metaW, 2, 0);

    const float subY = metaY + std::min(52.0f, titleH + 6.0f);
    char subBuf[128];
    std::snprintf(
        subBuf, sizeof(subBuf), "%s  •  %s  •  %d %s",
        g.serial.empty() ? "PS1" : g.serial.c_str(),
        g.sizeStr.empty() ? "-" : g.sizeStr.c_str(),
        g.discCount,
        g.discCount > 1 ? TextOr("ps1ui_discs", "Discs").c_str() : TextOr("ps1ui_disc", "Disc").c_str());
    DrawText(subBuf, metaX, subY, 14.5f, STYLE_MONO, SecondaryTextColor(alpha), metaW, 1, 0);

    const float line3Y = subY + 23.0f;
    if (g.saveCount > 0) {
        char svBuf[64];
        std::snprintf(svBuf, sizeof(svBuf), "%s: %d",
                      TextOr("ps1ui_quick_saves_lbl", "Quick Saves").c_str(), g.saveCount);
        const RgbaColor qsCol = (settings_.theme == 1)
            ? RgbaColor{0.08f, 0.36f, 0.78f, alpha}
            : RgbaColor{0.46f, 0.88f, 1.0f, alpha};
        DrawText(svBuf, metaX, line3Y, 14.5f, STYLE_BODY_BOLD,
                 qsCol, metaW, 1, 0);
    } else if (!g.dateStr.empty()) {
        DrawText(g.dateStr, metaX, line3Y, 14.0f, STYLE_BODY, SecondaryTextColor(0.75f * alpha), metaW, 1, 0);
    }

    // Action Buttons at bottom of Right Pane
    const float btnH = 42.0f;
    const float btnY = topY + paneH - btnH - 12.0f;
    const bool hasQuickContinue = (g.latestSaveSlot >= 0);
    const float optW = 48.0f;
    const float btnGap = 8.0f;
    const float availBtnW = metaW - optW - btnGap;

    if (hasQuickContinue) {
        const float startW = std::floor((availBtnW - btnGap) * 0.48f);
        const float contW = availBtnW - btnGap - startW;
        // START
        DrawPanel(metaX, btnY, startW, btnH, 7.0f, 1.4f,
                  AccentColor(0.95f * alpha),
                  {AccentColor(1.0f).r * 0.6f, AccentColor(1.0f).g * 0.65f, AccentColor(1.0f).b * 0.85f, 0.98f * alpha},
                  {0.82f, 0.92f, 1.0f, alpha});
        float sw = 0, sh = 0;
        const std::string startLbl = TextOr("ps1ui_btn_start", "Start");
        MeasureText(startLbl, 16.5f, STYLE_BODY_BOLD, startW - 10.0f, 1, sw, sh);
        DrawText(startLbl, metaX, btnY + (btnH - sh) * 0.5f, 16.5f, STYLE_BODY_BOLD,
                 {1, 1, 1, alpha}, startW - 10.0f, 1, 1, startW);

        // CONTINUE
        const float contX = metaX + startW + btnGap;
        DrawPanel(contX, btnY, contW, btnH, 7.0f, 1.4f,
                  {0.14f, 0.58f, 0.46f, 0.95f * alpha},
                  {0.08f, 0.36f, 0.30f, 0.98f * alpha},
                  {0.58f, 1.0f, 0.84f, alpha});
        const std::string contLbl = TextOr("ps1ui_btn_continue", "Continue");
        MeasureText(contLbl, 16.5f, STYLE_BODY_BOLD, contW - 10.0f, 1, sw, sh);
        DrawText(contLbl, contX, btnY + (btnH - sh) * 0.5f, 16.5f, STYLE_BODY_BOLD,
                 {1, 1, 1, alpha}, contW - 10.0f, 1, 1, contW);

        if (overlay_ == OVERLAY_NONE) {
            RegisterHit(metaX, btnY, startW, btnH, HIT_GAME_START, game_index_);
            RegisterHit(contX, btnY, contW, btnH, HIT_GAME_CONTINUE, game_index_);
        }
    } else {
        DrawPanel(metaX, btnY, availBtnW, btnH, 7.0f, 1.4f,
                  AccentColor(0.95f * alpha),
                  {AccentColor(1.0f).r * 0.6f, AccentColor(1.0f).g * 0.65f, AccentColor(1.0f).b * 0.85f, 0.98f * alpha},
                  {0.82f, 0.92f, 1.0f, alpha});
        float sw = 0, sh = 0;
        const std::string startLbl = TextOr("ps1ui_btn_start", "Start");
        MeasureText(startLbl, 17.0f, STYLE_BODY_BOLD, availBtnW - 12.0f, 1, sw, sh);
        DrawText(startLbl, metaX, btnY + (btnH - sh) * 0.5f, 17.0f, STYLE_BODY_BOLD,
                 {1, 1, 1, alpha}, availBtnW - 12.0f, 1, 1, availBtnW);
        if (overlay_ == OVERLAY_NONE) {
            RegisterHit(metaX, btnY, availBtnW, btnH, HIT_GAME_START, game_index_);
        }
    }

    // OPTIONS icon button
    const float optX = metaX + availBtnW + btnGap;
    DrawPanel(optX, btnY, optW, btnH, 7.0f, 1.3f,
              PanelTopColor(0.90f * alpha), PanelBotColor(0.96f * alpha), PanelBorderColor(0.90f * alpha));
    float ow = 0, oh = 0;
    MeasureText("△", 18.0f, STYLE_MONO, optW, 1, ow, oh);
    DrawText("△", optX, btnY + (btnH - oh) * 0.5f, 18.0f, STYLE_MONO,
             {0.34f, 0.90f, 0.62f, alpha}, optW, 1, 1, optW);

    if (overlay_ == OVERLAY_NONE) {
        RegisterHit(optX, btnY, optW, btnH, HIT_GAME_OPTIONS, game_index_);
        RegisterHit(caseX, caseY, caseSize * 1.15f, caseSize, HIT_GAME_START, game_index_);
    }
}

std::vector<int> Ps1Renderer::FilteredSaveIndices() const {
    std::vector<int> out;
    out.reserve(saves_.size());
    for (int i = 0; i < static_cast<int>(saves_.size()); ++i) {
        if (mc_tab_ == 1 && !mc_filter_game_path_.empty()) {
            if (saves_[i].gamePath == mc_filter_game_path_) {
                out.push_back(i);
            }
        } else {
            out.push_back(i);
        }
    }
    return out;
}

void Ps1Renderer::DrawMemoryCardScreen(float alpha) {
    const LayoutMetrics m = ComputeMetrics();
    DrawHeaderPlaque(TextOr("ps1ui_menu_memory_card", "MEMORY CARD"), alpha);

    // Top 3 Sub-Tabs
    const float tabY = m.bodyTop;
    const float tabH = 36.0f;
    const float tabGap = 10.0f;
    const float usableW = screen_width_ - 2.0f * m.padX;
    const float tabW = std::min(220.0f, (usableW - 2.0f * tabGap) / 3.0f);

    std::string tabNames[3] = {
        TextOr("ps1ui_mc_tab_all", "ALL SAVES"),
        TextOr("ps1ui_mc_tab_game", "GAME SAVES"),
        TextOr("ps1ui_mc_tab_cards", "PS1 CARDS"),
    };
    for (int t = 0; t < 3; ++t) {
        const float tx = m.padX + static_cast<float>(t) * (tabW + tabGap);
        const bool active = (mc_tab_ == t);
        DrawPanel(
            tx, tabY, tabW, tabH, 7.0f, active ? 1.6f : 1.0f,
            active ? AccentColor(0.92f * alpha) : PanelTopColor(0.68f * alpha),
            active ? PanelBotColor(0.95f * alpha) : PanelBotColor(0.75f * alpha),
            active ? RgbaColor{0.84f, 0.92f, 1.0f, alpha} : PanelBorderColor(0.55f * alpha));
        float tw = 0, th = 0;
        MeasureText(tabNames[t], 15.5f, STYLE_BODY_BOLD, tabW - 14.0f, 1, tw, th);
        const RgbaColor fg = (active && settings_.theme == 1)
            ? RgbaColor{1, 1, 1, alpha}
            : PrimaryTextColor(alpha);
        DrawText(tabNames[t], tx, tabY + (tabH - th) * 0.5f, 15.5f, STYLE_BODY_BOLD,
                 fg, tabW - 14.0f, 1, 1, tabW);
        if (overlay_ == OVERLAY_NONE) {
            RegisterHit(tx, tabY, tabW, tabH, HIT_MC_TAB, t);
        }
    }

    const float paneTop = tabY + tabH + 10.0f;
    const float paneH = m.bodyBot - paneTop;

    // Tab 2: Physical 128 KiB PS1 Memory Cards View
    if (mc_tab_ == 2) {
        const float cardGap = 18.0f;
        const float slotW = (usableW - cardGap) * 0.5f;
        const float slotH = 148.0f;
        struct SlotInfo {
            std::string title;
            std::string name;
            std::string size;
            std::string status;
        };
        SlotInfo slots[2] = {
            {
                TextOr("ps1ui_mc_slot1", "MEMORY CARD SLOT 1"),
                slot1_card_name_.empty() ? TextOr("ps1ui_mc_empty_slot", "Empty") : slot1_card_name_,
                slot1_card_size_,
                slot1_card_status_,
            },
            {
                TextOr("ps1ui_mc_slot2", "MEMORY CARD SLOT 2"),
                slot2_card_name_.empty() ? TextOr("ps1ui_mc_empty_slot", "Empty") : slot2_card_name_,
                slot2_card_size_,
                slot2_card_status_,
            },
        };
        for (int s = 0; s < 2; ++s) {
            const float sx = m.padX + static_cast<float>(s) * (slotW + cardGap);
            const bool focused = (mc_card_slot_idx_ == s);
            DrawPanel(
                sx, paneTop, slotW, slotH, 10.0f, focused ? 2.0f : 1.4f,
                PanelTopColor(0.88f * alpha), PanelBotColor(0.94f * alpha),
                focused ? RgbaColor{0.85f, 0.94f, 1.0f, alpha} : PanelBorderColor(0.75f * alpha));

            // Stylized PS1 Memory Card notch detail on left
            DrawPanel(sx + 16.0f, paneTop + 18.0f, 64.0f, 82.0f, 6.0f, 1.4f,
                      {0.26f, 0.30f, 0.44f, alpha}, {0.16f, 0.19f, 0.30f, alpha},
                      {0.62f, 0.70f, 0.88f, alpha});
            DrawPanel(sx + 23.0f, paneTop + 26.0f, 50.0f, 38.0f, 3.0f, 0.8f,
                      {0.88f, 0.90f, 0.96f, alpha}, {0.72f, 0.76f, 0.86f, alpha}, {0.3f, 0.36f, 0.5f, alpha});
            float k128W = 0.0f, k128H = 0.0f;
            MeasureText("128K", 14.0f, STYLE_MONO, 48.0f, 1, k128W, k128H);
            DrawText("128K", sx + 23.0f, paneTop + 26.0f + (38.0f - k128H) * 0.5f, 14.0f, STYLE_MONO,
                     {0.10f, 0.16f, 0.38f, alpha}, 50.0f, 1, 1, 50.0f);

            const float infoX = sx + 96.0f;
            const float infoW = slotW - 112.0f;
            DrawText(slots[s].title, infoX, paneTop + 18.0f, 14.5f, STYLE_MONO, SecondaryTextColor(alpha), infoW, 1, 0);
            DrawText(slots[s].name, infoX, paneTop + 44.0f, 19.5f, STYLE_BODY_BOLD, PrimaryTextColor(alpha), infoW, 2, 0);
            DrawText(slots[s].size + (slots[s].status.empty() ? "" : ("  •  " + slots[s].status)),
                     infoX, paneTop + 102.0f, 14.5f, STYLE_BODY, SecondaryTextColor(alpha), infoW, 1, 0);
            if (overlay_ == OVERLAY_NONE) {
                RegisterHit(sx, paneTop, slotW, slotH, HIT_CARD_SLOT, s);
            }
        }

        const float listY = paneTop + slotH + 12.0f;
        const float listH = m.bodyBot - listY;
        DrawPanel(m.padX, listY, usableW, listH, 8.0f, 1.4f,
                  PanelTopColor(0.84f * alpha), PanelBotColor(0.90f * alpha), PanelBorderColor(0.75f * alpha));
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "%s (%zu)",
                      TextOr("ps1ui_mc_available_cards", "Available PS1 Memory Cards").c_str(),
                      memcards_.size());
        DrawText(hdr, m.padX + 16.0f, listY + 12.0f, 16.5f, STYLE_BODY_BOLD, PrimaryTextColor(alpha), usableW - 32.0f, 1, 0);

        const int maxShow = std::min(4, static_cast<int>(memcards_.size()));
        for (int i = 0; i < maxShow; ++i) {
            const float ry = listY + 44.0f + static_cast<float>(i) * 36.0f;
            if (ry + 30.0f > listY + listH) break;
            DrawText(memcards_[i].name, m.padX + 18.0f, ry, 16.0f, STYLE_BODY, PrimaryTextColor(alpha), usableW * 0.54f, 1, 0);
            DrawText(memcards_[i].sizeStr + "  •  " + memcards_[i].dateStr,
                     m.padX + usableW * 0.56f, ry, 14.5f, STYLE_MONO, SecondaryTextColor(alpha), usableW * 0.40f, 1, 2, usableW * 0.40f);
        }
        return;
    }

    // Tab 0 (All Saves) or Tab 1 (Game Saves): Classic PS1 15-Block Memory Card Grid + CRT Live Preview
    const std::vector<int> filtered = FilteredSaveIndices();
    const int totalFiltered = static_cast<int>(filtered.size());
    const int totalPages = std::max(1, (totalFiltered + 14) / 15);
    mc_page_ = std::clamp(mc_page_, 0, totalPages - 1);

    const float gap = 18.0f;
    const float leftW = std::round(usableW * 0.52f);
    const float rightW = usableW - leftW - gap;
    const float leftX = m.padX;
    const float rightX = leftX + leftW + gap;

    // Left Pane: Sculpted PS1 15-Block Memory Card Shell
    DrawPanel(leftX, paneTop, leftW, paneH, 10.0f, 1.8f,
              PanelTopColor(0.86f * alpha), PanelBotColor(0.92f * alpha), PanelBorderColor(0.90f * alpha));

    char mcHdr[96];
    if (mc_tab_ == 1 && !mc_filter_game_title_.empty()) {
        std::snprintf(mcHdr, sizeof(mcHdr), "%s  •  %d/%d",
                      mc_filter_game_title_.c_str(), mc_page_ + 1, totalPages);
    } else {
        std::snprintf(mcHdr, sizeof(mcHdr), "%s  •  %d %s  •  %d/%d",
                      TextOr("ps1ui_mc_blocks_title", "15-BLOCK CARD").c_str(),
                      totalFiltered,
                      TextOr("ps1ui_saves_word", "Saves").c_str(),
                      mc_page_ + 1, totalPages);
    }
    DrawText(mcHdr, leftX + 16.0f, paneTop + 10.0f, 15.5f, STYLE_MONO, PrimaryTextColor(alpha), leftW - 32.0f, 1, 0);

    // 5 cols x 3 rows on wide screens, 3 cols x 5 rows on narrow 4:3
    const int cols = (screen_width_ >= 780.0f) ? 5 : 3;
    const int rows = 15 / cols;
    const float gridX = leftX + 14.0f;
    const float gridY = paneTop + 38.0f;
    const float gridW = leftW - 28.0f;
    const float gridH = paneH - (totalPages > 1 ? 82.0f : 50.0f);
    const float cellGap = 8.0f;
    const float cellW = (gridW - cellGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const float cellH = (gridH - cellGap * static_cast<float>(rows - 1)) / static_cast<float>(rows);

    float selCx = gridX, selCy = gridY;
    for (int slot = 0; slot < 15; ++slot) {
        const int r = slot / cols;
        const int c = slot % cols;
        const float bx = gridX + static_cast<float>(c) * (cellW + cellGap);
        const float by = gridY + static_cast<float>(r) * (cellH + cellGap);

        const int filteredPos = mc_page_ * 15 + slot;
        const bool occupied = (filteredPos < totalFiltered);
        const bool focused = (mc_focus_pane_ == 0 && mc_slot_index_ == slot);

        DrawPanel(
            bx, by, cellW, cellH, 6.0f, focused ? 2.2f : 1.1f,
            occupied ? AccentColor(0.32f * alpha) : PanelTopColor(0.45f * alpha),
            occupied ? PanelBotColor(0.82f * alpha) : PanelBotColor(0.55f * alpha),
            focused ? RgbaColor{0.92f, 0.96f, 1.0f, alpha} : PanelBorderColor(occupied ? 0.72f * alpha : 0.30f * alpha));

        if (occupied) {
            const SaveEntry& sv = saves_[filtered[filteredPos]];
            const auto& thumb = GetOrCreateImage(sv.previewPath, 256);
            if (thumb.tex) {
                DrawImageAspectFit(thumb, bx + 4.0f, by + 4.0f, cellW - 8.0f, cellH - 8.0f, alpha, true);
            } else {
                DrawOpticalDisc(bx + cellW * 0.5f, by + cellH * 0.48f, std::min(cellW, cellH) * 0.30f, 0.3f, alpha * 0.85f);
            }
            // Corner slot badge (AUTO or S1..S10)
            char slotTag[16];
            if (sv.slotNum == 0) std::snprintf(slotTag, sizeof(slotTag), "AUTO");
            else std::snprintf(slotTag, sizeof(slotTag), "S%d", sv.slotNum);

            float sw = 0, sh = 0;
            MeasureText(slotTag, 12.5f, STYLE_MONO, cellW - 8.0f, 1, sw, sh);
            const float tagW = sw + 10.0f;
            const float tagH = 19.0f;
            DrawPanel(bx + cellW - tagW - 4.0f, by + cellH - tagH - 4.0f, tagW, tagH, 4.0f, 0.8f,
                      {0.02f, 0.05f, 0.16f, 0.88f * alpha}, {0.02f, 0.05f, 0.16f, 0.94f * alpha},
                      {0.58f, 0.76f, 1.0f, 0.9f * alpha});
            DrawText(slotTag, bx + cellW - tagW - 4.0f, by + cellH - tagH - 4.0f + (tagH - sh) * 0.5f,
                     12.5f, STYLE_MONO, {1, 1, 1, alpha}, tagW, 1, 1, tagW);
        } else {
            const std::string freeStr = TextOr("ps1ui_mc_free_block", "FREE");
            float fw = 0, fh = 0;
            MeasureText(freeStr, 13.0f, STYLE_MONO, cellW - 8.0f, 1, fw, fh);
            DrawText(freeStr, bx, by + (cellH - fh) * 0.5f, 13.0f, STYLE_MONO,
                     SecondaryTextColor(0.45f * alpha), cellW - 8.0f, 1, 1, cellW);
        }

        if (focused) {
            selCx = bx + 12.0f;
            selCy = by + 13.0f;
        }
        if (overlay_ == OVERLAY_NONE) {
            RegisterHit(bx, by, cellW, cellH, HIT_MC_BLOCK, slot);
        }
    }

    if (totalPages > 1) {
        const float pgY = paneTop + paneH - 36.0f;
        const float pgW = 72.0f;
        DrawPanel(leftX + 14.0f, pgY, pgW, 28.0f, 5.0f, 1.0f,
                  PanelTopColor(0.8f * alpha), PanelBotColor(0.9f * alpha), PanelBorderColor(0.75f * alpha));
        DrawText("◀", leftX + 14.0f, pgY + 4.0f, 15.0f, STYLE_MONO, PrimaryTextColor(alpha), pgW, 1, 1, pgW);
        DrawPanel(leftX + leftW - pgW - 14.0f, pgY, pgW, 28.0f, 5.0f, 1.0f,
                  PanelTopColor(0.8f * alpha), PanelBotColor(0.9f * alpha), PanelBorderColor(0.75f * alpha));
        DrawText("▶", leftX + leftW - pgW - 14.0f, pgY + 4.0f, 15.0f, STYLE_MONO, PrimaryTextColor(alpha), pgW, 1, 1, pgW);
        if (overlay_ == OVERLAY_NONE) {
            RegisterHit(leftX + 14.0f, pgY, pgW, 28.0f, HIT_MC_PAGE_PREV, 0);
            RegisterHit(leftX + leftW - pgW - 14.0f, pgY, pgW, 28.0f, HIT_MC_PAGE_NEXT, 0);
        }
    }

    // Right Pane: CRT Preview Monitor + Metadata + Command Buttons (Load / Delete / Exit)
    DrawPanel(rightX, paneTop, rightW, paneH, 10.0f, 1.8f,
              PanelTopColor(0.86f * alpha), PanelBotColor(0.92f * alpha), PanelBorderColor(0.90f * alpha));

    const float monW = rightW - 32.0f;
    const float monH = std::min(paneH * 0.48f, monW * 0.62f);
    const float monX = rightX + 16.0f;
    const float monY = paneTop + 12.0f;

    // Beveled CRT Preview Frame
    DrawPanel(monX, monY, monW, monH, 8.0f, 2.0f,
              {0.02f, 0.03f, 0.07f, 0.98f * alpha}, {0.01f, 0.02f, 0.05f, 0.99f * alpha},
              PanelBorderColor(0.88f * alpha));

    const int curFilteredIdx = mc_page_ * 15 + mc_slot_index_;
    const bool hasSelectedSave = (curFilteredIdx >= 0 && curFilteredIdx < totalFiltered);
    const SaveEntry* activeSave = hasSelectedSave ? &saves_[filtered[curFilteredIdx]] : nullptr;

    if (activeSave) {
        const auto& previewImg = GetOrCreateImage(activeSave->previewPath, 512);
        if (previewImg.tex) {
            DrawImageAspectFit(previewImg, monX + 4.0f, monY + 4.0f, monW - 8.0f, monH - 8.0f, alpha, false);
        } else {
            DrawOpticalDisc(monX + monW * 0.5f, monY + monH * 0.5f, monH * 0.34f, disc_spin_angle_, alpha * 0.9f);
        }
        // Subtle CRT glass reflection
        PushQuad(white_tex_, monX + 4.0f, monY + 4.0f, monW - 8.0f, monH * 0.25f,
                 {1, 1, 1, 0.08f * alpha}, {1, 1, 1, 0.0f});

        const float infoY = monY + monH + 10.0f;
        float itw = 0.0f, ith = 0.0f;
        MeasureText(activeSave->title, 18.5f, STYLE_BODY_BOLD, monW, 2, itw, ith);
        DrawText(activeSave->title, monX, infoY, 18.5f, STYLE_BODY_BOLD, PrimaryTextColor(alpha), monW, 2, 0);
        const float line2Y = infoY + std::min(46.0f, ith + 5.0f);
        const std::string line2 = activeSave->slotLabel +
            (activeSave->serial.empty() ? "" : ("  •  " + activeSave->serial));
        const RgbaColor slotCol = (settings_.theme == 1)
            ? RgbaColor{0.08f, 0.36f, 0.78f, alpha}
            : RgbaColor{0.48f, 0.86f, 1.0f, alpha};
        DrawText(line2, monX, line2Y, 14.5f, STYLE_MONO, slotCol, monW, 1, 0);
        const std::string line3 = activeSave->dateStr +
            (activeSave->sizeStr.empty() ? "" : ("  •  " + activeSave->sizeStr));
        DrawText(line3, monX, line2Y + 22.0f, 14.0f, STYLE_BODY, SecondaryTextColor(alpha), monW, 1, 0);
    } else {
        const std::string emptyMsg = saves_loading_
            ? TextOr("ps1ui_loading_saves", "Loading quick saves...")
            : (totalFiltered == 0
                ? TextOr("ps1ui_mc_no_saves", "No quick saves found")
                : TextOr("ps1ui_mc_empty_block", "Empty memory block"));
        float ew = 0, eh = 0;
        MeasureText(emptyMsg, 17.5f, STYLE_BODY_BOLD, monW - 24.0f, 2, ew, eh);
        DrawText(emptyMsg, monX + 12.0f, monY + (monH - eh) * 0.5f, 17.5f, STYLE_BODY_BOLD,
                 SecondaryTextColor(alpha), monW - 24.0f, 2, 1, monW - 24.0f);
    }

    // 3 Authentic PS1 Memory Card Action Buttons: LOAD | DELETE | EXIT
    const float actH = 40.0f;
    const float actY = paneTop + paneH - actH - 12.0f;
    const float actGap = 8.0f;
    const float actW = (monW - 2.0f * actGap) / 3.0f;
    const std::string actLabels[3] = {
        TextOr("ps1ui_mc_load", "LOAD"),
        TextOr("ps1ui_mc_delete", "DELETE"),
        TextOr("ps1ui_mc_exit", "EXIT"),
    };

    for (int a = 0; a < 3; ++a) {
        const float ax = monX + static_cast<float>(a) * (actW + actGap);
        const bool enabled = (a == 2) || hasSelectedSave;
        const bool focused = (mc_focus_pane_ == 1 && mc_action_idx_ == a);
        const float effAlpha = enabled ? alpha : alpha * 0.38f;
        RgbaColor topC = (a == 0) ? AccentColor(0.92f * effAlpha)
            : (a == 1 ? RgbaColor{0.58f, 0.14f, 0.22f, 0.90f * effAlpha} : PanelTopColor(0.86f * effAlpha));
        RgbaColor botC = (a == 0) ? PanelBotColor(0.96f * effAlpha)
            : (a == 1 ? RgbaColor{0.34f, 0.06f, 0.12f, 0.96f * effAlpha} : PanelBotColor(0.92f * effAlpha));
        DrawPanel(
            ax, actY, actW, actH, 7.0f, focused ? 2.2f : 1.2f,
            topC, botC,
            focused ? RgbaColor{1.0f, 1.0f, 1.0f, effAlpha} : PanelBorderColor(0.72f * effAlpha));
        float aw = 0, ah = 0;
        MeasureText(actLabels[a], 15.5f, STYLE_BODY_BOLD, actW - 10.0f, 1, aw, ah);
        const RgbaColor btnTxtCol = (a == 2) ? PrimaryTextColor(effAlpha) : RgbaColor{1, 1, 1, effAlpha};
        DrawText(actLabels[a], ax, actY + (actH - ah) * 0.5f, 15.5f, STYLE_BODY_BOLD,
                 btnTxtCol, actW - 10.0f, 1, 1, actW);
        if (focused) {
            selCx = ax + 12.0f;
            selCy = actY + actH * 0.5f;
        }
        if (overlay_ == OVERLAY_NONE && enabled) {
            RegisterHit(ax, actY, actW, actH, HIT_MC_ACTION, a);
        }
    }

    if (overlay_ == OVERLAY_NONE) {
        DrawTriangleCursor(selCx, selCy, 12.0f, alpha);
    }
}

void Ps1Renderer::DrawSettingsScreen(float alpha) {
    const LayoutMetrics m = ComputeMetrics();
    DrawHeaderPlaque(TextOr("ps1ui_menu_settings", "SETTINGS"), alpha);

    const float px = m.padX;
    const float py = m.bodyTop;
    const float pw = screen_width_ - 2.0f * m.padX;
    const float ph = m.bodyH;

    DrawPanel(px, py, pw, ph, 10.0f, 1.6f,
              PanelTopColor(0.86f * alpha), PanelBotColor(0.92f * alpha), PanelBorderColor(0.88f * alpha));

    struct SettingRowData {
        std::string label;
        std::string value;
        bool isArrowSelector;
    };

    auto themeStr = [&]() -> std::string {
        if (settings_.theme == 1) return TextOr("ps1ui_theme_psone", "PSone Silver");
        if (settings_.theme == 2) return TextOr("ps1ui_theme_slate", "Original Slate");
        return TextOr("ps1ui_theme_classic", "Classic Blue");
    };
    auto aspectStr = [&]() -> std::string {
        if (settings_.aspectMode == 1) return TextOr("ps1ui_aspect_16_9", "16:9");
        if (settings_.aspectMode == 2) return TextOr("ps1ui_aspect_4_3", "4:3 (Original)");
        return TextOr("ps1ui_aspect_auto", "Auto (Full Screen)");
    };
    auto orbsStr = [&]() -> std::string {
        if (settings_.orbsMode == 1) return TextOr("ps1ui_orbs_calm", "Calm");
        if (settings_.orbsMode == 2) return TextOr("ps1ui_orbs_static", "Static");
        return TextOr("ps1ui_orbs_normal", "Normal");
    };
    auto scanStr = [&]() -> std::string {
        if (settings_.scanlinesMode == 1) return TextOr("ps1ui_scan_subtle", "Subtle");
        if (settings_.scanlinesMode == 2) return TextOr("ps1ui_scan_classic", "Classic");
        return TextOr("ps1ui_off", "Off");
    };
    auto bootStr = [&]() -> std::string {
        if (settings_.bootAnimMode == 1) return TextOr("ps1ui_boot_fast", "Fast");
        if (settings_.bootAnimMode == 2) return TextOr("ps1ui_off", "Off");
        return TextOr("ps1ui_boot_full", "Full");
    };
    auto dateStr = [&]() -> std::string {
        if (settings_.dateFormat == 1) return "MM/DD/YYYY";
        if (settings_.dateFormat == 2) return "YYYY/MM/DD";
        return "DD/MM/YYYY";
    };

    const std::string onTxt = TextOr("ps1ui_on", "On");
    const std::string offTxt = TextOr("ps1ui_off", "Off");

    std::vector<SettingRowData> rows = {
        {TextOr("ps1ui_set_aspect", "Aspect Ratio"), aspectStr(), true},
        {TextOr("ps1ui_set_theme", "BIOS Theme"), themeStr(), true},
        {TextOr("ps1ui_set_orbs", "Background Orbs"), orbsStr(), true},
        {TextOr("ps1ui_set_scanlines", "CRT Scanlines"), scanStr(), true},
        {TextOr("ps1ui_set_boot_anim", "Boot Animation"), bootStr(), true},
        {TextOr("ps1ui_set_key_tone", "Menu Sounds"), settings_.keyTone ? onTxt : offTxt, true},
        {TextOr("ps1ui_set_ambient", "Ambient Sound"), settings_.ambientSound ? onTxt : offTxt, true},
        {TextOr("ps1ui_set_confirm_btn", "Confirm Button"),
         settings_.swapConfirm ? TextOr("ps1ui_btn_circle", "○ Circle") : TextOr("ps1ui_btn_cross", "✕ Cross"), true},
        {TextOr("ps1ui_set_touch", "Touch Controls"), settings_.touchControls ? onTxt : offTxt, true},
        {TextOr("ps1ui_set_touch_guide", "Touch Controls Guide"), TextOr("ps1ui_open", "Open"), false},
        {TextOr("ps1ui_set_time_fmt", "Clock Format"),
         settings_.time24h ? TextOr("ps1ui_time_24h", "24-Hour") : TextOr("ps1ui_time_12h", "12-Hour"), true},
        {TextOr("ps1ui_set_date_fmt", "Date Format"), dateStr(), true},
        {TextOr("ps1ui_set_keep_screen", "Keep Screen On"), settings_.keepScreenOn ? onTxt : offTxt, true},
        {TextOr("ps1ui_set_sysinfo", "System Information"), TextOr("ps1ui_view", "View"), false},
        {TextOr("ps1ui_set_restore", "Restore Defaults"), TextOr("ps1ui_reset", "Reset"), false},
    };

    const float listTop = py + 10.0f;
    const float listH = ph - 20.0f;
    const float rowH = 48.0f;
    const float rowGap = 5.0f;
    const float rowStep = rowH + rowGap;

    const float totalH = static_cast<float>(rows.size()) * rowStep - rowGap;
    const float maxScroll = std::max(0.0f, totalH - listH);
    settings_scroll_target_y_ = std::clamp(settings_scroll_target_y_, 0.0f, maxScroll);
    settings_scroll_y_ += (settings_scroll_target_y_ - settings_scroll_y_) * 0.25f;

    FlushQuadBatch();
    const float sxScale = static_cast<float>(view_width_) / screen_width_;
    const float syScale = static_cast<float>(view_height_) / kScreenHeight;
    const int scX = view_x_ + static_cast<int>(std::floor((px + 6.0f) * sxScale));
    const int scY = view_y_ + static_cast<int>(std::floor((kScreenHeight - (listTop + listH)) * syScale));
    const int scW = std::max(1, static_cast<int>(std::ceil((pw - 18.0f) * sxScale)));
    const int scH = std::max(1, static_cast<int>(std::ceil(listH * syScale)));
    glEnable(GL_SCISSOR_TEST);
    glScissor(scX, scY, scW, scH);

    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const float ry = listTop + static_cast<float>(i) * rowStep - settings_scroll_y_;
        if (ry + rowH < listTop - 4.0f || ry > listTop + listH + 4.0f) continue;

        const bool selected = (i == settings_index_);
        const float rx = px + 14.0f;
        const float rw = pw - 34.0f;

        if (selected) {
            DrawPanel(rx, ry, rw, rowH, 7.0f, 1.6f,
                      AccentColor(0.90f * alpha),
                      {AccentColor(1.0f).r * 0.62f, AccentColor(1.0f).g * 0.65f, AccentColor(1.0f).b * 0.84f, 0.94f * alpha},
                      {0.84f, 0.92f, 1.0f, alpha});
        } else {
            DrawPanel(rx, ry, rw, rowH, 7.0f, 0.0f,
                      PanelTopColor(0.30f * alpha), PanelBotColor(0.36f * alpha), {0, 0, 0, 0});
        }

        const float valAreaW = std::min(280.0f, rw * 0.40f);
        const float valAreaX = rx + rw - valAreaW - 10.0f;
        const float lblMaxW = std::max(140.0f, valAreaX - (rx + 32.0f) - 10.0f);
        const RgbaColor lblCol = (selected && settings_.theme == 1)
            ? RgbaColor{1, 1, 1, alpha} : PrimaryTextColor(alpha);
        float tw = 0, th = 0;
        MeasureText(rows[i].label, 18.0f, selected ? STYLE_BODY_BOLD : STYLE_BODY, lblMaxW, 1, tw, th);
        DrawText(rows[i].label, rx + 32.0f, ry + (rowH - th) * 0.5f, 18.0f,
                 selected ? STYLE_BODY_BOLD : STYLE_BODY, lblCol, lblMaxW, 1, 0);

        float vw = 0, vh = 0;
        MeasureText(rows[i].value, 17.0f, STYLE_BODY_BOLD, valAreaW - 60.0f, 1, vw, vh);
        DrawText(rows[i].value, valAreaX + 30.0f, ry + (rowH - vh) * 0.5f, 17.0f, STYLE_BODY_BOLD,
                 selected ? RgbaColor{0.92f, 0.98f, 1.0f, alpha} : SecondaryTextColor(alpha),
                 valAreaW - 60.0f, 1, 1, valAreaW - 60.0f);

        if (rows[i].isArrowSelector) {
            float aw = 0, ah = 0;
            MeasureText("◀", 15.5f, STYLE_MONO, 26.0f, 1, aw, ah);
            const float arrowY = ry + (rowH - ah) * 0.5f;
            DrawText("◀", valAreaX, arrowY, 15.5f, STYLE_MONO, SecondaryTextColor(alpha), 26.0f, 1, 1, 26.0f);
            DrawText("▶", valAreaX + valAreaW - 26.0f, arrowY, 15.5f, STYLE_MONO,
                     SecondaryTextColor(alpha), 26.0f, 1, 1, 26.0f);
        }

        if (selected && overlay_ == OVERLAY_NONE) {
            DrawTriangleCursor(rx + 15.0f, ry + rowH * 0.5f, 12.0f, alpha);
        }

        if (overlay_ == OVERLAY_NONE) {
            const float ht = std::max(listTop, ry);
            const float hb = std::min(listTop + listH, ry + rowH);
            if (hb > ht + 4.0f) {
                if (rows[i].isArrowSelector) {
                    RegisterHit(valAreaX - 6.0f, ht, 36.0f, hb - ht, HIT_SETTING_LEFT, i);
                    RegisterHit(valAreaX + valAreaW - 30.0f, ht, 36.0f, hb - ht, HIT_SETTING_RIGHT, i);
                }
                RegisterHit(rx, ht, rw, hb - ht, HIT_SETTING_ROW, i);
            }
        }
    }
    FlushQuadBatch();
    glDisable(GL_SCISSOR_TEST);

    if (maxScroll > 1.0f) {
        const float sbX = px + pw - 11.0f;
        const float sbY = listTop + 2.0f;
        const float sbH = listH - 4.0f;
        DrawPanel(sbX, sbY, 4.0f, sbH, 2.0f, 0.0f,
                  {0.1f, 0.14f, 0.35f, 0.5f * alpha}, {0.1f, 0.14f, 0.35f, 0.5f * alpha}, {0, 0, 0, 0});
        const float thumbH = std::max(26.0f, sbH * (listH / totalH));
        const float thumbY = sbY + (sbH - thumbH) * (settings_scroll_y_ / maxScroll);
        DrawPanel(sbX, thumbY, 4.0f, thumbH, 2.0f, 0.0f,
                  PanelBorderColor(alpha), AccentColor(alpha), {0, 0, 0, 0});
    }
}

void Ps1Renderer::DrawLaunchStage() {
    const float inFade = SmoothStep(0.0f, 0.22f, launch_t_);
    const float outFade = SmoothStep(0.84f, 1.0f, launch_t_);
    const float uiAlpha = inFade * (1.0f - outFade);

    PushQuad(white_tex_, 0.0f, 0.0f, screen_width_, kScreenHeight,
             {0.015f, 0.02f, 0.055f, inFade}, {0.01f, 0.01f, 0.03f, inFade});

    if (uiAlpha <= 0.01f) return;

    const float cx = screen_width_ * 0.5f;
    const float cy = kScreenHeight * 0.42f;
    const float spin = launch_t_ * 14.0f;
    DrawOpticalDisc(cx + 58.0f, cy, 96.0f, spin, uiAlpha);

    const auto& cov = GetOrCreateImage(launch_cover_, 512);
    const float boxS = 172.0f;
    const float bx = cx - boxS * 0.72f;
    const float by = cy - boxS * 0.50f;
    DrawPanel(bx, by, boxS, boxS, 6.0f, 2.0f,
              {0.06f, 0.08f, 0.18f, uiAlpha}, {0.03f, 0.04f, 0.10f, uiAlpha},
              {0.58f, 0.72f, 1.0f, uiAlpha});
    if (cov.tex) {
        DrawImageAspectFit(cov, bx + 4.0f, by + 4.0f, boxS - 8.0f, boxS - 8.0f, uiAlpha, true);
    } else {
        DrawOpticalDisc(bx + boxS * 0.5f, by + boxS * 0.5f, boxS * 0.36f, -spin * 0.5f, uiAlpha);
    }

    float tw = 0, th = 0;
    MeasureText(launch_title_, 24.0f, STYLE_HEADER, screen_width_ * 0.78f, 1, tw, th);
    DrawText(launch_title_, cx - tw * 0.5f, cy + 108.0f, 24.0f, STYLE_HEADER,
             {1, 1, 1, uiAlpha}, screen_width_ * 0.78f, 1, 0);

    const std::string statusMsg = launch_is_save_
        ? TextOr("ps1ui_launching_save", "LOADING SAVE STATE...")
        : TextOr("ps1ui_launching_disc", "LOADING DISC...");
    float sw = 0, sh = 0;
    MeasureText(statusMsg, 14.5f, STYLE_MONO, 400.0f, 1, sw, sh);
    DrawText(statusMsg, cx - sw * 0.5f, cy + 144.0f, 14.5f, STYLE_MONO,
             {0.55f, 0.80f, 1.0f, uiAlpha}, 400.0f, 1, 0);

    // Progress bar
    const float pbW = 280.0f;
    const float pbH = 8.0f;
    const float pbX = cx - pbW * 0.5f;
    const float pbY = cy + 174.0f;
    DrawPanel(pbX, pbY, pbW, pbH, 4.0f, 1.0f,
              {0.08f, 0.10f, 0.22f, uiAlpha}, {0.06f, 0.08f, 0.18f, uiAlpha},
              {0.34f, 0.48f, 0.90f, uiAlpha});
    DrawPanel(pbX + 2.0f, pbY + 2.0f, std::max(4.0f, (pbW - 4.0f) * Clamp01(launch_t_ * 1.15f)),
              pbH - 4.0f, 2.0f, 0.0f,
              {0.42f, 0.72f, 1.0f, uiAlpha}, {0.26f, 0.48f, 1.0f, uiAlpha}, {0, 0, 0, 0});
}

void Ps1Renderer::DrawConfirmDialog(
    const std::string& title,
    const std::string& message,
    const std::string& yesText,
    const std::string& noText,
    int focusedBtn) {
    PushQuad(white_tex_, 0, 0, screen_width_, kScreenHeight, {0.0f, 0.0f, 0.04f, 0.62f});
    RegisterHit(0, 0, screen_width_, kScreenHeight, HIT_OVERLAY_BACKDROP, 0);

    const float dw = std::min(490.0f, screen_width_ - 52.0f);
    const float dh = 224.0f;
    const float dx = (screen_width_ - dw) * 0.5f;
    const float dy = (kScreenHeight - dh) * 0.5f;

    DrawPanel(dx, dy, dw, dh, 10.0f, 2.0f,
              PanelTopColor(0.96f), PanelBotColor(0.98f), PanelBorderColor(1.0f));
    DrawText(title, dx + 24.0f, dy + 22.0f, 21.5f, STYLE_HEADER, PrimaryTextColor(1.0f), dw - 48.0f, 1, 1, dw - 48.0f);
    DrawText(message, dx + 26.0f, dy + 68.0f, 16.5f, STYLE_BODY, SecondaryTextColor(1.0f), dw - 52.0f, 3, 1, dw - 52.0f);

    const float bw = (dw - 68.0f) * 0.5f;
    const float bh = 44.0f;
    const float by = dy + dh - bh - 20.0f;
    const std::string labels[2] = {yesText, noText};
    for (int i = 0; i < 2; ++i) {
        const float bx = dx + 24.0f + static_cast<float>(i) * (bw + 20.0f);
        const bool foc = (focusedBtn == i);
        DrawPanel(bx, by, bw, bh, 7.0f, foc ? 2.2f : 1.2f,
                  foc ? AccentColor(0.96f) : PanelTopColor(0.85f),
                  foc ? PanelBotColor(0.98f) : PanelBotColor(0.92f),
                  foc ? RgbaColor{1, 1, 1, 1} : PanelBorderColor(0.75f));
        float tw = 0, th = 0;
        MeasureText(labels[i], 17.0f, STYLE_BODY_BOLD, bw - 16.0f, 1, tw, th);
        const RgbaColor tc = (foc && settings_.theme == 1) ? RgbaColor{1, 1, 1, 1} : PrimaryTextColor(1.0f);
        DrawText(labels[i], bx, by + (bh - th) * 0.5f, 17.0f, STYLE_BODY_BOLD, tc, bw - 16.0f, 1, 1, bw);
        RegisterHit(bx, by, bw, bh, HIT_OVERLAY_ITEM, i);
        if (foc) {
            DrawTriangleCursor(bx + 15.0f, by + bh * 0.5f, 12.0f, 1.0f);
        }
    }
}

void Ps1Renderer::DrawOverlay() {
    if (overlay_ == OVERLAY_NONE) return;

    if (overlay_ == OVERLAY_EXIT_CONFIRM) {
        DrawConfirmDialog(
            TextOr("ps1ui_exit_title", "EXIT PS1 MODE"),
            TextOr("ps1ui_exit_msg", "Return to the standard game library?"),
            TextOr("ps1ui_yes", "Yes"),
            TextOr("ps1ui_no", "No"),
            dialog_btn_);
        return;
    }
    if (overlay_ == OVERLAY_DELETE_CONFIRM) {
        DrawConfirmDialog(
            TextOr("ps1ui_del_title", "DELETE QUICK SAVE"),
            TextOr("ps1ui_del_msg", "Permanently delete this quick save state?"),
            TextOr("ps1ui_yes", "Yes"),
            TextOr("ps1ui_no", "No"),
            dialog_btn_);
        return;
    }
    if (overlay_ == OVERLAY_RESTORE_CONFIRM) {
        DrawConfirmDialog(
            TextOr("ps1ui_restore_title", "RESTORE DEFAULTS"),
            TextOr("ps1ui_restore_msg", "Reset all PS1 BIOS interface settings to defaults?"),
            TextOr("ps1ui_yes", "Yes"),
            TextOr("ps1ui_no", "No"),
            dialog_btn_);
        return;
    }

    PushQuad(white_tex_, 0, 0, screen_width_, kScreenHeight, {0.0f, 0.0f, 0.04f, 0.62f});
    RegisterHit(0, 0, screen_width_, kScreenHeight, HIT_OVERLAY_BACKDROP, 0);

    if (overlay_ == OVERLAY_GAME_OPTIONS && game_index_ < static_cast<int>(games_.size())) {
        const GameEntry& g = games_[game_index_];
        std::vector<std::string> opts;
        opts.push_back(TextOr("ps1ui_opt_start", "Start Game"));
        if (g.latestSaveSlot >= 0) {
            opts.push_back(TextOr("ps1ui_opt_continue", "Continue Latest Save"));
        }
        opts.push_back(TextOr("ps1ui_opt_saves", "Quick Saves"));
        opts.push_back(TextOr("ps1ui_opt_info", "Game Information"));
        opts.push_back(TextOr("ps1ui_opt_settings", "Game Settings"));

        const float dw = std::min(440.0f, screen_width_ - 56.0f);
        const float rowH = 46.0f;
        const float rowStep = 52.0f;
        const float dh = 78.0f + static_cast<float>(opts.size()) * rowStep;
        const float dx = (screen_width_ - dw) * 0.5f;
        const float dy = (kScreenHeight - dh) * 0.5f;

        DrawPanel(dx, dy, dw, dh, 10.0f, 2.0f,
                  PanelTopColor(0.96f), PanelBotColor(0.98f), PanelBorderColor(1.0f));
        DrawText(g.title, dx + 20.0f, dy + 16.0f, 20.0f, STYLE_HEADER, PrimaryTextColor(1.0f), dw - 40.0f, 1, 1, dw - 40.0f);

        options_index_ = std::clamp(options_index_, 0, static_cast<int>(opts.size()) - 1);
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
            const float oy = dy + 58.0f + static_cast<float>(i) * rowStep;
            const bool foc = (i == options_index_);
            DrawPanel(dx + 18.0f, oy, dw - 36.0f, rowH, 7.0f, foc ? 1.8f : 1.0f,
                      foc ? AccentColor(0.94f) : PanelTopColor(0.65f),
                      foc ? PanelBotColor(0.96f) : PanelBotColor(0.75f),
                      foc ? RgbaColor{1, 1, 1, 1} : PanelBorderColor(0.55f));
            float ow = 0, oh = 0;
            MeasureText(opts[i], 17.5f, STYLE_BODY_BOLD, dw - 76.0f, 1, ow, oh);
            const RgbaColor tc = (foc && settings_.theme == 1) ? RgbaColor{1, 1, 1, 1} : PrimaryTextColor(1.0f);
            DrawText(opts[i], dx + 50.0f, oy + (rowH - oh) * 0.5f, 17.5f, STYLE_BODY_BOLD, tc, dw - 76.0f, 1, 0);
            RegisterHit(dx + 18.0f, oy, dw - 36.0f, rowH, HIT_OVERLAY_ITEM, i);
            if (foc) DrawTriangleCursor(dx + 32.0f, oy + rowH * 0.5f, 12.0f, 1.0f);
        }
        return;
    }

    if (overlay_ == OVERLAY_DISC_PICKER && game_index_ < static_cast<int>(games_.size())) {
        const GameEntry& g = games_[game_index_];
        const int count = static_cast<int>(g.discPaths.size());
        const float dw = std::min(440.0f, screen_width_ - 56.0f);
        const float rowH = 46.0f;
        const float rowStep = 52.0f;
        const float dh = 78.0f + static_cast<float>(std::min(count, 6)) * rowStep;
        const float dx = (screen_width_ - dw) * 0.5f;
        const float dy = (kScreenHeight - dh) * 0.5f;

        DrawPanel(dx, dy, dw, dh, 10.0f, 2.0f,
                  PanelTopColor(0.96f), PanelBotColor(0.98f), PanelBorderColor(1.0f));
        DrawText(TextOr("ps1ui_select_disc", "SELECT DISC"), dx + 20.0f, dy + 16.0f, 20.5f, STYLE_HEADER,
                 PrimaryTextColor(1.0f), dw - 40.0f, 1, 1, dw - 40.0f);

        disc_picker_index_ = std::clamp(disc_picker_index_, 0, std::max(0, count - 1));
        for (int i = 0; i < std::min(count, 6); ++i) {
            const float oy = dy + 58.0f + static_cast<float>(i) * rowStep;
            const bool foc = (i == disc_picker_index_);
            const std::string lbl = (i < static_cast<int>(g.discLabels.size()))
                ? g.discLabels[i] : (TextOr("ps1ui_disc", "Disc") + " " + std::to_string(i + 1));
            DrawPanel(dx + 18.0f, oy, dw - 36.0f, rowH, 7.0f, foc ? 1.8f : 1.0f,
                      foc ? AccentColor(0.94f) : PanelTopColor(0.65f),
                      foc ? PanelBotColor(0.96f) : PanelBotColor(0.75f),
                      foc ? RgbaColor{1, 1, 1, 1} : PanelBorderColor(0.55f));
            float lw = 0, lh = 0;
            MeasureText(lbl, 17.5f, STYLE_BODY_BOLD, dw - 76.0f, 1, lw, lh);
            const RgbaColor tc = (foc && settings_.theme == 1) ? RgbaColor{1, 1, 1, 1} : PrimaryTextColor(1.0f);
            DrawText(lbl, dx + 50.0f, oy + (rowH - lh) * 0.5f, 17.5f, STYLE_BODY_BOLD, tc, dw - 76.0f, 1, 0);
            RegisterHit(dx + 18.0f, oy, dw - 36.0f, rowH, HIT_OVERLAY_ITEM, i);
            if (foc) DrawTriangleCursor(dx + 32.0f, oy + rowH * 0.5f, 12.0f, 1.0f);
        }
        return;
    }

    if (overlay_ == OVERLAY_GAME_INFO && game_index_ < static_cast<int>(games_.size())) {
        const GameEntry& g = games_[game_index_];
        const float dw = std::min(540.0f, screen_width_ - 48.0f);
        const float dh = 304.0f;
        const float dx = (screen_width_ - dw) * 0.5f;
        const float dy = (kScreenHeight - dh) * 0.5f;

        DrawPanel(dx, dy, dw, dh, 10.0f, 2.0f,
                  PanelTopColor(0.96f), PanelBotColor(0.98f), PanelBorderColor(1.0f));
        DrawText(TextOr("ps1ui_opt_info", "Game Information"), dx + 22.0f, dy + 18.0f, 21.5f, STYLE_HEADER,
                 PrimaryTextColor(1.0f), dw - 44.0f, 1, 1, dw - 44.0f);

        auto drawKV = [&](float rowY, const std::string& k, const std::string& v) {
            DrawText(k, dx + 24.0f, rowY, 16.0f, STYLE_BODY_BOLD, SecondaryTextColor(1.0f), 170.0f, 1, 0);
            DrawText(v.empty() ? "-" : v, dx + 198.0f, rowY, 16.0f, STYLE_BODY, PrimaryTextColor(1.0f), dw - 222.0f, 1, 0);
        };
        drawKV(dy + 68.0f, TextOr("ps1ui_info_title", "Title"), g.title);
        drawKV(dy + 102.0f, TextOr("ps1ui_info_serial", "Serial"), g.serial);
        drawKV(dy + 136.0f, TextOr("ps1ui_info_size", "File Size"), g.sizeStr);
        drawKV(dy + 170.0f, TextOr("ps1ui_info_discs", "Discs"), std::to_string(g.discCount));
        drawKV(dy + 204.0f, TextOr("ps1ui_quick_saves_lbl", "Quick Saves"), std::to_string(g.saveCount));

        const float bw = 166.0f;
        const float bx = dx + (dw - bw) * 0.5f;
        const float by = dy + dh - 56.0f;
        DrawPanel(bx, by, bw, 40.0f, 7.0f, 1.5f, AccentColor(0.95f), PanelBotColor(0.98f), {1, 1, 1, 1});
        float cw = 0, ch = 0;
        const std::string closeLbl = TextOr("ps1ui_close", "Close");
        MeasureText(closeLbl, 16.5f, STYLE_BODY_BOLD, bw, 1, cw, ch);
        DrawText(closeLbl, bx, by + (40.0f - ch) * 0.5f, 16.5f, STYLE_BODY_BOLD,
                 {1, 1, 1, 1}, bw, 1, 1, bw);
        RegisterHit(bx, by, bw, 40.0f, HIT_OVERLAY_ITEM, 0);
        return;
    }

    if (overlay_ == OVERLAY_SYSTEM_INFO) {
        const float dw = std::min(550.0f, screen_width_ - 48.0f);
        const float dh = 334.0f;
        const float dx = (screen_width_ - dw) * 0.5f;
        const float dy = (kScreenHeight - dh) * 0.5f;
        DrawPanel(dx, dy, dw, dh, 10.0f, 2.0f,
                  PanelTopColor(0.96f), PanelBotColor(0.98f), PanelBorderColor(1.0f));
        DrawText(TextOr("ps1ui_set_sysinfo", "System Information"), dx + 22.0f, dy + 18.0f, 21.5f, STYLE_HEADER,
                 PrimaryTextColor(1.0f), dw - 44.0f, 1, 1, dw - 44.0f);

        auto drawKV = [&](float rowY, const std::string& k, const std::string& v) {
            DrawText(k, dx + 24.0f, rowY, 15.5f, STYLE_BODY_BOLD, SecondaryTextColor(1.0f), 186.0f, 1, 0);
            DrawText(v.empty() ? "-" : v, dx + 214.0f, rowY, 15.5f, STYLE_MONO, PrimaryTextColor(1.0f), dw - 238.0f, 1, 0);
        };
        drawKV(dy + 68.0f, TextOr("ps1ui_sys_app", "Version"), "EmuCoreR " + status_.appVersion);
        drawKV(dy + 102.0f, TextOr("ps1ui_sys_device", "Device"), status_.deviceName);
        drawKV(dy + 136.0f, TextOr("ps1ui_sys_bios", "BIOS"), status_.biosStatus);
        drawKV(dy + 170.0f, TextOr("ps1ui_sys_storage", "Free Storage"), status_.freeStorage);
        drawKV(dy + 204.0f, TextOr("ps1ui_menu_games", "Games"), std::to_string(games_.size()));
        drawKV(dy + 238.0f, TextOr("ps1ui_quick_saves_lbl", "Quick Saves"), std::to_string(saves_.size()));

        const float bw = 166.0f;
        const float bx = dx + (dw - bw) * 0.5f;
        const float by = dy + dh - 56.0f;
        DrawPanel(bx, by, bw, 40.0f, 7.0f, 1.5f, AccentColor(0.95f), PanelBotColor(0.98f), {1, 1, 1, 1});
        float cw = 0, ch = 0;
        const std::string closeLbl = TextOr("ps1ui_close", "Close");
        MeasureText(closeLbl, 16.5f, STYLE_BODY_BOLD, bw, 1, cw, ch);
        DrawText(closeLbl, bx, by + (40.0f - ch) * 0.5f, 16.5f, STYLE_BODY_BOLD,
                 {1, 1, 1, 1}, bw, 1, 1, bw);
        RegisterHit(bx, by, bw, 40.0f, HIT_OVERLAY_ITEM, 0);
        return;
    }

    if (overlay_ == OVERLAY_TOUCH_GUIDE) {
        const float dw = std::min(560.0f, screen_width_ - 44.0f);
        const float dh = 318.0f;
        const float dx = (screen_width_ - dw) * 0.5f;
        const float dy = (kScreenHeight - dh) * 0.5f;
        DrawPanel(dx, dy, dw, dh, 10.0f, 2.0f,
                  PanelTopColor(0.96f), PanelBotColor(0.98f), PanelBorderColor(1.0f));
        DrawText(TextOr("ps1ui_touch_guide_title", "TOUCH CONTROLS"), dx + 22.0f, dy + 18.0f, 21.5f, STYLE_HEADER,
                 PrimaryTextColor(1.0f), dw - 44.0f, 1, 1, dw - 44.0f);

        const std::string tips[4] = {
            TextOr("ps1ui_touch_tip_1", "• Tap any item or block to focus, tap again to open or load."),
            TextOr("ps1ui_touch_tip_2", "• Drag vertically on game or settings lists to scroll smoothly."),
            TextOr("ps1ui_touch_tip_3", "• Long-press a game or tap △ Options for Continue & Game Info."),
            TextOr("ps1ui_touch_tip_4", "• Tap with two fingers anywhere to go Back."),
        };
        for (int i = 0; i < 4; ++i) {
            DrawText(tips[i], dx + 26.0f, dy + 66.0f + static_cast<float>(i) * 46.0f,
                     16.0f, STYLE_BODY, PrimaryTextColor(1.0f), dw - 52.0f, 2, 0);
        }
        const float bw = 196.0f;
        const float bx = dx + (dw - bw) * 0.5f;
        const float by = dy + dh - 56.0f;
        DrawPanel(bx, by, bw, 40.0f, 7.0f, 1.5f, AccentColor(0.95f), PanelBotColor(0.98f), {1, 1, 1, 1});
        float gw = 0, gh = 0;
        const std::string gotItLbl = TextOr("ps1ui_got_it", "Got It");
        MeasureText(gotItLbl, 16.5f, STYLE_BODY_BOLD, bw, 1, gw, gh);
        DrawText(gotItLbl, bx, by + (40.0f - gh) * 0.5f, 16.5f, STYLE_BODY_BOLD,
                 {1, 1, 1, 1}, bw, 1, 1, bw);
        RegisterHit(bx, by, bw, 40.0f, HIT_OVERLAY_ITEM, 0);
    }
}

void Ps1Renderer::DrawScanlinesPass() {
    if (settings_.scanlinesMode <= 0) return;
    const float strength = (settings_.scanlinesMode == 1) ? 0.14f : 0.28f;
    FlushQuadBatch();
    glUseProgram(prog_scan_);
    glUniform2f(scan_u_viewport_, static_cast<float>(view_width_), static_cast<float>(view_height_));
    glUniform1f(scan_u_strength_, strength);
    glBindVertexArray(fs_vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Ps1Renderer::Draw() {
    if (!gl_ready_) return;

    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (has_frame_tp_) {
        dt = std::chrono::duration<float>(now - last_frame_tp_).count();
        dt = std::clamp(dt, 0.001f, 0.05f);
    }
    last_frame_tp_ = now;
    has_frame_tp_ = true;
    time_sec_ += dt;
    disc_spin_angle_ += dt * 1.45f;

    if (image_cache_dirty_) {
        image_cache_dirty_ = false;
        ClearImageCache();
    }

    // Responsive aspect calculation (1:1 virtual square pixels matching EmuCoreA)
    if (settings_.aspectMode == 1) {
        screen_width_ = 960.0f; // 16:9
    } else if (settings_.aspectMode == 2) {
        screen_width_ = 720.0f; // 4:3 Original
    } else {
        const float surfAspect = static_cast<float>(surface_width_) / static_cast<float>(std::max(1, surface_height_));
        screen_width_ = std::clamp(kScreenHeight * surfAspect, 720.0f, 1280.0f);
    }

    const float scale = std::min(
        static_cast<float>(surface_width_) / screen_width_,
        static_cast<float>(surface_height_) / kScreenHeight);
    view_width_ = std::max(1, static_cast<int>(std::lround(screen_width_ * scale)));
    view_height_ = std::max(1, static_cast<int>(std::lround(kScreenHeight * scale)));
    view_x_ = (surface_width_ - view_width_) / 2;
    view_y_ = (surface_height_ - view_height_) / 2;

    raster_scale_ = std::clamp(scale, 1.35f, 3.65f);
    if (std::fabs(raster_scale_ - last_raster_scale_) > 0.25f) {
        last_raster_scale_ = raster_scale_;
        ClearTextCache();
    }

    // Clear full window black (clean pillarbox/letterbox if 4:3 or 16:9 selected)
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, surface_width_, surface_height_);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(view_x_, view_y_, view_width_, view_height_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw background shader + 3D spheres (pure smooth continuous float, independent of cursor/screen)
    const float motionScale = (settings_.orbsMode == 2) ? 0.0f : (settings_.orbsMode == 1 ? 0.35f : 1.0f);
    glUseProgram(prog_bg_);
    glUniform2f(bg_u_screen_, screen_width_, kScreenHeight);
    glUniform1f(bg_u_time_, time_sec_);
    glUniform1f(bg_u_motion_, motionScale);
    glUniform1i(bg_u_theme_, settings_.theme);
    glUniform2f(bg_u_parallax_, 0.0f, 0.0f);
    glBindVertexArray(fs_vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    hit_targets_.clear();
    BeginQuadBatch();

    if (launching_) {
        launch_t_ = std::min(1.0f, launch_t_ + dt / 1.35f);
        DrawLaunchStage();
        if (launch_t_ >= 1.0f && !launch_dispatched_) {
            launch_dispatched_ = true;
            if (launch_is_save_) {
                HostNotifySaveLaunch(launch_path_, launch_save_slot_);
            } else {
                HostNotifyGameLaunch(launch_path_);
            }
        }
        FlushQuadBatch();
        DrawScanlinesPass();
        return;
    }

    if (boot_active_) {
        if (!boot_sfx_played_) {
            boot_sfx_played_ = true;
            HostPlaySfx(SFX_BOOT_INTRO);
        }
        boot_t_ += dt / std::max(0.2f, boot_duration_);
        if (boot_t_ >= 1.0f) {
            boot_active_ = false;
            screen_fade_t_ = 0.55f;
        }
    }

    screen_fade_t_ = std::min(1.0f, screen_fade_t_ + dt * 4.5f);
    const float uiAlpha = boot_active_
        ? SmoothStep(0.76f, 1.0f, boot_t_)
        : EaseOutCubic(screen_fade_t_);

    if (uiAlpha > 0.01f) {
        DrawStatusBar(uiAlpha);
        if (screen_ == SCREEN_MAIN_MENU) {
            DrawMainMenu(uiAlpha);
        } else if (screen_ == SCREEN_GAMES) {
            DrawGamesScreen(uiAlpha);
        } else if (screen_ == SCREEN_MEMORY_CARD) {
            DrawMemoryCardScreen(uiAlpha);
        } else if (screen_ == SCREEN_SETTINGS) {
            DrawSettingsScreen(uiAlpha);
        }
        DrawBottomBar(uiAlpha);
        DrawOverlay();
    }

    if (boot_active_) {
        DrawBootIntro(1.0f);
    }

    FlushQuadBatch();
    DrawScanlinesPass();
}

void Ps1Renderer::RegisterHit(float x, float y, float w, float h, int kind, int index) {
    hit_targets_.push_back({x, y, w, h, kind, index});
}

const Ps1Renderer::HitTarget* Ps1Renderer::FindHit(float vx, float vy) const {
    for (auto it = hit_targets_.rbegin(); it != hit_targets_.rend(); ++it) {
        if (vx >= it->x && vx <= it->x + it->w && vy >= it->y && vy <= it->y + it->h) {
            return &(*it);
        }
    }
    return nullptr;
}

bool Ps1Renderer::SurfaceNormToVirtual(float normX, float normY, float& outVx, float& outVy) const {
    const float px = normX * static_cast<float>(surface_width_);
    const float py = normY * static_cast<float>(surface_height_);
    outVx = ((px - static_cast<float>(view_x_)) / static_cast<float>(std::max(1, view_width_))) * screen_width_;
    outVy = ((py - static_cast<float>(view_y_)) / static_cast<float>(std::max(1, view_height_))) * kScreenHeight;
    return outVx >= 0.0f && outVx <= screen_width_ && outVy >= 0.0f && outVy <= kScreenHeight;
}

void Ps1Renderer::SwitchScreen(int nextScreen) {
    if (screen_ == nextScreen) return;
    screen_ = nextScreen;
    screen_fade_t_ = 0.0f;
    cursor_pos_init_ = false;
    overlay_ = OVERLAY_NONE;
}

void Ps1Renderer::StartLaunchGame(
    const std::string& path,
    const std::string& title,
    const std::string& serial,
    const std::string& coverPath) {
    if (path.empty() || launching_) return;
    launching_ = true;
    launch_is_save_ = false;
    launch_dispatched_ = false;
    launch_t_ = 0.0f;
    launch_path_ = path;
    launch_title_ = title;
    launch_serial_ = serial;
    launch_cover_ = coverPath;
    HostPlaySfx(SFX_DISC_LAUNCH);
}

void Ps1Renderer::StartLaunchQuickSave(
    const std::string& path,
    int slot,
    const std::string& title,
    const std::string& serial,
    const std::string& coverPath) {
    if (path.empty() || launching_) return;
    launching_ = true;
    launch_is_save_ = true;
    launch_save_slot_ = slot;
    launch_dispatched_ = false;
    launch_t_ = 0.0f;
    launch_path_ = path;
    launch_title_ = title;
    launch_serial_ = serial;
    launch_cover_ = coverPath;
    HostPlaySfx(SFX_DISC_LAUNCH);
}

void Ps1Renderer::StartLaunchSave(int filteredIndex) {
    std::vector<int> filtered = FilteredSaveIndices();
    if (filteredIndex < 0 || filteredIndex >= static_cast<int>(filtered.size())) {
        HostPlaySfx(SFX_ERROR);
        return;
    }
    const SaveEntry& sv = saves_[filtered[filteredIndex]];
    if (sv.gamePath.empty()) {
        HostPlaySfx(SFX_ERROR);
        return;
    }
    StartLaunchQuickSave(sv.gamePath, sv.slotNum, sv.title, sv.serial, sv.previewPath);
}

void Ps1Renderer::ActivateGameLaunch(int gameIdx) {
    if (gameIdx < 0 || gameIdx >= static_cast<int>(games_.size())) {
        HostPlaySfx(SFX_ERROR);
        return;
    }
    const GameEntry& g = games_[gameIdx];
    if (g.discCount > 1 && g.discPaths.size() > 1) {
        disc_picker_index_ = 0;
        overlay_ = OVERLAY_DISC_PICKER;
        HostPlaySfx(SFX_CONFIRM);
    } else {
        StartLaunchGame(g.path, g.title, g.serial, g.coverPath);
    }
}

void Ps1Renderer::PushSettingsToHost() {
    int32_t arr[12] = {
        settings_.theme,
        settings_.aspectMode,
        settings_.orbsMode,
        settings_.scanlinesMode,
        settings_.bootAnimMode,
        settings_.keyTone ? 1 : 0,
        settings_.ambientSound ? 1 : 0,
        settings_.swapConfirm ? 1 : 0,
        settings_.touchControls ? 1 : 0,
        settings_.time24h ? 1 : 0,
        settings_.dateFormat,
        settings_.keepScreenOn ? 1 : 0,
    };
    HostNotifySettingsChanged(arr, 12);
}

void Ps1Renderer::CycleSetting(int row, int delta) {
    auto wrap = [delta](int val, int count) -> int {
        return ((val + delta) % count + count) % count;
    };
    bool changed = true;
    switch (row) {
        case 0: settings_.aspectMode = wrap(settings_.aspectMode, 3); break;
        case 1: settings_.theme = wrap(settings_.theme, 3); break;
        case 2: settings_.orbsMode = wrap(settings_.orbsMode, 3); break;
        case 3: settings_.scanlinesMode = wrap(settings_.scanlinesMode, 3); break;
        case 4: settings_.bootAnimMode = wrap(settings_.bootAnimMode, 3); break;
        case 5: settings_.keyTone = !settings_.keyTone; break;
        case 6: settings_.ambientSound = !settings_.ambientSound; break;
        case 7: settings_.swapConfirm = !settings_.swapConfirm; break;
        case 8: settings_.touchControls = !settings_.touchControls; break;
        case 9:
            changed = false;
            overlay_ = OVERLAY_TOUCH_GUIDE;
            break;
        case 10: settings_.time24h = !settings_.time24h; break;
        case 11: settings_.dateFormat = wrap(settings_.dateFormat, 3); break;
        case 12: settings_.keepScreenOn = !settings_.keepScreenOn; break;
        case 13:
            changed = false;
            overlay_ = OVERLAY_SYSTEM_INFO;
            break;
        case 14:
            changed = false;
            dialog_btn_ = 1;
            overlay_ = OVERLAY_RESTORE_CONFIRM;
            break;
        default:
            changed = false;
            break;
    }
    HostPlaySfx(SFX_CONFIRM);
    if (changed) {
        PushSettingsToHost();
    }
}

void Ps1Renderer::ClampSelectionIndices() {
    main_index_ = std::clamp(main_index_, 0, 2);
    if (!games_.empty()) {
        game_index_ = std::clamp(game_index_, 0, static_cast<int>(games_.size()) - 1);
    } else {
        game_index_ = 0;
    }
    mc_slot_index_ = std::clamp(mc_slot_index_, 0, 14);
    settings_index_ = std::clamp(settings_index_, 0, kSettingsCount - 1);
}

void Ps1Renderer::OnKey(int key) {
    if (launching_) return;
    if (boot_active_) {
        if (key == KEY_CONFIRM || key == KEY_BACK || key == KEY_START) {
            boot_active_ = false;
            screen_fade_t_ = 0.4f;
        }
        return;
    }

    // Handle active overlays first
    if (overlay_ != OVERLAY_NONE) {
        if (overlay_ == OVERLAY_EXIT_CONFIRM ||
            overlay_ == OVERLAY_DELETE_CONFIRM ||
            overlay_ == OVERLAY_RESTORE_CONFIRM) {
            if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) {
                dialog_btn_ = 1 - dialog_btn_;
                HostPlaySfx(SFX_MOVE);
            } else if (key == KEY_CONFIRM) {
                if (dialog_btn_ == 0) {
                    HostPlaySfx(SFX_CONFIRM);
                    if (overlay_ == OVERLAY_EXIT_CONFIRM) {
                        overlay_ = OVERLAY_NONE;
                        HostNotifyExitRequested();
                    } else if (overlay_ == OVERLAY_DELETE_CONFIRM) {
                        int delIdx = pending_delete_save_idx_;
                        overlay_ = OVERLAY_NONE;
                        pending_delete_save_idx_ = -1;
                        if (delIdx >= 0 && delIdx < static_cast<int>(saves_.size())) {
                            HostNotifyDeleteSave(delIdx);
                        }
                    } else if (overlay_ == OVERLAY_RESTORE_CONFIRM) {
                        settings_ = ShellSettings{};
                        overlay_ = OVERLAY_NONE;
                        PushSettingsToHost();
                    }
                } else {
                    HostPlaySfx(SFX_CANCEL);
                    overlay_ = OVERLAY_NONE;
                }
            } else if (key == KEY_BACK) {
                HostPlaySfx(SFX_CANCEL);
                overlay_ = OVERLAY_NONE;
            }
            return;
        }

        if (overlay_ == OVERLAY_GAME_OPTIONS) {
            if (game_index_ >= static_cast<int>(games_.size())) {
                overlay_ = OVERLAY_NONE;
                return;
            }
            const GameEntry& g = games_[game_index_];
            const bool hasCont = (g.latestSaveSlot >= 0);
            const int optCount = hasCont ? 5 : 4;
            if (key == KEY_UP) {
                options_index_ = (options_index_ - 1 + optCount) % optCount;
                HostPlaySfx(SFX_MOVE);
            } else if (key == KEY_DOWN) {
                options_index_ = (options_index_ + 1) % optCount;
                HostPlaySfx(SFX_MOVE);
            } else if (key == KEY_BACK || key == KEY_OPTIONS) {
                overlay_ = OVERLAY_NONE;
                HostPlaySfx(SFX_CANCEL);
            } else if (key == KEY_CONFIRM) {
                int sel = options_index_;
                overlay_ = OVERLAY_NONE;
                if (sel == 0) {
                    ActivateGameLaunch(game_index_);
                } else if (hasCont && sel == 1) {
                    StartLaunchQuickSave(g.path, g.latestSaveSlot, g.title, g.serial, g.coverPath);
                } else {
                    int rem = sel - (hasCont ? 2 : 1);
                    if (rem == 0) {
                        mc_filter_game_path_ = g.path;
                        mc_filter_game_title_ = g.title;
                        mc_tab_ = 1;
                        mc_page_ = 0;
                        mc_slot_index_ = 0;
                        mc_focus_pane_ = 0;
                        return_screen_ = SCREEN_GAMES;
                        SwitchScreen(SCREEN_MEMORY_CARD);
                        HostPlaySfx(SFX_CONFIRM);
                    } else if (rem == 1) {
                        overlay_ = OVERLAY_GAME_INFO;
                        HostPlaySfx(SFX_CONFIRM);
                    } else if (rem == 2) {
                        HostPlaySfx(SFX_CONFIRM);
                        HostNotifyOpenGameSettings(game_index_);
                    }
                }
            }
            return;
        }

        if (overlay_ == OVERLAY_DISC_PICKER) {
            if (game_index_ >= static_cast<int>(games_.size())) {
                overlay_ = OVERLAY_NONE;
                return;
            }
            const GameEntry& g = games_[game_index_];
            const int cnt = std::min(6, static_cast<int>(g.discPaths.size()));
            if (cnt <= 0) {
                overlay_ = OVERLAY_NONE;
                return;
            }
            if (key == KEY_UP) {
                disc_picker_index_ = (disc_picker_index_ - 1 + cnt) % cnt;
                HostPlaySfx(SFX_MOVE);
            } else if (key == KEY_DOWN) {
                disc_picker_index_ = (disc_picker_index_ + 1) % cnt;
                HostPlaySfx(SFX_MOVE);
            } else if (key == KEY_BACK) {
                overlay_ = OVERLAY_NONE;
                HostPlaySfx(SFX_CANCEL);
            } else if (key == KEY_CONFIRM) {
                std::string path = g.discPaths[disc_picker_index_];
                std::string lbl = g.title;
                if (disc_picker_index_ < static_cast<int>(g.discLabels.size())) {
                    lbl += " (" + g.discLabels[disc_picker_index_] + ")";
                }
                overlay_ = OVERLAY_NONE;
                StartLaunchGame(path, lbl, g.serial, g.coverPath);
            }
            return;
        }

        // Info / Touch guide modals
        if (key == KEY_CONFIRM || key == KEY_BACK) {
            overlay_ = OVERLAY_NONE;
            HostPlaySfx(SFX_CANCEL);
        }
        return;
    }

    // Main Menu
    if (screen_ == SCREEN_MAIN_MENU) {
        if (key == KEY_UP) {
            main_index_ = (main_index_ + 2) % 3;
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_DOWN) {
            main_index_ = (main_index_ + 1) % 3;
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_CONFIRM || key == KEY_START) {
            HostPlaySfx(SFX_CONFIRM);
            return_screen_ = SCREEN_MAIN_MENU;
            if (main_index_ == 0) {
                SwitchScreen(SCREEN_GAMES);
            } else if (main_index_ == 1) {
                mc_tab_ = 0;
                mc_page_ = 0;
                mc_slot_index_ = 0;
                mc_focus_pane_ = 0;
                mc_filter_game_path_.clear();
                SwitchScreen(SCREEN_MEMORY_CARD);
            } else {
                SwitchScreen(SCREEN_SETTINGS);
            }
        } else if (key == KEY_BACK) {
            dialog_btn_ = 1;
            overlay_ = OVERLAY_EXIT_CONFIRM;
            HostPlaySfx(SFX_CANCEL);
        }
        return;
    }

    // Games Screen
    if (screen_ == SCREEN_GAMES) {
        if (key == KEY_BACK) {
            SwitchScreen(SCREEN_MAIN_MENU);
            HostPlaySfx(SFX_CANCEL);
            return;
        }
        if (games_.empty()) return;
        const int n = static_cast<int>(games_.size());
        auto ensureGameVisible = [&]() {
            const float rowStep = 59.0f;
            const float viewH = ComputeMetrics().bodyH - 44.0f;
            const float itemTop = static_cast<float>(game_index_) * rowStep;
            const float itemBot = itemTop + 54.0f;
            if (itemTop < game_scroll_target_y_) {
                game_scroll_target_y_ = itemTop;
            } else if (itemBot > game_scroll_target_y_ + viewH) {
                game_scroll_target_y_ = itemBot - viewH;
            }
        };
        if (key == KEY_UP) {
            game_index_ = (game_index_ - 1 + n) % n;
            ensureGameVisible();
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_DOWN) {
            game_index_ = (game_index_ + 1) % n;
            ensureGameVisible();
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_PREV_TAB) {
            game_index_ = std::max(0, game_index_ - 6);
            ensureGameVisible();
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_NEXT_TAB) {
            game_index_ = std::min(n - 1, game_index_ + 6);
            ensureGameVisible();
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_CONFIRM || key == KEY_START) {
            ActivateGameLaunch(game_index_);
        } else if (key == KEY_SECONDARY) {
            const GameEntry& g = games_[game_index_];
            if (g.latestSaveSlot >= 0) {
                StartLaunchQuickSave(g.path, g.latestSaveSlot, g.title, g.serial, g.coverPath);
            } else {
                HostPlaySfx(SFX_ERROR);
            }
        } else if (key == KEY_OPTIONS) {
            options_index_ = 0;
            overlay_ = OVERLAY_GAME_OPTIONS;
            HostPlaySfx(SFX_CONFIRM);
        }
        return;
    }

    // Memory Card Screen
    if (screen_ == SCREEN_MEMORY_CARD) {
        if (key == KEY_BACK) {
            if (mc_focus_pane_ == 1) {
                mc_focus_pane_ = 0;
                HostPlaySfx(SFX_CANCEL);
            } else {
                SwitchScreen(return_screen_);
                HostPlaySfx(SFX_CANCEL);
            }
            return;
        }
        if (key == KEY_PREV_TAB) {
            mc_tab_ = (mc_tab_ + 2) % 3;
            mc_focus_pane_ = 0;
            mc_page_ = 0;
            HostPlaySfx(SFX_MOVE);
            return;
        }
        if (key == KEY_NEXT_TAB) {
            mc_tab_ = (mc_tab_ + 1) % 3;
            mc_focus_pane_ = 0;
            mc_page_ = 0;
            HostPlaySfx(SFX_MOVE);
            return;
        }
        if (mc_tab_ == 2) {
            if (key == KEY_LEFT || key == KEY_RIGHT) {
                mc_card_slot_idx_ = 1 - mc_card_slot_idx_;
                HostPlaySfx(SFX_MOVE);
            }
            return;
        }

        const std::vector<int> filtered = FilteredSaveIndices();
        const int totalFiltered = static_cast<int>(filtered.size());
        const int totalPages = std::max(1, (totalFiltered + 14) / 15);
        const int cols = (screen_width_ >= 780.0f) ? 5 : 3;
        const int curFilteredPos = mc_page_ * 15 + mc_slot_index_;
        const bool hasSave = (curFilteredPos >= 0 && curFilteredPos < totalFiltered);

        if (mc_focus_pane_ == 0) {
            const int col = mc_slot_index_ % cols;
            if (key == KEY_LEFT) {
                if (col > 0) {
                    mc_slot_index_--;
                    HostPlaySfx(SFX_MOVE);
                } else if (mc_page_ > 0) {
                    mc_page_--;
                    mc_slot_index_ += (cols - 1);
                    HostPlaySfx(SFX_MOVE);
                }
            } else if (key == KEY_RIGHT) {
                if (col < cols - 1) {
                    mc_slot_index_++;
                    HostPlaySfx(SFX_MOVE);
                } else {
                    mc_focus_pane_ = 1;
                    mc_action_idx_ = hasSave ? 0 : 2;
                    HostPlaySfx(SFX_MOVE);
                }
            } else if (key == KEY_UP) {
                if (mc_slot_index_ >= cols) {
                    mc_slot_index_ -= cols;
                    HostPlaySfx(SFX_MOVE);
                }
            } else if (key == KEY_DOWN) {
                if (mc_slot_index_ + cols < 15) {
                    mc_slot_index_ += cols;
                    HostPlaySfx(SFX_MOVE);
                }
            } else if (key == KEY_CONFIRM || key == KEY_START) {
                if (hasSave) {
                    if (key == KEY_START) {
                        StartLaunchSave(curFilteredPos);
                    } else {
                        mc_focus_pane_ = 1;
                        mc_action_idx_ = 0;
                        HostPlaySfx(SFX_CONFIRM);
                    }
                } else {
                    HostPlaySfx(SFX_ERROR);
                }
            } else if (key == KEY_SECONDARY && hasSave) {
                pending_delete_save_idx_ = filtered[curFilteredPos];
                dialog_btn_ = 1;
                overlay_ = OVERLAY_DELETE_CONFIRM;
                HostPlaySfx(SFX_CONFIRM);
            }
        } else {
            if (key == KEY_LEFT) {
                if (mc_action_idx_ > 0) {
                    mc_action_idx_--;
                    HostPlaySfx(SFX_MOVE);
                } else {
                    mc_focus_pane_ = 0;
                    HostPlaySfx(SFX_MOVE);
                }
            } else if (key == KEY_RIGHT) {
                if (mc_action_idx_ < 2) {
                    mc_action_idx_++;
                    HostPlaySfx(SFX_MOVE);
                }
            } else if (key == KEY_UP) {
                mc_focus_pane_ = 0;
                HostPlaySfx(SFX_MOVE);
            } else if (key == KEY_CONFIRM) {
                if (mc_action_idx_ == 0 && hasSave) {
                    StartLaunchSave(curFilteredPos);
                } else if (mc_action_idx_ == 1 && hasSave) {
                    pending_delete_save_idx_ = filtered[curFilteredPos];
                    dialog_btn_ = 1;
                    overlay_ = OVERLAY_DELETE_CONFIRM;
                    HostPlaySfx(SFX_CONFIRM);
                } else if (mc_action_idx_ == 2) {
                    SwitchScreen(return_screen_);
                    HostPlaySfx(SFX_CANCEL);
                } else {
                    HostPlaySfx(SFX_ERROR);
                }
            }
        }
        (void)totalPages;
        return;
    }

    // Settings Screen
    if (screen_ == SCREEN_SETTINGS) {
        if (key == KEY_BACK) {
            SwitchScreen(SCREEN_MAIN_MENU);
            HostPlaySfx(SFX_CANCEL);
            return;
        }
        auto ensureSettingVisible = [&]() {
            const float rowStep = 53.0f;
            const float viewH = ComputeMetrics().bodyH - 20.0f;
            const float top = static_cast<float>(settings_index_) * rowStep;
            const float bot = top + 48.0f;
            if (top < settings_scroll_target_y_) settings_scroll_target_y_ = top;
            else if (bot > settings_scroll_target_y_ + viewH) settings_scroll_target_y_ = bot - viewH;
        };
        if (key == KEY_UP) {
            settings_index_ = (settings_index_ - 1 + kSettingsCount) % kSettingsCount;
            ensureSettingVisible();
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_DOWN) {
            settings_index_ = (settings_index_ + 1) % kSettingsCount;
            ensureSettingVisible();
            HostPlaySfx(SFX_MOVE);
        } else if (key == KEY_LEFT) {
            CycleSetting(settings_index_, -1);
        } else if (key == KEY_RIGHT || key == KEY_CONFIRM) {
            CycleSetting(settings_index_, 1);
        }
    }
}

void Ps1Renderer::OnTouchDown(float normX, float normY) {
    float vx = 0.0f, vy = 0.0f;
    SurfaceNormToVirtual(normX, normY, vx, vy);
    touch_down_vx_ = vx;
    touch_down_vy_ = vy;
    touch_last_vy_ = vy;
    touch_moved_ = false;
    touch_dragging_ = false;
    touch_drag_target_ = 0;

    if (overlay_ == OVERLAY_NONE) {
        if (screen_ == SCREEN_GAMES && vx < screen_width_ * 0.58f) {
            touch_drag_target_ = 1;
        } else if (screen_ == SCREEN_SETTINGS) {
            touch_drag_target_ = 2;
        }
    }
}

void Ps1Renderer::OnTouchMove(float normX, float normY) {
    float vx = 0.0f, vy = 0.0f;
    SurfaceNormToVirtual(normX, normY, vx, vy);
    const float dyTotal = vy - touch_down_vy_;
    const float dxTotal = vx - touch_down_vx_;
    if (!touch_moved_ && (std::fabs(dyTotal) > 10.0f || std::fabs(dxTotal) > 10.0f)) {
        touch_moved_ = true;
    }
    if (touch_drag_target_ != 0 && std::fabs(dyTotal) > 10.0f) {
        touch_dragging_ = true;
        const float delta = touch_last_vy_ - vy;
        if (touch_drag_target_ == 1) {
            game_scroll_target_y_ += delta;
            game_scroll_y_ = game_scroll_target_y_;
        } else if (touch_drag_target_ == 2) {
            settings_scroll_target_y_ += delta;
            settings_scroll_y_ = settings_scroll_target_y_;
        }
    }
    touch_last_vy_ = vy;
}

void Ps1Renderer::OnTouchUp(float normX, float normY) {
    const bool wasDragging = touch_moved_ || touch_dragging_;
    touch_dragging_ = false;
    touch_moved_ = false;
    touch_drag_target_ = 0;
    if (wasDragging || launching_) return;

    if (boot_active_) {
        boot_active_ = false;
        screen_fade_t_ = 0.4f;
        return;
    }

    float vx = 0.0f, vy = 0.0f;
    if (!SurfaceNormToVirtual(normX, normY, vx, vy)) return;
    const HitTarget* hit = FindHit(vx, vy);
    if (!hit) return;

    switch (hit->kind) {
        case HIT_OVERLAY_BACKDROP:
            overlay_ = OVERLAY_NONE;
            HostPlaySfx(SFX_CANCEL);
            break;

        case HIT_OVERLAY_ITEM:
            if (overlay_ == OVERLAY_EXIT_CONFIRM ||
                overlay_ == OVERLAY_DELETE_CONFIRM ||
                overlay_ == OVERLAY_RESTORE_CONFIRM) {
                dialog_btn_ = hit->index;
                OnKey(KEY_CONFIRM);
            } else if (overlay_ == OVERLAY_GAME_OPTIONS) {
                options_index_ = hit->index;
                OnKey(KEY_CONFIRM);
            } else if (overlay_ == OVERLAY_DISC_PICKER) {
                disc_picker_index_ = hit->index;
                OnKey(KEY_CONFIRM);
            } else {
                overlay_ = OVERLAY_NONE;
                HostPlaySfx(SFX_CANCEL);
            }
            break;

        case HIT_BOTTOM_ACTION:
            OnKey(hit->index);
            break;

        case HIT_MAIN_ROW:
            main_index_ = hit->index;
            OnKey(KEY_CONFIRM);
            break;

        case HIT_GAME_ROW:
            if (game_index_ == hit->index) {
                ActivateGameLaunch(hit->index);
            } else {
                game_index_ = hit->index;
                HostPlaySfx(SFX_MOVE);
            }
            break;

        case HIT_GAME_START:
            ActivateGameLaunch(hit->index);
            break;

        case HIT_GAME_CONTINUE:
            if (hit->index >= 0 && hit->index < static_cast<int>(games_.size())) {
                const GameEntry& g = games_[hit->index];
                if (g.latestSaveSlot >= 0) {
                    StartLaunchQuickSave(g.path, g.latestSaveSlot, g.title, g.serial, g.coverPath);
                }
            }
            break;

        case HIT_GAME_OPTIONS:
            game_index_ = hit->index;
            options_index_ = 0;
            overlay_ = OVERLAY_GAME_OPTIONS;
            HostPlaySfx(SFX_CONFIRM);
            break;

        case HIT_MC_TAB:
            mc_tab_ = hit->index;
            mc_focus_pane_ = 0;
            mc_page_ = 0;
            HostPlaySfx(SFX_MOVE);
            break;

        case HIT_MC_BLOCK: {
            const std::vector<int> filtered = FilteredSaveIndices();
            const int filteredPos = mc_page_ * 15 + hit->index;
            const bool sameSelected = (mc_focus_pane_ == 0 && mc_slot_index_ == hit->index);
            mc_focus_pane_ = 0;
            mc_slot_index_ = hit->index;
            if (filteredPos < static_cast<int>(filtered.size())) {
                if (sameSelected) {
                    StartLaunchSave(filteredPos);
                } else {
                    HostPlaySfx(SFX_MOVE);
                }
            } else {
                HostPlaySfx(SFX_MOVE);
            }
            break;
        }

        case HIT_MC_ACTION: {
            const std::vector<int> filtered = FilteredSaveIndices();
            const int filteredPos = mc_page_ * 15 + mc_slot_index_;
            mc_focus_pane_ = 1;
            mc_action_idx_ = hit->index;
            if (hit->index == 0 && filteredPos < static_cast<int>(filtered.size())) {
                StartLaunchSave(filteredPos);
            } else if (hit->index == 1 && filteredPos < static_cast<int>(filtered.size())) {
                pending_delete_save_idx_ = filtered[filteredPos];
                dialog_btn_ = 1;
                overlay_ = OVERLAY_DELETE_CONFIRM;
                HostPlaySfx(SFX_CONFIRM);
            } else if (hit->index == 2) {
                SwitchScreen(return_screen_);
                HostPlaySfx(SFX_CANCEL);
            }
            break;
        }

        case HIT_MC_PAGE_PREV:
            if (mc_page_ > 0) {
                mc_page_--;
                HostPlaySfx(SFX_MOVE);
            }
            break;

        case HIT_MC_PAGE_NEXT: {
            const int totalPages = std::max(1, (static_cast<int>(FilteredSaveIndices().size()) + 14) / 15);
            if (mc_page_ < totalPages - 1) {
                mc_page_++;
                HostPlaySfx(SFX_MOVE);
            }
            break;
        }

        case HIT_CARD_SLOT:
            mc_card_slot_idx_ = hit->index;
            HostPlaySfx(SFX_MOVE);
            break;

        case HIT_SETTING_LEFT:
            settings_index_ = hit->index;
            CycleSetting(hit->index, -1);
            break;

        case HIT_SETTING_RIGHT:
        case HIT_SETTING_ROW:
            settings_index_ = hit->index;
            CycleSetting(hit->index, 1);
            break;

        default:
            break;
    }
}

void Ps1Renderer::OnTouchCancel() {
    touch_dragging_ = false;
    touch_moved_ = false;
    touch_drag_target_ = 0;
}

void Ps1Renderer::OnTouchTwoFingerBack() {
    OnKey(KEY_BACK);
}

void Ps1Renderer::OnTouchLongPress(float normX, float normY) {
    if (boot_active_ || launching_ || overlay_ != OVERLAY_NONE) return;
    float vx = 0.0f, vy = 0.0f;
    if (!SurfaceNormToVirtual(normX, normY, vx, vy)) return;
    const HitTarget* hit = FindHit(vx, vy);
    if (!hit) return;
    if (hit->kind == HIT_GAME_ROW) {
        game_index_ = hit->index;
        options_index_ = 0;
        overlay_ = OVERLAY_GAME_OPTIONS;
        HostPlaySfx(SFX_CONFIRM);
    } else if (hit->kind == HIT_MC_BLOCK) {
        const std::vector<int> filtered = FilteredSaveIndices();
        const int filteredPos = mc_page_ * 15 + hit->index;
        if (filteredPos < static_cast<int>(filtered.size())) {
            mc_slot_index_ = hit->index;
            pending_delete_save_idx_ = filtered[filteredPos];
            dialog_btn_ = 1;
            overlay_ = OVERLAY_DELETE_CONFIRM;
            HostPlaySfx(SFX_CONFIRM);
        }
    }
}

void Ps1Renderer::SetStrings(std::unordered_map<std::string, std::string> strings) {
    strings_ = std::move(strings);
    ClearTextCache();
}

void Ps1Renderer::SetGames(std::vector<GameEntry> games) {
    games_ = std::move(games);
    image_cache_dirty_ = true;
    ClampSelectionIndices();
}

void Ps1Renderer::SetSaves(std::vector<SaveEntry> saves) {
    saves_ = std::move(saves);
    image_cache_dirty_ = true;
    ClampSelectionIndices();
}

void Ps1Renderer::SetMemoryCards(
    std::string slot1Name,
    std::string slot1Size,
    std::string slot1Status,
    std::string slot2Name,
    std::string slot2Size,
    std::string slot2Status,
    std::vector<MemoryCardEntry> cards) {
    slot1_card_name_ = std::move(slot1Name);
    slot1_card_size_ = std::move(slot1Size);
    slot1_card_status_ = std::move(slot1Status);
    slot2_card_name_ = std::move(slot2Name);
    slot2_card_size_ = std::move(slot2Size);
    slot2_card_status_ = std::move(slot2Status);
    memcards_ = std::move(cards);
}

void Ps1Renderer::SetSettings(const ShellSettings& settings) {
    settings_ = settings;
    settings_.theme = std::clamp(settings_.theme, 0, 2);
    settings_.aspectMode = std::clamp(settings_.aspectMode, 0, 2);
    settings_.orbsMode = std::clamp(settings_.orbsMode, 0, 2);
    settings_.scanlinesMode = std::clamp(settings_.scanlinesMode, 0, 2);
    settings_.bootAnimMode = std::clamp(settings_.bootAnimMode, 0, 2);
    settings_.dateFormat = std::clamp(settings_.dateFormat, 0, 2);
}

void Ps1Renderer::SetSystemStatus(const SystemStatus& status) {
    status_ = status;
}

void Ps1Renderer::SetTouchMode(bool active, bool showTutorial) {
    touch_mode_active_ = active;
    if (showTutorial && overlay_ == OVERLAY_NONE && !boot_active_) {
        overlay_ = OVERLAY_TOUCH_GUIDE;
    }
}

void Ps1Renderer::SetGamesLoading(bool loading) {
    games_loading_ = loading;
}

void Ps1Renderer::SetSavesLoading(bool loading) {
    saves_loading_ = loading;
}

void Ps1Renderer::TriggerTransition(int kind) {
    launching_ = false;
    launch_dispatched_ = false;
    launch_t_ = 0.0f;
    boot_sfx_played_ = false;

    if (kind == 1 || settings_.bootAnimMode == 2) {
        // Returned from a game OR Boot Animation Off: quick crisp fade straight to menu
        boot_active_ = false;
        screen_fade_t_ = 0.15f;
    } else {
        boot_active_ = true;
        boot_t_ = 0.0f;
        boot_duration_ = (settings_.bootAnimMode == 1) ? 0.85f : 1.85f;
        screen_fade_t_ = 1.0f;
    }
}

std::vector<int> Ps1Renderer::GetState() const {
    return {screen_, main_index_, game_index_, mc_slot_index_};
}

void Ps1Renderer::SetState(int screen, int mainIndex, int gameIndex, int saveIndex) {
    screen_ = std::clamp(screen, 0, 3);
    main_index_ = std::clamp(mainIndex, 0, 2);
    game_index_ = std::max(0, gameIndex);
    mc_slot_index_ = std::clamp(saveIndex, 0, 14);
    ClampSelectionIndices();
    cursor_pos_init_ = false;
}

} // namespace ps1ui

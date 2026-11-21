// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EmuCoreR GPU Accuracy Oracle:
//   Golden reference : Software (Single-Threaded, ecr_GPU_UseThread = false)
//   Targets checked  :
//     1) SoftwareThreaded (Software, ecr_GPU_UseThread = true)  -> 100% bit-exact required
//     2) HardwareOpenGL   (OpenGL ES 3.0 headless EGL PBuffer)  -> Exact %, +-1-LSB %, MAE, PSNR (dB)
#include "mips_emitter.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ecr_oracle;

namespace {

struct RgbImage {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<uint8_t> rgb;       // width * height * 3 (R8G8B8 top-to-bottom)
  std::vector<uint8_t> raw_frame; // raw callback buffer (for bit-exact SW vs ThreadedSW check)
  uint64_t raw_hash = 0;
};

struct GpuEnv {
  std::string sys_dir;
  std::string renderer;
  bool use_thread = false;
  retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_RGB565;
  retro_hw_render_callback hw_cb{};
  bool hw_requested = false;
  GLuint fbo = 0;
  GLuint color_tex = 0;
  GLuint depth_rb = 0;
  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  EGLContext egl_context = EGL_NO_CONTEXT;
  RgbImage capture;
};

GpuEnv* g_gpu = nullptr;

inline uint8_t Expand5To8(uint32_t v5) {
  return static_cast<uint8_t>((v5 << 3) | (v5 >> 2));
}

bool InitHeadlessEgl(GpuEnv& env) {
  env.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (env.egl_display == EGL_NO_DISPLAY) {
    std::fprintf(stderr, "EGL: eglGetDisplay failed\n");
    return false;
  }
  EGLint major = 0, minor = 0;
  if (eglInitialize(env.egl_display, &major, &minor) != EGL_TRUE) {
    std::fprintf(stderr, "EGL: eglInitialize failed (0x%x)\n", eglGetError());
    return false;
  }
  const EGLint cfg_attribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24,
    EGL_NONE
  };
  EGLConfig cfg = nullptr;
  EGLint num_cfg = 0;
  if (eglChooseConfig(env.egl_display, cfg_attribs, &cfg, 1, &num_cfg) != EGL_TRUE || num_cfg < 1) {
    std::fprintf(stderr, "EGL: eglChooseConfig failed (0x%x)\n", eglGetError());
    return false;
  }
  const EGLint pb_attribs[] = {EGL_WIDTH, 1024, EGL_HEIGHT, 512, EGL_NONE};
  env.egl_surface = eglCreatePbufferSurface(env.egl_display, cfg, pb_attribs);
  if (env.egl_surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "EGL: eglCreatePbufferSurface failed (0x%x)\n", eglGetError());
    return false;
  }
  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  env.egl_context = eglCreateContext(env.egl_display, cfg, EGL_NO_CONTEXT, ctx_attribs);
  if (env.egl_context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "EGL: eglCreateContext failed (0x%x)\n", eglGetError());
    return false;
  }
  if (eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context) != EGL_TRUE) {
    std::fprintf(stderr, "EGL: eglMakeCurrent failed (0x%x)\n", eglGetError());
    return false;
  }

  glGenTextures(1, &env.color_tex);
  glBindTexture(GL_TEXTURE_2D, env.color_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenRenderbuffers(1, &env.depth_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, env.depth_rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1024, 512);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glGenFramebuffers(1, &env.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, env.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, env.color_tex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, env.depth_rb);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr, "GLES3: FBO incomplete (0x%x)\n", status);
    return false;
  }
  return true;
}

void ShutdownHeadlessEgl(GpuEnv& env) {
  if (env.egl_display != EGL_NO_DISPLAY) {
    if (env.egl_context != EGL_NO_CONTEXT && env.egl_surface != EGL_NO_SURFACE) {
      eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
      if (env.fbo) glDeleteFramebuffers(1, &env.fbo);
      if (env.depth_rb) glDeleteRenderbuffers(1, &env.depth_rb);
      if (env.color_tex) glDeleteTextures(1, &env.color_tex);
    }
    eglMakeCurrent(env.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (env.egl_context != EGL_NO_CONTEXT) eglDestroyContext(env.egl_display, env.egl_context);
    if (env.egl_surface != EGL_NO_SURFACE) eglDestroySurface(env.egl_display, env.egl_surface);
    eglTerminate(env.egl_display);
  }
  env.fbo = env.color_tex = env.depth_rb = 0;
  env.egl_display = EGL_NO_DISPLAY;
  env.egl_surface = EGL_NO_SURFACE;
  env.egl_context = EGL_NO_CONTEXT;
}

bool EnvironmentCallback(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *static_cast<const char**>(data) = g_gpu->sys_dir.c_str();
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* var = static_cast<retro_variable*>(data);
      const std::string key(var->key ? var->key : "");
      const char* val = nullptr;
      if (key == "ecr_GPU_Renderer") val = g_gpu->renderer.c_str();
      else if (key == "ecr_GPU_UseThread") val = g_gpu->use_thread ? "true" : "false";
      else if (key == "ecr_GPU_ResolutionScale") val = "1";
      else if (key == "ecr_GPU_TextureFilter") val = "Nearest";
      else if (key == "ecr_GPU_TrueColor") val = "false";
      else if (key == "ecr_GPU_ScaledDithering") val = "false";
      else if (key == "ecr_GPU_MSAA") val = "1";
      else if (key == "ecr_GPU_PGXPEnable") val = "false";
      else if (key == "ecr_GPU_UseSoftwareRendererForReadbacks") val = "false";
      else if (key == "ecr_Display_CropMode") val = "None";
      else if (key == "ecr_Display_AspectRatio") val = "4:3";
      else if (key == "ecr_CPU_ExecutionMode") val = "Interpreter";
      else if (key == "ecr_Console_Region") val = "NTSC-U";
      else if (key == "ecr_BIOS_PathNTSCU") val = "gpu_oracle_bios.bin";
      else if (key == "ecr_MemoryCards_Card1Type" || key == "ecr_MemoryCards_Card2Type") val = "None";
      else if (key == "ecr_BIOS_PatchFastBoot" || key == "ecr_Main_ApplyGameSettings") val = "false";
      else if (key == "ecr_CDROM_ReadThread" || key == "ecr_Audio_FastHook") val = "false";
      var->value = val;
      return val != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *static_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      if (!data) return false;
      g_gpu->pixel_format = *static_cast<const retro_pixel_format*>(data);
      return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
      if (!data) return false;
      *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGLES_VERSION;
      return g_gpu->renderer == "OpenGL";
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      if (!data || g_gpu->renderer != "OpenGL") return false;
      auto* cb = static_cast<retro_hw_render_callback*>(data);
      if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
          cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION) {
        return false;
      }
      cb->get_proc_address = [](const char* sym) -> retro_proc_address_t {
        return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym));
      };
      cb->get_current_framebuffer = []() -> uintptr_t {
        return static_cast<uintptr_t>(g_gpu->fbo);
      };
      cb->depth = true;
      cb->stencil = false;
      g_gpu->hw_cb = *cb;
      g_gpu->hw_requested = true;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return true;
    default:
      return false;
  }
}

void VideoCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (!g_gpu || !data || width == 0 || height == 0) return;

  if (data == RETRO_HW_FRAME_BUFFER_VALID) {
    glFinish();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gpu->fbo);
    std::vector<uint8_t> rgba(width * height * 4u, 0u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    g_gpu->capture.width = width;
    g_gpu->capture.height = height;
    g_gpu->capture.rgb.resize(width * height * 3u);
    for (unsigned y = 0; y < height; y++) {
      const unsigned src_y = g_gpu->hw_cb.bottom_left_origin ? (height - 1u - y) : y;
      const uint8_t* src_row = rgba.data() + src_y * width * 4u;
      uint8_t* dst_row = g_gpu->capture.rgb.data() + y * width * 3u;
      for (unsigned x = 0; x < width; x++) {
        dst_row[x * 3u + 0u] = src_row[x * 4u + 0u];
        dst_row[x * 3u + 1u] = src_row[x * 4u + 1u];
        dst_row[x * 3u + 2u] = src_row[x * 4u + 2u];
      }
    }
    return;
  }

  // Software pixel formats: decode to top-to-bottom RGB888 and store raw bytes
  const size_t bpp = (g_gpu->pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) ? 4u : 2u;
  g_gpu->capture.width = width;
  g_gpu->capture.height = height;
  g_gpu->capture.rgb.resize(width * height * 3u);
  g_gpu->capture.raw_frame.resize(width * height * bpp);
  const auto* src = static_cast<const uint8_t*>(data);

  for (unsigned y = 0; y < height; y++) {
    const uint8_t* srow = src + y * pitch;
    std::memcpy(g_gpu->capture.raw_frame.data() + y * width * bpp, srow, width * bpp);
    uint8_t* drow = g_gpu->capture.rgb.data() + y * width * 3u;
    if (g_gpu->pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
      for (unsigned x = 0; x < width; x++) {
        const uint32_t px = *reinterpret_cast<const uint32_t*>(srow + x * 4u);
        drow[x * 3u + 0u] = static_cast<uint8_t>((px >> 16) & 0xFFu);
        drow[x * 3u + 1u] = static_cast<uint8_t>((px >> 8) & 0xFFu);
        drow[x * 3u + 2u] = static_cast<uint8_t>(px & 0xFFu);
      }
    } else if (g_gpu->pixel_format == RETRO_PIXEL_FORMAT_0RGB1555) {
      for (unsigned x = 0; x < width; x++) {
        const uint16_t px = *reinterpret_cast<const uint16_t*>(srow + x * 2u);
        drow[x * 3u + 0u] = Expand5To8((px >> 10) & 31u);
        drow[x * 3u + 1u] = Expand5To8((px >> 5) & 31u);
        drow[x * 3u + 2u] = Expand5To8(px & 31u);
      }
    } else {
      // RETRO_PIXEL_FORMAT_RGB565 (5-6-5). Note GPU_SW encodes PS1 5-bit G5 into bits 10..6.
      for (unsigned x = 0; x < width; x++) {
        const uint16_t px = *reinterpret_cast<const uint16_t*>(srow + x * 2u);
        drow[x * 3u + 0u] = Expand5To8((px >> 11) & 31u);
        drow[x * 3u + 1u] = Expand5To8((px >> 6) & 31u);
        drow[x * 3u + 2u] = Expand5To8(px & 31u);
      }
    }
  }
  g_gpu->capture.raw_hash = Fnv1a64(g_gpu->capture.raw_frame.data(), g_gpu->capture.raw_frame.size());
}

// GPU command emission helpers using S0 = 0xBF801810 (GP0), S0+4 = 0xBF801814 (GP1).
void EmitGpuWaitReady(BiosBuilder& b) {
  b.Lw(T9, 4, S0);
  b.Lui(T8, 0x1000u);  // GPUSTAT bit 28: ready to receive DMA block / GP0 words
  b.And(T9, T9, T8);
  b.Beq(T9, ZERO, -4);
  b.Nop();
}

void Gp0(BiosBuilder& b, uint32_t word) {
  b.Li32(T0, word);
  b.Sw(T0, 0, S0);
}

void Gp1(BiosBuilder& b, uint32_t word) {
  b.Li32(T0, word);
  b.Sw(T0, 4, S0);
}

constexpr uint32_t PackXY(int16_t x, int16_t y) {
  return (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16) |
         static_cast<uint32_t>(static_cast<uint16_t>(x));
}

constexpr uint32_t PackUVPal(uint8_t u, uint8_t v, uint16_t clut) {
  return (static_cast<uint32_t>(clut) << 16) |
         (static_cast<uint32_t>(v) << 8) | static_cast<uint32_t>(u);
}

constexpr uint32_t PackClut(uint16_t cx_div16, uint16_t cy) {
  return ((static_cast<uint32_t>(cy) & 0x1FFu) << 6) | (static_cast<uint32_t>(cx_div16) & 0x3Fu);
}

constexpr uint32_t PackTexpage(uint16_t tx_div64, uint16_t ty_div256, uint16_t depth_0_1_2, uint16_t abr = 0u) {
  return (static_cast<uint32_t>(depth_0_1_2 & 3u) << 7) |
         (static_cast<uint32_t>(abr & 3u) << 5) |
         (static_cast<uint32_t>(ty_div256 & 1u) << 4) |
         (static_cast<uint32_t>(tx_div64) & 0xFu);
}

void EmitScenePrologue(BiosBuilder& b, uint32_t bg_rgb24 = 0x181410u) {
  b.Seek(0x0u);
  b.J(0xBFC00400u);
  b.Nop();
  b.Seek(0x400u);
  b.Li32(S0, 0xBF801810u);

  Gp1(b, 0x00000000u);  // Reset GPU
  Gp1(b, 0x03000000u);  // Display enable = ON
  Gp1(b, 0x04000000u);  // DMA direction = Off
  Gp1(b, 0x05000000u);  // Display VRAM start = (0, 0)
  Gp1(b, 0x06C60260u);  // Horizontal display range: 320px width
  Gp1(b, 0x07040010u);  // Vertical display range: 240 lines (16..256)
  Gp1(b, 0x08000001u);  // Display mode: 320x240, 15-bit, NTSC

  Gp0(b, 0xE1000400u);  // Draw to display area enabled, dither off
  Gp0(b, 0xE2000000u);  // Texture window off
  Gp0(b, 0xE3000000u);  // Clip TL (0, 0)
  Gp0(b, 0xE4000000u | (239u << 10) | 319u);  // Clip BR (319, 239)
  Gp0(b, 0xE5000000u);  // Drawing offset (0, 0)
  Gp0(b, 0xE6000000u);  // Mask bit off

  // Clear entire 320x240 active region cleanly using FillRect (0x02)
  Gp0(b, 0x02000000u | (bg_rgb24 & 0xFFFFFFu));
  Gp0(b, PackXY(0, 0));
  Gp0(b, PackXY(320, 240));
  EmitGpuWaitReady(b);
}

void UploadTexturesAndPalettes(BiosBuilder& b) {
  // Palette 0 at (0, 256): 16 colors (4bpp CLUT), entries with STP=0 & STP=1 at end
  std::vector<uint16_t> pal4(16u);
  for (uint16_t i = 0; i < 16u; i++) {
    uint16_t r = (i * 2u) & 31u;
    uint16_t g = ((15u - i) * 2u) & 31u;
    uint16_t bl = ((i & 3u) * 8u) & 31u;
    uint16_t stp = (i >= 12u) ? 0x8000u : 0x0000u;
    pal4[i] = stp | (bl << 10) | (g << 5) | r;
  }
  pal4[0] = 0x0000u;  // transparent index 0
  EmitGpuWaitReady(b);
  Gp0(b, 0xA0000000u);
  Gp0(b, PackXY(0, 256));
  Gp0(b, PackXY(16, 1));
  for (size_t i = 0; i < pal4.size(); i += 2) {
    Gp0(b, (static_cast<uint32_t>(pal4[i + 1]) << 16) | pal4[i]);
  }

  // Palette 1 at (0, 258): 256 colors (8bpp CLUT)
  std::vector<uint16_t> pal8(256u);
  for (uint16_t i = 0; i < 256u; i++) {
    uint16_t r = (i & 31u);
    uint16_t g = ((i * 3u) >> 3) & 31u;
    uint16_t bl = ((255u - i) >> 3) & 31u;
    pal8[i] = (bl << 10) | (g << 5) | r;
  }
  pal8[0] = 0x0000u;
  EmitGpuWaitReady(b);
  Gp0(b, 0xA0000000u);
  Gp0(b, PackXY(0, 258));
  Gp0(b, PackXY(256, 1));
  for (size_t i = 0; i < pal8.size(); i += 2) {
    Gp0(b, (static_cast<uint32_t>(pal8[i + 1]) << 16) | pal8[i]);
  }

  // Upload 4bpp pattern into Texture Page 8 -> VRAM (512, 0), size 16x64 halfwords (= 64x64 4bpp texels)
  EmitGpuWaitReady(b);
  Gp0(b, 0xA0000000u);
  Gp0(b, PackXY(512, 0));
  Gp0(b, PackXY(16, 64));
  for (uint32_t y = 0; y < 64u; y++) {
    for (uint32_t hw = 0; hw < 16u; hw += 2u) {
      auto pack4 = [&](uint32_t h) -> uint16_t {
        uint16_t w = 0;
        for (uint32_t nib = 0; nib < 4u; nib++) {
          uint32_t x = h * 4u + nib;
          uint32_t idx = ((x / 4u + y / 4u) & 15u);
          if (idx == 0u && ((x ^ y) & 8u)) idx = 1u;
          w |= static_cast<uint16_t>(idx << (nib * 4u));
        }
        return w;
      };
      Gp0(b, (static_cast<uint32_t>(pack4(hw + 1u)) << 16) | pack4(hw));
    }
  }

  // Upload 8bpp pattern into Texture Page 9 -> VRAM (576, 0), size 32x64 halfwords (= 64x64 8bpp texels)
  EmitGpuWaitReady(b);
  Gp0(b, 0xA0000000u);
  Gp0(b, PackXY(576, 0));
  Gp0(b, PackXY(32, 64));
  for (uint32_t y = 0; y < 64u; y++) {
    for (uint32_t hw = 0; hw < 32u; hw += 2u) {
      auto pack8 = [&](uint32_t h) -> uint16_t {
        uint8_t lo = static_cast<uint8_t>((h * 4u + y * 3u) | 1u);
        uint8_t hi = static_cast<uint8_t>(((h * 4u + 2u) + y * 5u) | 1u);
        return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
      };
      Gp0(b, (static_cast<uint32_t>(pack8(hw + 1u)) << 16) | pack8(hw));
    }
  }

  // Upload 15bpp direct pattern into Texture Page 10 -> VRAM (640, 0), size 64x64
  EmitGpuWaitReady(b);
  Gp0(b, 0xA0000000u);
  Gp0(b, PackXY(640, 0));
  Gp0(b, PackXY(64, 64));
  for (uint32_t y = 0; y < 64u; y++) {
    for (uint32_t x = 0; x < 64u; x += 2u) {
      auto rgb15 = [&](uint32_t px) -> uint16_t {
        uint16_t r = (px >> 1) & 31u;
        uint16_t g = (y >> 1) & 31u;
        uint16_t bl = ((px ^ y) >> 1) & 31u;
        uint16_t stp = (((px / 8u) + (y / 8u)) & 1u) ? 0x8000u : 0x0000u;
        return stp | (bl << 10) | (g << 5) | r;
      };
      Gp0(b, (static_cast<uint32_t>(rgb15(x + 1u)) << 16) | rgb15(x));
    }
  }
  EmitGpuWaitReady(b);
}

struct GpuScene {
  const char* name;
  std::function<void(BiosBuilder&)> build;
};

std::vector<GpuScene> BuildScenes() {
  std::vector<GpuScene> scenes;

  // 1. Flat & Gouraud shaded triangles & quads (Dither OFF)
  scenes.push_back({"shaded_polys_dither_off", [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x201410u);
    Gp0(b, 0xE1000400u);  // dither=0
    // Flat tri (0x20)
    Gp0(b, 0x2030D0FFu);
    Gp0(b, PackXY(20, 20)); Gp0(b, PackXY(130, 35)); Gp0(b, PackXY(55, 110));
    // Flat quad (0x28)
    Gp0(b, 0x28FF7020u);
    Gp0(b, PackXY(150, 25)); Gp0(b, PackXY(280, 30)); Gp0(b, PackXY(165, 100)); Gp0(b, PackXY(295, 105));
    // Gouraud tri (0x30)
    Gp0(b, 0x300000F8u); Gp0(b, PackXY(20, 125));
    Gp0(b, 0x0000F800u); Gp0(b, PackXY(145, 130));
    Gp0(b, 0x00F80000u); Gp0(b, PackXY(80, 225));
    // Gouraud quad (0x38)
    Gp0(b, 0x38F8F810u); Gp0(b, PackXY(160, 120));
    Gp0(b, 0x0010F8F8u); Gp0(b, PackXY(300, 125));
    Gp0(b, 0x00F810F8u); Gp0(b, PackXY(170, 225));
    Gp0(b, 0x00F8F8F8u); Gp0(b, PackXY(305, 220));
    b.HaltLoop();
  }});

  // 2. Gouraud shaded quads with 4x4 ordered dithering ON
  scenes.push_back({"shaded_polys_dither_on", [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x101018u);
    Gp0(b, 0xE1000600u);  // dfe=1, dtd=1 (bit 9)
    Gp0(b, 0x38101010u); Gp0(b, PackXY(16, 16));
    Gp0(b, 0x00E0F0F8u); Gp0(b, PackXY(304, 16));
    Gp0(b, 0x0020A040u); Gp0(b, PackXY(16, 224));
    Gp0(b, 0x00F840C0u); Gp0(b, PackXY(304, 224));
    b.HaltLoop();
  }});

  // 3. 4-bit CLUT, 8-bit CLUT, 15-bit direct textured quads + Texture Window (0xE2)
  scenes.push_back({"textured_clut_4_8_15", [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x141824u);
    UploadTexturesAndPalettes(b);
    const uint32_t pal4 = PackClut(0u, 256u);
    const uint32_t pal8 = PackClut(0u, 258u);
    const uint32_t tp4 = PackTexpage(8u, 0u, 0u);
    const uint32_t tp8 = PackTexpage(9u, 0u, 1u);
    const uint32_t tp15 = PackTexpage(10u, 0u, 2u);

    // Raw 4bpp textured quad (0x2D)
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(16, 20));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, PackXY(104, 20)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp4)));
    Gp0(b, PackXY(16, 108)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(104, 108)); Gp0(b, PackUVPal(63, 63, 0));

    // Modulated 8bpp textured quad (0x2C)
    Gp0(b, 0x2CA0D0FFu);
    Gp0(b, PackXY(116, 20));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal8)));
    Gp0(b, PackXY(204, 20));  Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp8)));
    Gp0(b, PackXY(116, 108)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(204, 108)); Gp0(b, PackUVPal(63, 63, 0));

    // Raw 15bpp direct textured quad (0x2D)
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(216, 20));  Gp0(b, PackUVPal(0, 0, 0));
    Gp0(b, PackXY(304, 20));  Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp15)));
    Gp0(b, PackXY(216, 108)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(304, 108)); Gp0(b, PackUVPal(63, 63, 0));

    // Texture Window 0xE2: repeat 16x16 region of 4bpp texture (maskX=29->3, maskY=29->3, off=0)
    Gp0(b, 0xE2000000u | (0x1Du << 0) | (0x1Du << 5));
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(40, 124));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, PackXY(280, 124)); Gp0(b, PackUVPal(127, 0, static_cast<uint16_t>(tp4)));
    Gp0(b, PackXY(40, 220));  Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(280, 220)); Gp0(b, PackUVPal(127, 63, 0));
    b.HaltLoop();
  }});

  // 4. All 4 semi-transparency blend modes + STP textured transparency
  scenes.push_back({"semi_transparency_modes", [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x403020u);
    UploadTexturesAndPalettes(b);
    // Bright striped background quads across screen
    Gp0(b, 0x284080C0u);
    Gp0(b, PackXY(16, 16)); Gp0(b, PackXY(160, 16)); Gp0(b, PackXY(16, 224)); Gp0(b, PackXY(160, 224));
    Gp0(b, 0x28C06040u);
    Gp0(b, PackXY(160, 16)); Gp0(b, PackXY(304, 16)); Gp0(b, PackXY(160, 224)); Gp0(b, PackXY(304, 224));

    for (uint32_t abr = 0; abr < 4u; abr++) {
      Gp0(b, 0xE1000400u | (abr << 5));
      int16_t x0 = static_cast<int16_t>(24 + abr * 70);
      Gp0(b, 0x2A5090D0u);  // semi-transparent flat quad (0x2A)
      Gp0(b, PackXY(x0, 32)); Gp0(b, PackXY(x0 + 58, 32));
      Gp0(b, PackXY(x0, 120)); Gp0(b, PackXY(x0 + 58, 120));
    }

    // Textured semi-transparent quad using 15bpp texture where half tiles have STP=1 (0x2F)
    const uint32_t tp15_abr0 = PackTexpage(10u, 0u, 2u, 0u);
    Gp0(b, 0x2F000000u);
    Gp0(b, PackXY(40, 136));  Gp0(b, PackUVPal(0, 0, 0));
    Gp0(b, PackXY(280, 136)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp15_abr0)));
    Gp0(b, PackXY(40, 212));  Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(280, 212)); Gp0(b, PackUVPal(63, 63, 0));
    b.HaltLoop();
  }});

  // 5. Flat/Gouraud lines, polyline, flat rects, and textured sprites
  scenes.push_back({"lines_sprites_rects", [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x181822u);
    UploadTexturesAndPalettes(b);
    const uint32_t pal4 = PackClut(0u, 256u);
    Gp0(b, 0xE1000400u | PackTexpage(8u, 0u, 0u));

    // Monochrome line (0x40)
    Gp0(b, 0x4040F0FFu); Gp0(b, PackXY(16, 20)); Gp0(b, PackXY(300, 48));
    // Gouraud polyline (0x58 ... 0x55555555)
    Gp0(b, 0x580000F8u); Gp0(b, PackXY(20, 60));
    Gp0(b, 0x0000F800u); Gp0(b, PackXY(120, 32));
    Gp0(b, 0x00F80000u); Gp0(b, PackXY(220, 88));
    Gp0(b, 0x00F8F8F8u); Gp0(b, PackXY(300, 50));
    Gp0(b, 0x55555555u);

    // Variable opaque rect (0x60) & fixed 16x16 flat rect (0x78)
    Gp0(b, 0x6030C060u); Gp0(b, PackXY(24, 108)); Gp0(b, PackXY(72, 48));
    Gp0(b, 0x78C040F0u); Gp0(b, PackXY(112, 112));

    // Variable raw sprite (0x65), 8x8 raw sprite (0x75), 16x16 raw sprite (0x7D)
    Gp0(b, 0x65000000u); Gp0(b, PackXY(150, 108)); Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4))); Gp0(b, PackXY(64, 64));
    Gp0(b, 0x75000000u); Gp0(b, PackXY(230, 112)); Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, 0x7D000000u); Gp0(b, PackXY(256, 112)); Gp0(b, PackUVPal(8, 8, static_cast<uint16_t>(pal4)));
    b.HaltLoop();
  }});

  // 6. FillRect (0x02), CPU->VRAM (0xA0), VRAM->VRAM (0x80) overlap, and Mask Bit (0xE6)
  scenes.push_back({"vram_blits_mask_bit", [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x202020u);
    // Fill two source tiles at (32, 32) and (96, 32)
    Gp0(b, 0x022060E0u); Gp0(b, PackXY(32, 32)); Gp0(b, PackXY(64, 48));
    Gp0(b, 0x02E0C030u); Gp0(b, PackXY(96, 32)); Gp0(b, PackXY(48, 48));

    // VRAM->VRAM non-overlapping copy (32, 32) -> (32, 110)
    EmitGpuWaitReady(b);
    Gp0(b, 0x80000000u); Gp0(b, PackXY(32, 32)); Gp0(b, PackXY(32, 110)); Gp0(b, PackXY(64, 48));
    // VRAM->VRAM overlapping copy shifted (+16, +8)
    EmitGpuWaitReady(b);
    Gp0(b, 0x80000000u); Gp0(b, PackXY(32, 32)); Gp0(b, PackXY(48, 40)); Gp0(b, PackXY(48, 36));

    // Mask bit: draw quad WITH force mask bit (0xE6 = 1), then draw OVER it with check mask (0xE6 = 2)
    Gp0(b, 0xE6000001u);  // set bit 15 on writes
    Gp0(b, 0x6040E040u); Gp0(b, PackXY(176, 40)); Gp0(b, PackXY(96, 96));
    Gp0(b, 0xE6000002u);  // preserve pixels that have bit 15 = 1
    Gp0(b, 0x604040F0u); Gp0(b, PackXY(156, 80)); Gp0(b, PackXY(136, 96));
    Gp0(b, 0xE6000000u);
    b.HaltLoop();
  }});

  // 7. Drawing area clip (0xE3/0xE4) and signed drawing offset (0xE5)
  scenes.push_back({"clip_and_offset", [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x241818u);
    // Restrict drawing window to [48..271, 32..207]
    Gp0(b, 0xE3000000u | (32u << 10) | 48u);
    Gp0(b, 0xE4000000u | (207u << 10) | 271u);
    // Apply signed drawing offset (-24, +16) -> 11-bit two's complement
    const uint32_t offX = static_cast<uint32_t>(-24) & 0x7FFu;
    const uint32_t offY = static_cast<uint32_t>(16) & 0x7FFu;
    Gp0(b, 0xE5000000u | (offY << 11) | offX);

    // Clipped Gouraud quad extending beyond [48..271, 32..207]
    Gp0(b, 0x3800F0F0u); Gp0(b, PackXY(10, 0));
    Gp0(b, 0x00F000F0u); Gp0(b, PackXY(340, 0));
    Gp0(b, 0x00F0F000u); Gp0(b, PackXY(10, 230));
    Gp0(b, 0x00FFFFFFu); Gp0(b, PackXY(340, 230));

    // Clipped rect partially outside right edge
    Gp0(b, 0x60E05030u); Gp0(b, PackXY(240, 60)); Gp0(b, PackXY(80, 100));
    b.HaltLoop();
  }});

  return scenes;
}

bool CaptureScene(EcrApi& api, const std::string& sys_dir, const GpuScene& scene,
                  const std::string& renderer, bool use_thread, RgbImage* out) {
  GpuEnv env;
  env.sys_dir = sys_dir;
  env.renderer = renderer;
  env.use_thread = use_thread;
  g_gpu = &env;

  const bool is_gl = (renderer == "OpenGL");
  if (is_gl && !InitHeadlessEgl(env)) {
    g_gpu = nullptr;
    return false;
  }

  BiosBuilder builder;
  scene.build(builder);
  if (!builder.WriteFile(sys_dir + "/gpu_oracle_bios.bin")) {
    ShutdownHeadlessEgl(env);
    g_gpu = nullptr;
    return false;
  }

  api.retro_set_environment(EnvironmentCallback);
  api.retro_set_video_refresh(VideoCallback);
  api.retro_set_audio_sample([](int16_t, int16_t) {});
  api.retro_set_audio_sample_batch([](const int16_t*, size_t n) { return n; });
  api.retro_set_input_poll([]() {});
  api.retro_set_input_state([](unsigned, unsigned, unsigned, unsigned) { return int16_t(0); });

  api.retro_init();
  if (!api.retro_load_game(nullptr)) {
    std::fprintf(stderr, "FAIL [%s] retro_load_game failed for %s\n", scene.name, renderer.c_str());
    api.retro_deinit();
    ShutdownHeadlessEgl(env);
    g_gpu = nullptr;
    return false;
  }

  if (is_gl) {
    if (!env.hw_requested || !env.hw_cb.context_reset) {
      std::fprintf(stderr, "FAIL [%s] Core did not negotiate GLES HW render\n", scene.name);
      api.retro_unload_game();
      api.retro_deinit();
      ShutdownHeadlessEgl(env);
      g_gpu = nullptr;
      return false;
    }
    eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
    env.hw_cb.context_reset();
  }

  for (int f = 0; f < 10; f++) {
    if (is_gl) eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
    api.retro_run();
  }

  if (is_gl && env.hw_cb.context_destroy) {
    eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
    env.hw_cb.context_destroy();
  }

  api.retro_unload_game();
  api.retro_deinit();
  ShutdownHeadlessEgl(env);
  *out = std::move(env.capture);
  g_gpu = nullptr;
  return out->width > 0 && out->height > 0 && !out->rgb.empty();
}

struct ImageDiffMetrics {
  bool raw_exact = false;
  double exact5_pct = 0.0;
  double within1lsb5_pct = 0.0;
  double mae_8bit = 0.0;
  int max_diff_8bit = 0;
  double psnr_db = 0.0;
};

ImageDiffMetrics CompareImages(const RgbImage& ref, const RgbImage& cand) {
  ImageDiffMetrics m{};
  m.raw_exact = (!ref.raw_frame.empty() && ref.raw_frame == cand.raw_frame && ref.raw_hash == cand.raw_hash);
  if (ref.width != cand.width || ref.height != cand.height || ref.rgb.size() != cand.rgb.size()) {
    return m;
  }
  const size_t pixels = static_cast<size_t>(ref.width) * ref.height;
  size_t exact5 = 0;
  size_t within1lsb5 = 0;
  double sum_abs = 0.0;
  double sum_sq = 0.0;

  for (size_t i = 0; i < pixels; i++) {
    bool px_exact5 = true;
    bool px_within1 = true;
    for (int c = 0; c < 3; c++) {
      const int a = ref.rgb[i * 3u + c];
      const int b = cand.rgb[i * 3u + c];
      const int diff8 = std::abs(a - b);
      sum_abs += diff8;
      sum_sq += static_cast<double>(diff8) * diff8;
      if (diff8 > m.max_diff_8bit) m.max_diff_8bit = diff8;

      const int a5 = a >> 3;
      const int b5 = b >> 3;
      const int d5 = std::abs(a5 - b5);
      if (d5 != 0) px_exact5 = false;
      if (d5 > 1) px_within1 = false;
    }
    if (px_exact5) exact5++;
    if (px_within1) within1lsb5++;
  }

  const double channels = static_cast<double>(pixels * 3u);
  m.exact5_pct = 100.0 * static_cast<double>(exact5) / static_cast<double>(pixels);
  m.within1lsb5_pct = 100.0 * static_cast<double>(within1lsb5) / static_cast<double>(pixels);
  m.mae_8bit = sum_abs / channels;
  const double mse = sum_sq / channels;
  m.psnr_db = (mse == 0.0) ? 99.99 : (10.0 * std::log10((255.0 * 255.0) / mse));
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <libecr_android.so>\n", argv[0]);
    return 2;
  }

  EcrApi api;
  if (!api.Load(argv[1])) return 2;

  char tmp_template[] = "/data/local/tmp/ecr-gpu-oracle-XXXXXX";
  char* tmp_dir = mkdtemp(tmp_template);
  if (!tmp_dir) { std::perror("mkdtemp"); return 2; }
  const std::string sys_dir(tmp_dir);

  const auto scenes = BuildScenes();
  int total_checks = 0;
  int total_failures = 0;

  std::printf("=== EmuCoreR ECR GPU Accuracy Oracle (Golden: Software Single-Thread) ===\n");
  for (const auto& s : scenes) {
    RgbImage golden;
    if (!CaptureScene(api, sys_dir, s, "Software", false, &golden)) {
      std::fprintf(stderr, "FAIL %-26s Golden Software failed\n", s.name);
      total_failures++;
      continue;
    }

    // 1. SoftwareThreaded (must be 100% bit-exact)
    {
      total_checks++;
      RgbImage thr;
      if (!CaptureScene(api, sys_dir, s, "Software", true, &thr)) {
        std::printf("FAIL %-26s backend=SoftwareThreaded (run error)\n", s.name);
        total_failures++;
      } else {
        const auto m = CompareImages(golden, thr);
        const bool ok = (golden.width == thr.width && golden.height == thr.height && m.raw_exact && m.max_diff_8bit == 0);
        std::printf("%s %-26s backend=SoftwareThreaded (%ux%u) exact=%.2f%% maxDiff=%d hash=0x%016llX\n",
                    ok ? "PASS" : "FAIL", s.name, thr.width, thr.height,
                    m.exact5_pct, m.max_diff_8bit, static_cast<unsigned long long>(thr.raw_hash));
        if (!ok) total_failures++;
      }
    }

    // 2. HardwareOpenGL (GLES 3.0 PBuffer, threshold check + rich metrics)
    {
      total_checks++;
      RgbImage gl;
      if (!CaptureScene(api, sys_dir, s, "OpenGL", false, &gl)) {
        std::printf("FAIL %-26s backend=HardwareOpenGL (EGL/GLES error)\n", s.name);
        total_failures++;
      } else {
        const auto m = CompareImages(golden, gl);
        const bool dims_ok = (golden.width == gl.width && golden.height == gl.height);
        const bool acc_ok = dims_ok && (m.psnr_db >= 25.0) && (m.within1lsb5_pct >= 96.0);
        std::printf("%s %-26s backend=HardwareOpenGL   (%ux%u) exact5=%.2f%% +/-1LSB5=%.2f%% MAE=%.3f maxDiff=%d PSNR=%.2f dB\n",
                    acc_ok ? "PASS" : "FAIL", s.name, gl.width, gl.height,
                    m.exact5_pct, m.within1lsb5_pct, m.mae_8bit, m.max_diff_8bit, m.psnr_db);
        if (!acc_ok) total_failures++;
      }
    }
  }

  std::remove((sys_dir + "/gpu_oracle_bios.bin").c_str());
  rmdir(tmp_dir);
  std::printf("GPU Oracle Summary: %d comparisons, %d failures\n", total_checks, total_failures);
  return total_failures == 0 ? 0 : 1;
}

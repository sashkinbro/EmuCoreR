// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EmuCoreR ECR Expanded GPU & PGXP Accuracy Oracle:
//   Golden reference : Software (Single-Threaded, ecr_GPU_UseThread = false)
//   Targets checked  :
//     1) SoftwareThreaded (Software, ecr_GPU_UseThread = true)      -> 100% bit-exact required
//     2) HardwareOpenGL   (OpenGL ES 3.0 headless EGL PBuffer)      -> Exact %, +-1-LSB %, MAE, PSNR (dB)
//     3) HardwareVulkan   (Vulkan 1.0 headless device + staging)    -> Exact %, +-1-LSB %, MAE, PSNR (dB)
//   Includes 14 scenes: shading, dithering, CLUT 4/8/15, semi-transparency, lines/sprites, VRAM blits,
//   clip/offset, texture-window wrap, Gouraud-textured modulation, winding/negative-vertex clipping,
//   512x240 hi-res / 24-bit display, VRAM feedback self-copy, 2x upscaling, and GTE 3D PGXP checks.
//
//   Options: --no-vulkan  --dump-dir <dir>
#include "mips_emitter.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>
#include <libretro_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
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

struct GpuRunOptions {
  std::string renderer = "Software";
  bool use_thread = false;
  int resolution_scale = 1;
  bool true_color = false;
  bool scaled_dithering = false;
  std::string cpu_mode = "Interpreter";
  bool pgxp_enable = false;
  bool pgxp_cpu = false;
  bool pgxp_culling = true;
  bool pgxp_texture_correction = true;
  bool pgxp_preserve_proj_fp = true;
  bool pgxp_depth_buffer = false;
};

struct GpuEnv {
  std::string sys_dir;
  GpuRunOptions opts;
  std::string res_scale_str = "1";
  bool capture_enabled = false;
  retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_RGB565;
  retro_hw_render_callback hw_cb{};
  bool hw_requested = false;

  // OpenGL ES 3.0 headless FBO
  GLuint fbo = 0;
  GLuint color_tex = 0;
  GLuint depth_rb = 0;
  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  EGLContext egl_context = EGL_NO_CONTEXT;

  // Vulkan 1.0 headless context + readback staging
  bool vk_negotiated = false;
  retro_hw_render_context_negotiation_interface_vulkan vk_neg{};
  VkInstance vk_instance = VK_NULL_HANDLE;
  retro_vulkan_context vk_ctx{};
  retro_hw_render_interface_vulkan vk_iface{};
  retro_vulkan_image vk_current_image{};
  bool vk_has_image = false;
  std::mutex vk_queue_mutex;
  VkCommandPool vk_cmd_pool = VK_NULL_HANDLE;
  VkCommandBuffer vk_cmd_buf = VK_NULL_HANDLE;
  VkBuffer vk_staging_buf = VK_NULL_HANDLE;
  VkDeviceMemory vk_staging_mem = VK_NULL_HANDLE;
  VkDeviceSize vk_staging_size = 0;

  RgbImage capture;
};

GpuEnv* g_gpu = nullptr;

inline uint8_t Expand5To8(uint32_t v5) {
  return static_cast<uint8_t>((v5 << 3) | (v5 >> 2));
}

// ============================================================================
// Headless EGL / GLES 3.0
// ============================================================================
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
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24,
    EGL_NONE
  };
  EGLConfig cfg = nullptr;
  EGLint num_cfg = 0;
  if (eglChooseConfig(env.egl_display, cfg_attribs, &cfg, 1, &num_cfg) != EGL_TRUE || num_cfg < 1) {
    std::fprintf(stderr, "EGL: eglChooseConfig failed (0x%x)\n", eglGetError());
    return false;
  }
  const EGLint pb_attribs[] = {EGL_WIDTH, 1280, EGL_HEIGHT, 960, EGL_NONE};
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1280, 960, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenRenderbuffers(1, &env.depth_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, env.depth_rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1280, 960);
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

// ============================================================================
// Headless Vulkan
// ============================================================================
void VkSetImage(void* handle, const struct retro_vulkan_image* image, uint32_t, const VkSemaphore*, uint32_t) {
  auto* env = static_cast<GpuEnv*>(handle);
  if (image) {
    env->vk_current_image = *image;
    env->vk_has_image = true;
  } else {
    env->vk_has_image = false;
  }
}

uint32_t VkGetSyncIndex(void*) { return 0; }
uint32_t VkGetSyncIndexMask(void*) { return 1; }
void VkSetCommandBuffers(void*, uint32_t, const VkCommandBuffer*) {}

void VkWaitSyncIndex(void* handle) {
  auto* env = static_cast<GpuEnv*>(handle);
  if (env->vk_ctx.queue != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(env->vk_queue_mutex);
    vkQueueWaitIdle(env->vk_ctx.queue);
  }
}

void VkLockQueue(void* handle) { static_cast<GpuEnv*>(handle)->vk_queue_mutex.lock(); }
void VkUnlockQueue(void* handle) { static_cast<GpuEnv*>(handle)->vk_queue_mutex.unlock(); }
void VkSetSignalSemaphore(void*, VkSemaphore) {}

bool InitHeadlessVulkanForEnv(GpuEnv& env) {
  const VkApplicationInfo* app_info = nullptr;
  if (env.vk_negotiated && env.vk_neg.get_application_info) {
    app_info = env.vk_neg.get_application_info();
  }
  VkApplicationInfo fallback_app{};
  fallback_app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  if (!app_info) {
    fallback_app.pApplicationName = "GpuOracle";
    fallback_app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    fallback_app.pEngineName = "EmuCoreR";
    fallback_app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    fallback_app.apiVersion = VK_API_VERSION_1_0;
    app_info = &fallback_app;
  }

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = app_info;
  VkResult res = vkCreateInstance(&ici, nullptr, &env.vk_instance);
  if (res != VK_SUCCESS) {
    std::fprintf(stderr, "Vulkan: vkCreateInstance failed (%d)\n", static_cast<int>(res));
    return false;
  }

  if (!env.vk_negotiated || !env.vk_neg.create_device) {
    std::fprintf(stderr, "Vulkan: core did not provide negotiation create_device\n");
    return false;
  }
  if (!env.vk_neg.create_device(&env.vk_ctx, env.vk_instance, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                vkGetInstanceProcAddr, nullptr, 0, nullptr, 0, nullptr)) {
    std::fprintf(stderr, "Vulkan: core create_device failed\n");
    return false;
  }

  env.vk_iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
  env.vk_iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
  env.vk_iface.handle = &env;
  env.vk_iface.instance = env.vk_instance;
  env.vk_iface.gpu = env.vk_ctx.gpu;
  env.vk_iface.device = env.vk_ctx.device;
  env.vk_iface.get_device_proc_addr = vkGetDeviceProcAddr;
  env.vk_iface.get_instance_proc_addr = vkGetInstanceProcAddr;
  env.vk_iface.queue = env.vk_ctx.queue;
  env.vk_iface.queue_index = env.vk_ctx.queue_family_index;
  env.vk_iface.set_image = VkSetImage;
  env.vk_iface.get_sync_index = VkGetSyncIndex;
  env.vk_iface.get_sync_index_mask = VkGetSyncIndexMask;
  env.vk_iface.set_command_buffers = VkSetCommandBuffers;
  env.vk_iface.wait_sync_index = VkWaitSyncIndex;
  env.vk_iface.lock_queue = VkLockQueue;
  env.vk_iface.unlock_queue = VkUnlockQueue;
  env.vk_iface.set_signal_semaphore = VkSetSignalSemaphore;

  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = env.vk_ctx.queue_family_index;
  if (vkCreateCommandPool(env.vk_ctx.device, &cpci, nullptr, &env.vk_cmd_pool) != VK_SUCCESS) {
    std::fprintf(stderr, "Vulkan: vkCreateCommandPool failed\n");
    return false;
  }

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = env.vk_cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(env.vk_ctx.device, &cbai, &env.vk_cmd_buf) != VK_SUCCESS) {
    std::fprintf(stderr, "Vulkan: vkAllocateCommandBuffers failed\n");
    return false;
  }
  return true;
}

void CleanupVulkanOracleResources(GpuEnv& env) {
  if (env.vk_ctx.device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(env.vk_ctx.device);
    if (env.vk_staging_buf != VK_NULL_HANDLE) {
      vkDestroyBuffer(env.vk_ctx.device, env.vk_staging_buf, nullptr);
      env.vk_staging_buf = VK_NULL_HANDLE;
    }
    if (env.vk_staging_mem != VK_NULL_HANDLE) {
      vkFreeMemory(env.vk_ctx.device, env.vk_staging_mem, nullptr);
      env.vk_staging_mem = VK_NULL_HANDLE;
    }
    if (env.vk_cmd_pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(env.vk_ctx.device, env.vk_cmd_pool, nullptr);
      env.vk_cmd_pool = VK_NULL_HANDLE;
    }
  }
}

void DestroyVulkanInstance(GpuEnv& env) {
  if (env.vk_ctx.device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(env.vk_ctx.device);
    vkDestroyDevice(env.vk_ctx.device, nullptr);
    env.vk_ctx.device = VK_NULL_HANDLE;
  }
  if (env.vk_instance != VK_NULL_HANDLE) {
    vkDestroyInstance(env.vk_instance, nullptr);
    env.vk_instance = VK_NULL_HANDLE;
  }
  env.vk_ctx = {};
  env.vk_has_image = false;
}

bool EnsureVulkanStagingBuffer(GpuEnv& env, VkDeviceSize needed) {
  if (env.vk_staging_buf != VK_NULL_HANDLE && env.vk_staging_size >= needed) return true;
  if (env.vk_staging_buf != VK_NULL_HANDLE) {
    vkDestroyBuffer(env.vk_ctx.device, env.vk_staging_buf, nullptr);
    env.vk_staging_buf = VK_NULL_HANDLE;
  }
  if (env.vk_staging_mem != VK_NULL_HANDLE) {
    vkFreeMemory(env.vk_ctx.device, env.vk_staging_mem, nullptr);
    env.vk_staging_mem = VK_NULL_HANDLE;
  }

  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = needed;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(env.vk_ctx.device, &bci, nullptr, &env.vk_staging_buf) != VK_SUCCESS) return false;

  VkMemoryRequirements mem_req{};
  vkGetBufferMemoryRequirements(env.vk_ctx.device, env.vk_staging_buf, &mem_req);

  VkPhysicalDeviceMemoryProperties mem_props{};
  vkGetPhysicalDeviceMemoryProperties(env.vk_ctx.gpu, &mem_props);

  uint32_t chosen_type = UINT32_MAX;
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
    if ((mem_req.memoryTypeBits & (1u << i)) && ((mem_props.memoryTypes[i].propertyFlags & want) == want)) {
      chosen_type = i;
      break;
    }
  }
  if (chosen_type == UINT32_MAX) {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
      if ((mem_req.memoryTypeBits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        chosen_type = i;
        break;
      }
    }
  }
  if (chosen_type == UINT32_MAX) return false;

  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = mem_req.size;
  mai.memoryTypeIndex = chosen_type;
  if (vkAllocateMemory(env.vk_ctx.device, &mai, nullptr, &env.vk_staging_mem) != VK_SUCCESS) return false;
  if (vkBindBufferMemory(env.vk_ctx.device, env.vk_staging_buf, env.vk_staging_mem, 0) != VK_SUCCESS) return false;
  env.vk_staging_size = needed;
  return true;
}

bool ReadbackVulkanFrame(GpuEnv& env, unsigned width, unsigned height) {
  if (!env.vk_has_image || env.vk_current_image.create_info.image == VK_NULL_HANDLE) return false;
  const VkDeviceSize byte_size = static_cast<VkDeviceSize>(width) * height * 4u;
  if (!EnsureVulkanStagingBuffer(env, byte_size)) return false;

  std::lock_guard<std::mutex> lock(env.vk_queue_mutex);
  vkQueueWaitIdle(env.vk_ctx.queue);

  vkResetCommandBuffer(env.vk_cmd_buf, 0);
  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(env.vk_cmd_buf, &cbbi) != VK_SUCCESS) return false;

  const VkImage img = env.vk_current_image.create_info.image;
  const VkImageLayout old_layout = env.vk_current_image.image_layout;

  VkImageMemoryBarrier to_src{};
  to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  to_src.oldLayout = old_layout;
  to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_src.image = img;
  to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(env.vk_cmd_buf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(env.vk_cmd_buf, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         env.vk_staging_buf, 1, &region);

  VkImageMemoryBarrier restore = to_src;
  restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  restore.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  restore.newLayout = old_layout;
  vkCmdPipelineBarrier(env.vk_cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &restore);

  if (vkEndCommandBuffer(env.vk_cmd_buf) != VK_SUCCESS) return false;

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &env.vk_cmd_buf;
  if (vkQueueSubmit(env.vk_ctx.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
  vkQueueWaitIdle(env.vk_ctx.queue);

  void* mapped = nullptr;
  if (vkMapMemory(env.vk_ctx.device, env.vk_staging_mem, 0, byte_size, 0, &mapped) != VK_SUCCESS) return false;
  VkMappedMemoryRange rng{};
  rng.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  rng.memory = env.vk_staging_mem;
  rng.offset = 0;
  rng.size = VK_WHOLE_SIZE;
  vkInvalidateMappedMemoryRanges(env.vk_ctx.device, 1, &rng);

  const auto* rgba = static_cast<const uint8_t*>(mapped);
  const bool is_bgr = (env.vk_current_image.create_info.format == VK_FORMAT_B8G8R8A8_UNORM);
  env.capture.width = width;
  env.capture.height = height;
  env.capture.rgb.resize(static_cast<size_t>(width) * height * 3u);
  for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
    env.capture.rgb[i * 3u + 0u] = rgba[i * 4u + (is_bgr ? 2u : 0u)];
    env.capture.rgb[i * 3u + 1u] = rgba[i * 4u + 1u];
    env.capture.rgb[i * 3u + 2u] = rgba[i * 4u + (is_bgr ? 0u : 2u)];
  }
  vkUnmapMemory(env.vk_ctx.device, env.vk_staging_mem);
  return true;
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
      if (key == "ecr_GPU_Renderer") val = g_gpu->opts.renderer.c_str();
      else if (key == "ecr_GPU_UseThread") val = g_gpu->opts.use_thread ? "true" : "false";
      else if (key == "ecr_GPU_ResolutionScale") val = g_gpu->res_scale_str.c_str();
      else if (key == "ecr_GPU_TextureFilter") val = "Nearest";
      else if (key == "ecr_GPU_TrueColor") val = g_gpu->opts.true_color ? "true" : "false";
      else if (key == "ecr_GPU_ScaledDithering") val = g_gpu->opts.scaled_dithering ? "true" : "false";
      else if (key == "ecr_GPU_MSAA") val = "1";
      else if (key == "ecr_GPU_PGXPEnable") val = g_gpu->opts.pgxp_enable ? "true" : "false";
      else if (key == "ecr_GPU_PGXPCPU") val = g_gpu->opts.pgxp_cpu ? "true" : "false";
      else if (key == "ecr_GPU_PGXPCulling") val = g_gpu->opts.pgxp_culling ? "true" : "false";
      else if (key == "ecr_GPU_PGXPTextureCorrection") val = g_gpu->opts.pgxp_texture_correction ? "true" : "false";
      else if (key == "ecr_GPU_PGXPPreserveProjFP") val = g_gpu->opts.pgxp_preserve_proj_fp ? "true" : "false";
      else if (key == "ecr_GPU_PGXPDepthBuffer") val = g_gpu->opts.pgxp_depth_buffer ? "true" : "false";
      else if (key == "ecr_GPU_UseSoftwareRendererForReadbacks") val = "false";
      else if (key == "ecr_Display_CropMode") val = "None";
      else if (key == "ecr_Display_AspectRatio") val = "4:3";
      else if (key == "ecr_CPU_ExecutionMode") val = g_gpu->opts.cpu_mode.c_str();
      else if (key == "ecr_CPU_FastmemMode") val = "MMap";
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
      if (g_gpu->opts.renderer == "Vulkan") {
        *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_VULKAN;
        return true;
      }
      if (g_gpu->opts.renderer == "OpenGL") {
        *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGLES_VERSION;
        return true;
      }
      return false;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      if (!data) return false;
      auto* cb = static_cast<retro_hw_render_callback*>(data);
      if (g_gpu->opts.renderer == "OpenGL") {
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
            cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION) return false;
        cb->get_proc_address = [](const char* sym) -> retro_proc_address_t {
          return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym));
        };
        cb->get_current_framebuffer = []() -> uintptr_t { return static_cast<uintptr_t>(g_gpu->fbo); };
        cb->depth = true;
        cb->stencil = false;
        g_gpu->hw_cb = *cb;
        g_gpu->hw_requested = true;
        return true;
      }
      if (g_gpu->opts.renderer == "Vulkan") {
        if (cb->context_type != RETRO_HW_CONTEXT_VULKAN) return false;
        g_gpu->hw_cb = *cb;
        g_gpu->hw_requested = true;
        return true;
      }
      return false;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
      if (!data || g_gpu->opts.renderer != "Vulkan") return false;
      const auto* hdr = static_cast<const retro_hw_render_context_negotiation_interface*>(data);
      if (hdr->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) return false;
      g_gpu->vk_neg = *static_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(data);
      g_gpu->vk_negotiated = true;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: {
      if (!data || g_gpu->opts.renderer != "Vulkan" || g_gpu->vk_ctx.device == VK_NULL_HANDLE) return false;
      *static_cast<const retro_hw_render_interface**>(data) =
        reinterpret_cast<const retro_hw_render_interface*>(&g_gpu->vk_iface);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
      return true;
    default:
      return false;
  }
}

void VideoCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (!g_gpu || !g_gpu->capture_enabled || !data || width == 0 || height == 0) return;

  if (data == RETRO_HW_FRAME_BUFFER_VALID) {
    if (g_gpu->opts.renderer == "Vulkan") {
      ReadbackVulkanFrame(*g_gpu, width, height);
      return;
    }
    glFinish();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gpu->fbo);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u, 0u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    g_gpu->capture.width = width;
    g_gpu->capture.height = height;
    g_gpu->capture.rgb.resize(static_cast<size_t>(width) * height * 3u);
    for (unsigned y = 0; y < height; y++) {
      const unsigned src_y = g_gpu->hw_cb.bottom_left_origin ? (height - 1u - y) : y;
      const uint8_t* src_row = rgba.data() + static_cast<size_t>(src_y) * width * 4u;
      uint8_t* dst_row = g_gpu->capture.rgb.data() + static_cast<size_t>(y) * width * 3u;
      for (unsigned x = 0; x < width; x++) {
        dst_row[x * 3u + 0u] = src_row[x * 4u + 0u];
        dst_row[x * 3u + 1u] = src_row[x * 4u + 1u];
        dst_row[x * 3u + 2u] = src_row[x * 4u + 2u];
      }
    }
    return;
  }

  const size_t bpp = (g_gpu->pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) ? 4u : 2u;
  g_gpu->capture.width = width;
  g_gpu->capture.height = height;
  g_gpu->capture.rgb.resize(static_cast<size_t>(width) * height * 3u);
  g_gpu->capture.raw_frame.resize(static_cast<size_t>(width) * height * bpp);
  const auto* src = static_cast<const uint8_t*>(data);

  for (unsigned y = 0; y < height; y++) {
    const uint8_t* srow = src + y * pitch;
    std::memcpy(g_gpu->capture.raw_frame.data() + static_cast<size_t>(y) * width * bpp, srow, width * bpp);
    uint8_t* drow = g_gpu->capture.rgb.data() + static_cast<size_t>(y) * width * 3u;
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

void EmitGpuWaitReady(BiosBuilder& b) {
  b.Lw(T9, 4, S0);
  b.Lui(T8, 0x1000u);  // GPUSTAT bit 28
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

void EmitScenePrologue(BiosBuilder& b, uint32_t bg_rgb24 = 0x181410u, uint32_t disp_mode = 0x08000001u,
                       uint32_t width = 320u, uint32_t height = 240u) {
  b.Seek(0x0u);
  b.J(0xBFC00400u);
  b.Nop();
  b.Seek(0x400u);

  // Enable COP0 CU2 so GTE tests work if used
  b.Li32(T0, 0x40400000u);
  b.Mtc0(T0, 12u);
  b.Nop();

  b.Li32(S0, 0xBF801810u);
  Gp1(b, 0x00000000u);  // Reset GPU
  Gp1(b, 0x03000000u);  // Display enable = ON
  Gp1(b, 0x04000000u);  // DMA direction = Off
  Gp1(b, 0x05000000u);  // Display VRAM start = (0, 0)
  Gp1(b, 0x06C60260u);  // Horizontal display range
  Gp1(b, 0x07040010u);  // Vertical display range: 240 lines
  Gp1(b, disp_mode);    // Display mode

  Gp0(b, 0xE1000400u);  // dfe=1, dither off
  Gp0(b, 0xE2000000u);
  Gp0(b, 0xE3000000u);
  Gp0(b, 0xE4000000u | ((height - 1u) << 10) | (width - 1u));
  Gp0(b, 0xE5000000u);
  Gp0(b, 0xE6000000u);

  Gp0(b, 0x02000000u | (bg_rgb24 & 0xFFFFFFu));
  Gp0(b, PackXY(0, 0));
  Gp0(b, PackXY(static_cast<int16_t>(width), static_cast<int16_t>(height)));
  EmitGpuWaitReady(b);
}

void UploadTexturesAndPalettes(BiosBuilder& b) {
  std::vector<uint16_t> pal4(16u);
  for (uint16_t i = 0; i < 16u; i++) {
    uint16_t r = (i * 2u) & 31u;
    uint16_t g = ((15u - i) * 2u) & 31u;
    uint16_t bl = ((i & 3u) * 8u) & 31u;
    uint16_t stp = (i >= 12u) ? 0x8000u : 0x0000u;
    pal4[i] = stp | (bl << 10) | (g << 5) | r;
  }
  pal4[0] = 0x0000u;
  EmitGpuWaitReady(b);
  Gp0(b, 0xA0000000u);
  Gp0(b, PackXY(0, 256));
  Gp0(b, PackXY(16, 1));
  for (size_t i = 0; i < pal4.size(); i += 2) {
    Gp0(b, (static_cast<uint32_t>(pal4[i + 1]) << 16) | pal4[i]);
  }

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

void EmitDrawRichTextureScene(BiosBuilder& b) {
  UploadTexturesAndPalettes(b);
  const uint32_t pal4 = PackClut(0u, 256u);
  const uint32_t pal8 = PackClut(0u, 258u);
  const uint32_t tp4 = PackTexpage(8u, 0u, 0u);
  const uint32_t tp8 = PackTexpage(9u, 0u, 1u);
  const uint32_t tp15 = PackTexpage(10u, 0u, 2u);

  // Gouraud textured quad (0x3C) 4bpp
  Gp0(b, 0x3C2080FFu); Gp0(b, PackXY(16, 16));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
  Gp0(b, 0x00FF8020u); Gp0(b, PackXY(148, 22)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp4)));
  Gp0(b, 0x0020FF80u); Gp0(b, PackXY(20, 118)); Gp0(b, PackUVPal(0, 63, 0));
  Gp0(b, 0x00FFFFFFu); Gp0(b, PackXY(144, 114)); Gp0(b, PackUVPal(63, 63, 0));

  // Gouraud textured quad (0x3C) 8bpp overbright >128 modulation
  Gp0(b, 0x3CE0E040u); Gp0(b, PackXY(164, 18)); Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal8)));
  Gp0(b, 0x0040E0E0u); Gp0(b, PackXY(302, 16)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp8)));
  Gp0(b, 0x00E040E0u); Gp0(b, PackXY(168, 116)); Gp0(b, PackUVPal(0, 63, 0));
  Gp0(b, 0x00F8F8F8u); Gp0(b, PackXY(300, 120)); Gp0(b, PackUVPal(63, 63, 0));

  // Gouraud textured semi-transparent quad (0x3E) 15bpp ABR=1
  const uint32_t tp15_abr1 = PackTexpage(10u, 0u, 2u, 1u);
  Gp0(b, 0x3EA040FFu); Gp0(b, PackXY(30, 130)); Gp0(b, PackUVPal(0, 0, 0));
  Gp0(b, 0x00FFA040u); Gp0(b, PackXY(290, 130)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp15_abr1)));
  Gp0(b, 0x0040FFA0u); Gp0(b, PackXY(40, 224)); Gp0(b, PackUVPal(0, 63, 0));
  Gp0(b, 0x00C0C0C0u); Gp0(b, PackXY(280, 224)); Gp0(b, PackUVPal(63, 63, 0));
  (void)tp15;
}

// Emit GTE 3D perspective projection of a grid of quads & rotated cube vertices using real MIPS + GTE!
void EmitGte3DScene(BiosBuilder& b) {
  EmitScenePrologue(b, 0x101824u);
  UploadTexturesAndPalettes(b);

  // Set GTE control registers: rotation matrix (slight pitch + yaw), translation (0, 20, 650), OFX/OFY=(160,120), H=240
  auto SetCtrl = [&](uint32_t reg, uint32_t val) {
    b.Li32(T0, val);
    b.Ctc2(T0, reg);
  };
  SetCtrl(0u, 0x03200E00u);  // R12=0x0320, R11=0x0E00 (~0.875)
  SetCtrl(1u, 0xFD400000u);  // R21=0, R13=-0x02C0
  SetCtrl(2u, 0x01800D00u);  // R23=0x0180, R22=0x0D00
  SetCtrl(3u, 0xFB000400u);  // R32=0x0400, R31=-0x0500
  SetCtrl(4u, 0x00000E80u);  // R33=0x0E80
  SetCtrl(5u, 0u);           // TRX
  SetCtrl(6u, 12u);          // TRY
  SetCtrl(7u, 520u);         // TRZ
  SetCtrl(24u, 160u << 16);  // OFX
  SetCtrl(25u, 120u << 16);  // OFY
  SetCtrl(26u, 220u);        // H
  SetCtrl(27u, 0u);          // DQA
  SetCtrl(28u, 0x01000000u); // DQB
  b.Nop();

  // Base RAM workspace for vertices -> 4 receding quads forming a perspective floor + 3D prism
  struct Vtx3D { int16_t x, y, z; uint32_t col; uint8_t u, v; };
  struct Quad3D { Vtx3D v[4]; uint32_t tex; };
  const uint32_t pal4 = PackClut(0u, 256u);
  const uint32_t tp4 = PackTexpage(8u, 0u, 0u);
  const Quad3D quads[] = {
    // Paint strictly back-to-front (far floor, near floor, back wall, side wall) so 1x painter order matches Z-buffer!
    {{{-180,  70, -240, 0xFF8040u, 0, 0}, {  0,  70, -240, 0x40FF80u, 63, 0},
      {-180,  70,  280, 0x4080FFu, 0, 63}, {  0,  70,  280, 0xFFFFFFu, 63, 63}}, tp4},
    {{{   0,  70, -240, 0x40FF80u, 0, 0}, {180,  70, -240, 0xFF40FFu, 63, 0},
      {   0,  70,  280, 0xFFFFFFu, 0, 63}, {180,  70,  280, 0xFFFF40u, 63, 63}}, tp4},
    {{{-100, -90,  140, 0xFFE060u, 0, 0}, {100, -90,  140, 0x60FFE0u, 63, 0},
      {-100,  70,  140, 0x6080FFu, 0, 63}, {100,  70,  140, 0xFFFFFFu, 63, 63}}, tp4},
    {{{-100, -90, -100, 0xFF6080u, 0, 0}, {-100, -90,  140, 0xFFE060u, 63, 0},
      {-100,  70, -100, 0x8060FFu, 0, 63}, {-100,  70,  140, 0x6080FFu, 63, 63}}, tp4},
  };

  uint32_t ram_addr = 0x80020000u;
  for (const auto& q : quads) {
    // Store first 3 vertices into GTE V0, V1, V2 via RAM LWC2 (so PGXP-Mem/CPU tracks both RAM & GTE!)
    for (int i = 0; i < 3; i++) {
      b.SwImm32(ram_addr + static_cast<uint32_t>(i * 8), PackXY(q.v[i].x, q.v[i].y));
      b.SwImm32(ram_addr + static_cast<uint32_t>(i * 8 + 4), static_cast<uint16_t>(q.v[i].z));
    }
    b.Li32(S1, ram_addr);
    b.Lwc2(0u, 0, S1);  b.Lwc2(1u, 4, S1);
    b.Lwc2(2u, 8, S1);  b.Lwc2(3u, 12, S1);
    b.Lwc2(4u, 16, S1); b.Lwc2(5u, 20, S1);
    b.Nop();
    b.GteCmd(0x0280030u);  // RTPT (sf=1) -> populates SXY0..SXY2

    // Also project 4th vertex using RTPS on V0
    b.SwImm32(ram_addr + 24u, PackXY(q.v[3].x, q.v[3].y));
    b.SwImm32(ram_addr + 28u, static_cast<uint16_t>(q.v[3].z));
    b.Lwc2(0u, 24, S1);
    b.Lwc2(1u, 28, S1);
    b.Nop();
    b.GteCmd(0x0180001u);  // RTPS -> shifts FIFO: SXY0=v1, SXY1=v2, SXY2=v3; v0 read earlier!
    b.Nop();

    // Save v0..v3 screen coords into RAM via SWC2 BEFORE issuing RTPS (reload v0..v2 or read SXY0/1/2 twice!)
    // Actually: re-emit v0..v2 RTPT, read SXY0..SXY2 -> T1..T3, then RTPS v3 -> T4!
    b.Lwc2(0u, 0, S1);  b.Lwc2(1u, 4, S1);
    b.Lwc2(2u, 8, S1);  b.Lwc2(3u, 12, S1);
    b.Lwc2(4u, 16, S1); b.Lwc2(5u, 20, S1);
    b.Nop();
    b.GteCmd(0x0280030u);
    b.Nop();
    b.Swc2(12u, 32, S1);  // v0 SXY -> [S1+32]
    b.Swc2(13u, 36, S1);  // v1 SXY -> [S1+36]
    b.Swc2(14u, 40, S1);  // v2 SXY -> [S1+40]
    b.Lwc2(0u, 24, S1); b.Lwc2(1u, 28, S1);
    b.Nop();
    b.GteCmd(0x0180001u);
    b.Nop();
    b.Swc2(14u, 44, S1);  // v3 SXY -> [S1+44]

    // Submit GP0 Textured Gouraud Quad (0x3C) reading vertex XY directly from RAM (PGXP Memory tracks [S1+32..44]!)
    EmitGpuWaitReady(b);
    Gp0(b, 0x3C000000u | (q.v[0].col & 0xFFFFFFu));
    b.Lw(T0, 32, S1); b.Nop(); b.Sw(T0, 0, S0);
    Gp0(b, PackUVPal(q.v[0].u, q.v[0].v, static_cast<uint16_t>(pal4)));
    Gp0(b, q.v[1].col & 0xFFFFFFu);
    b.Lw(T0, 36, S1); b.Nop(); b.Sw(T0, 0, S0);
    Gp0(b, PackUVPal(q.v[1].u, q.v[1].v, static_cast<uint16_t>(q.tex)));
    Gp0(b, q.v[2].col & 0xFFFFFFu);
    b.Lw(T0, 40, S1); b.Nop(); b.Sw(T0, 0, S0);
    Gp0(b, PackUVPal(q.v[2].u, q.v[2].v, 0));
    Gp0(b, q.v[3].col & 0xFFFFFFu);
    b.Lw(T0, 44, S1); b.Nop(); b.Sw(T0, 0, S0);
    Gp0(b, PackUVPal(q.v[3].u, q.v[3].v, 0));

    ram_addr += 64u;
  }
  EmitGpuWaitReady(b);
  b.HaltLoop();
}

struct GpuScene {
  const char* name;
  int resolution_scale = 1;
  bool is_pgxp = false;
  double min_psnr_db = 25.0;
  double min_within1_pct = 96.0;
  std::function<void(BiosBuilder&)> build;
};

std::vector<GpuScene> BuildScenes() {
  std::vector<GpuScene> scenes;

  // 1..7 Original core scenes
  scenes.push_back({"shaded_polys_dither_off", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x201410u);
    Gp0(b, 0xE1000400u);
    Gp0(b, 0x2030D0FFu); Gp0(b, PackXY(20, 20)); Gp0(b, PackXY(130, 35)); Gp0(b, PackXY(55, 110));
    Gp0(b, 0x28FF7020u); Gp0(b, PackXY(150, 25)); Gp0(b, PackXY(280, 30)); Gp0(b, PackXY(165, 100)); Gp0(b, PackXY(295, 105));
    Gp0(b, 0x300000F8u); Gp0(b, PackXY(20, 125)); Gp0(b, 0x0000F800u); Gp0(b, PackXY(145, 130)); Gp0(b, 0x00F80000u); Gp0(b, PackXY(80, 225));
    Gp0(b, 0x38F8F810u); Gp0(b, PackXY(160, 120)); Gp0(b, 0x0010F8F8u); Gp0(b, PackXY(300, 125));
    Gp0(b, 0x00F810F8u); Gp0(b, PackXY(170, 225)); Gp0(b, 0x00F8F8F8u); Gp0(b, PackXY(305, 220));
    b.HaltLoop();
  }});

  scenes.push_back({"shaded_polys_dither_on", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x101018u);
    Gp0(b, 0xE1000600u);
    Gp0(b, 0x38101010u); Gp0(b, PackXY(16, 16));
    Gp0(b, 0x00E0F0F8u); Gp0(b, PackXY(304, 16));
    Gp0(b, 0x0020A040u); Gp0(b, PackXY(16, 224));
    Gp0(b, 0x00F840C0u); Gp0(b, PackXY(304, 224));
    b.HaltLoop();
  }});

  scenes.push_back({"textured_clut_4_8_15", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x141824u);
    UploadTexturesAndPalettes(b);
    const uint32_t pal4 = PackClut(0u, 256u);
    const uint32_t pal8 = PackClut(0u, 258u);
    const uint32_t tp4 = PackTexpage(8u, 0u, 0u);
    const uint32_t tp8 = PackTexpage(9u, 0u, 1u);
    const uint32_t tp15 = PackTexpage(10u, 0u, 2u);

    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(16, 20));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, PackXY(104, 20)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp4)));
    Gp0(b, PackXY(16, 108)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(104, 108)); Gp0(b, PackUVPal(63, 63, 0));

    Gp0(b, 0x2CA0D0FFu);
    Gp0(b, PackXY(116, 20));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal8)));
    Gp0(b, PackXY(204, 20));  Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp8)));
    Gp0(b, PackXY(116, 108)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(204, 108)); Gp0(b, PackUVPal(63, 63, 0));

    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(216, 20));  Gp0(b, PackUVPal(0, 0, 0));
    Gp0(b, PackXY(304, 20));  Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp15)));
    Gp0(b, PackXY(216, 108)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(304, 108)); Gp0(b, PackUVPal(63, 63, 0));

    Gp0(b, 0xE2000000u | (0x1Du << 0) | (0x1Du << 5));
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(40, 124));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, PackXY(280, 124)); Gp0(b, PackUVPal(127, 0, static_cast<uint16_t>(tp4)));
    Gp0(b, PackXY(40, 220));  Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(280, 220)); Gp0(b, PackUVPal(127, 63, 0));
    b.HaltLoop();
  }});

  scenes.push_back({"semi_transparency_modes", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x403020u);
    UploadTexturesAndPalettes(b);
    Gp0(b, 0x284080C0u); Gp0(b, PackXY(16, 16)); Gp0(b, PackXY(160, 16)); Gp0(b, PackXY(16, 224)); Gp0(b, PackXY(160, 224));
    Gp0(b, 0x28C06040u); Gp0(b, PackXY(160, 16)); Gp0(b, PackXY(304, 16)); Gp0(b, PackXY(160, 224)); Gp0(b, PackXY(304, 224));
    for (uint32_t abr = 0; abr < 4u; abr++) {
      Gp0(b, 0xE1000400u | (abr << 5));
      int16_t x0 = static_cast<int16_t>(24 + abr * 70);
      Gp0(b, 0x2A5090D0u);
      Gp0(b, PackXY(x0, 32)); Gp0(b, PackXY(x0 + 58, 32));
      Gp0(b, PackXY(x0, 120)); Gp0(b, PackXY(x0 + 58, 120));
    }
    const uint32_t tp15_abr0 = PackTexpage(10u, 0u, 2u, 0u);
    Gp0(b, 0x2F000000u);
    Gp0(b, PackXY(40, 136));  Gp0(b, PackUVPal(0, 0, 0));
    Gp0(b, PackXY(280, 136)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp15_abr0)));
    Gp0(b, PackXY(40, 212));  Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(280, 212)); Gp0(b, PackUVPal(63, 63, 0));
    b.HaltLoop();
  }});

  scenes.push_back({"lines_sprites_rects", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x181822u);
    UploadTexturesAndPalettes(b);
    const uint32_t pal4 = PackClut(0u, 256u);
    Gp0(b, 0xE1000400u | PackTexpage(8u, 0u, 0u));
    Gp0(b, 0x4040F0FFu); Gp0(b, PackXY(16, 20)); Gp0(b, PackXY(300, 48));
    Gp0(b, 0x580000F8u); Gp0(b, PackXY(20, 60));
    Gp0(b, 0x0000F800u); Gp0(b, PackXY(120, 32));
    Gp0(b, 0x00F80000u); Gp0(b, PackXY(220, 88));
    Gp0(b, 0x00F8F8F8u); Gp0(b, PackXY(300, 50));
    Gp0(b, 0x55555555u);
    Gp0(b, 0x6030C060u); Gp0(b, PackXY(24, 108)); Gp0(b, PackXY(72, 48));
    Gp0(b, 0x78C040F0u); Gp0(b, PackXY(112, 112));
    Gp0(b, 0x65000000u); Gp0(b, PackXY(150, 108)); Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4))); Gp0(b, PackXY(64, 64));
    Gp0(b, 0x75000000u); Gp0(b, PackXY(230, 112)); Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, 0x7D000000u); Gp0(b, PackXY(256, 112)); Gp0(b, PackUVPal(8, 8, static_cast<uint16_t>(pal4)));
    b.HaltLoop();
  }});

  scenes.push_back({"vram_blits_mask_bit", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x202020u);
    Gp0(b, 0x022060E0u); Gp0(b, PackXY(32, 32)); Gp0(b, PackXY(64, 48));
    Gp0(b, 0x02E0C030u); Gp0(b, PackXY(96, 32)); Gp0(b, PackXY(48, 48));
    EmitGpuWaitReady(b);
    Gp0(b, 0x80000000u); Gp0(b, PackXY(32, 32)); Gp0(b, PackXY(32, 110)); Gp0(b, PackXY(64, 48));
    EmitGpuWaitReady(b);
    Gp0(b, 0x80000000u); Gp0(b, PackXY(32, 32)); Gp0(b, PackXY(48, 40)); Gp0(b, PackXY(48, 36));
    Gp0(b, 0xE6000001u);
    Gp0(b, 0x6040E040u); Gp0(b, PackXY(176, 40)); Gp0(b, PackXY(96, 96));
    Gp0(b, 0xE6000002u);
    Gp0(b, 0x604040F0u); Gp0(b, PackXY(156, 80)); Gp0(b, PackXY(136, 96));
    Gp0(b, 0xE6000000u);
    b.HaltLoop();
  }});

  scenes.push_back({"clip_and_offset", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x241818u);
    Gp0(b, 0xE3000000u | (32u << 10) | 48u);
    Gp0(b, 0xE4000000u | (207u << 10) | 271u);
    const uint32_t offX = static_cast<uint32_t>(-24) & 0x7FFu;
    const uint32_t offY = static_cast<uint32_t>(16) & 0x7FFu;
    Gp0(b, 0xE5000000u | (offY << 11) | offX);
    Gp0(b, 0x3800F0F0u); Gp0(b, PackXY(10, 0));
    Gp0(b, 0x00F000F0u); Gp0(b, PackXY(340, 0));
    Gp0(b, 0x00F0F000u); Gp0(b, PackXY(10, 230));
    Gp0(b, 0x00FFFFFFu); Gp0(b, PackXY(340, 230));
    Gp0(b, 0x60E05030u); Gp0(b, PackXY(240, 60)); Gp0(b, PackXY(80, 100));
    b.HaltLoop();
  }});

  // 8. Texture Window wrapping with non-zero offsets across 4bpp/8bpp/15bpp + UV flip
  scenes.push_back({"texture_window_wrapping", 1, false, 25.0, 95.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x101820u);
    UploadTexturesAndPalettes(b);
    const uint32_t pal4 = PackClut(0u, 256u);
    const uint32_t pal8 = PackClut(0u, 258u);
    const uint32_t tp4 = PackTexpage(8u, 0u, 0u);
    const uint32_t tp8 = PackTexpage(9u, 0u, 1u);
    const uint32_t tp15 = PackTexpage(10u, 0u, 2u);

    // 4bpp with 32x32 window starting at offset (16, 16): mask = ~(3<<3) -> encoded as 0x1C, off = (2,2)
    Gp0(b, 0xE2000000u | (0x1Cu << 0) | (0x1Cu << 5) | (2u << 10) | (2u << 15));
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(12, 16));  Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, PackXY(148, 16)); Gp0(b, PackUVPal(127, 0, static_cast<uint16_t>(tp4)));
    Gp0(b, PackXY(12, 112)); Gp0(b, PackUVPal(0, 95, 0));
    Gp0(b, PackXY(148, 112)); Gp0(b, PackUVPal(127, 95, 0));

    // 8bpp mirrored UV + 16x16 window at offset (24, 8)
    Gp0(b, 0xE2000000u | (0x1Eu << 0) | (0x1Eu << 5) | (3u << 10) | (1u << 15));
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(168, 16)); Gp0(b, PackUVPal(63, 63, static_cast<uint16_t>(pal8)));
    Gp0(b, PackXY(304, 16)); Gp0(b, PackUVPal(0, 63, static_cast<uint16_t>(tp8)));
    Gp0(b, PackXY(168, 112)); Gp0(b, PackUVPal(63, 0, 0));
    Gp0(b, PackXY(304, 112)); Gp0(b, PackUVPal(0, 0, 0));

    // 15bpp strip window (maskX=0x18=64px, maskY=0x1F=8px, off=(8,16))
    Gp0(b, 0xE2000000u | (0x18u << 0) | (0x1Fu << 5) | (1u << 10) | (2u << 15));
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(20, 128));  Gp0(b, PackUVPal(0, 0, 0));
    Gp0(b, PackXY(300, 128)); Gp0(b, PackUVPal(191, 0, static_cast<uint16_t>(tp15)));
    Gp0(b, PackXY(20, 224));  Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(300, 224)); Gp0(b, PackUVPal(191, 63, 0));
    b.HaltLoop();
  }});

  // 9. Gouraud-textured modulation (0x34 / 0x3C / 0x3E) with dither ON and overbright >128
  scenes.push_back({"textured_gouraud_modulation", 1, false, 24.5, 95.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x16161Eu);
    Gp0(b, 0xE1000600u);  // dither=1
    EmitDrawRichTextureScene(b);
    b.HaltLoop();
  }});

  // 10. Negative/offscreen coords, CW & CCW winding, needle tris, right/bottom edge clipping
  scenes.push_back({"polygon_winding_clip_edges", 1, false, 25.0, 96.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x182018u);
    // Partially offscreen top-left Gouraud quad
    Gp0(b, 0x38FF2020u); Gp0(b, PackXY(-40, -30));
    Gp0(b, 0x0020FF20u); Gp0(b, PackXY(150, -20));
    Gp0(b, 0x002020FFu); Gp0(b, PackXY(-30, 130));
    Gp0(b, 0x00FFFFFFu); Gp0(b, PackXY(140, 110));

    // Counter-clockwise vs clockwise triangles + bottom-right offscreen overflow
    Gp0(b, 0x30F0F020u); Gp0(b, PackXY(180, 30)); Gp0(b, 0x00F020F0u); Gp0(b, PackXY(350, 260)); Gp0(b, 0x0020F0F0u); Gp0(b, PackXY(360, 10));
    // Thin triangle + bowtie-ish non-planar quad
    Gp0(b, 0x20FF8080u); Gp0(b, PackXY(20, 150)); Gp0(b, PackXY(280, 154)); Gp0(b, PackXY(150, 162));
    Gp0(b, 0x2860C0FFu); Gp0(b, PackXY(30, 175)); Gp0(b, PackXY(160, 230)); Gp0(b, PackXY(160, 175)); Gp0(b, PackXY(30, 230));
    b.HaltLoop();
  }});

  // 11. Hi-res 512x240 display mode (horizontal resolution 512, 240p) + thin diagonal lines
  scenes.push_back({"hires_512x240_mode", 1, false, 24.0, 94.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x141428u, 0x08000002u, 512u, 240u);
    UploadTexturesAndPalettes(b);
    const uint32_t pal4 = PackClut(0u, 256u);
    const uint32_t tp4 = PackTexpage(8u, 0u, 0u);
    Gp0(b, 0x38FF4020u); Gp0(b, PackXY(20, 20)); Gp0(b, 0x0020FF40u); Gp0(b, PackXY(492, 24));
    Gp0(b, 0x004020FFu); Gp0(b, PackXY(30, 220)); Gp0(b, 0x00FFFFFFu); Gp0(b, PackXY(480, 215));
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(160, 48)); Gp0(b, PackUVPal(0, 0, static_cast<uint16_t>(pal4)));
    Gp0(b, PackXY(352, 48)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp4)));
    Gp0(b, PackXY(160, 192)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(352, 192)); Gp0(b, PackUVPal(63, 63, 0));
    Gp0(b, 0x40F8F8F8u); Gp0(b, PackXY(8, 12)); Gp0(b, PackXY(500, 228));
    b.HaltLoop();
  }});

  // 12. VRAM feedback: render -> VRAM-to-VRAM copy -> sample updated VRAM as texture
  scenes.push_back({"vram_feedback_copy_overlap", 1, false, 24.0, 95.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x201818u);
    // Draw colorful pattern into offscreen texpage area (512, 0) in 15-bit direct mode!
    Gp0(b, 0xE3000000u);
    Gp0(b, 0xE4000000u | (511u << 10) | 1023u);
    Gp0(b, 0x38FF0000u); Gp0(b, PackXY(512, 0));
    Gp0(b, 0x0000FF00u); Gp0(b, PackXY(576, 0));
    Gp0(b, 0x000000FFu); Gp0(b, PackXY(512, 64));
    Gp0(b, 0x00FFFF00u); Gp0(b, PackXY(576, 64));
    EmitGpuWaitReady(b);

    // Restore scissor to display (0..319, 0..239)
    Gp0(b, 0xE4000000u | (239u << 10) | 319u);

    // Sample rendered VRAM at (512, 0) as 15bpp texture (TP8, depth=2)
    const uint32_t tp_fb = PackTexpage(8u, 0u, 2u);
    Gp0(b, 0x2D000000u);
    Gp0(b, PackXY(20, 20));  Gp0(b, PackUVPal(0, 0, 0));
    Gp0(b, PackXY(148, 20)); Gp0(b, PackUVPal(63, 0, static_cast<uint16_t>(tp_fb)));
    Gp0(b, PackXY(20, 148)); Gp0(b, PackUVPal(0, 63, 0));
    Gp0(b, PackXY(148, 148)); Gp0(b, PackUVPal(63, 63, 0));

    // Self-copy that drawn display quad (20,20) -> (172, 68) using GP0(0x80)
    EmitGpuWaitReady(b);
    Gp0(b, 0x80000000u); Gp0(b, PackXY(20, 20)); Gp0(b, PackXY(172, 68)); Gp0(b, PackXY(128, 128));
    b.HaltLoop();
  }});

  // 13. 2x resolution scaling accuracy check (smooth 4bpp/8bpp/Gouraud geometry comparing 2x HW vs 1x-scaled SW)
  scenes.push_back({"resolution_scaling_2x", 2, false, 23.0, 93.0, [](BiosBuilder& b) {
    EmitScenePrologue(b, 0x161620u);
    Gp0(b, 0x38FF4020u); Gp0(b, PackXY(20, 20)); Gp0(b, 0x0040FF40u); Gp0(b, PackXY(300, 20));
    Gp0(b, 0x004040FFu); Gp0(b, PackXY(20, 120)); Gp0(b, 0x00FFFFFFu); Gp0(b, PackXY(300, 120));
    Gp0(b, 0x283090E0u); Gp0(b, PackXY(30, 136)); Gp0(b, PackXY(150, 136)); Gp0(b, PackXY(30, 224)); Gp0(b, PackXY(150, 224));
    Gp0(b, 0x60E07040u); Gp0(b, PackXY(170, 136)); Gp0(b, PackXY(120, 88));
    b.HaltLoop();
  }});

  // 14. PGXP 3D perspective depth & texture correction at 2x resolution
  scenes.push_back({"pgxp_3d_perspective_depth", 2, true, 24.0, 92.0, [](BiosBuilder& b) {
    EmitGte3DScene(b);
  }});

  return scenes;
}

bool CaptureScene(EcrApi& api, const std::string& sys_dir, const GpuScene& scene,
                  const GpuRunOptions& opts, RgbImage* out) {
  GpuEnv env;
  env.sys_dir = sys_dir;
  env.opts = opts;
  env.res_scale_str = std::to_string(opts.resolution_scale);
  env.capture_enabled = false;
  g_gpu = &env;

  const bool is_gl = (opts.renderer == "OpenGL");
  const bool is_vk = (opts.renderer == "Vulkan");
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
    std::fprintf(stderr, "FAIL [%s] retro_load_game failed for %s\n", scene.name, opts.renderer.c_str());
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
  } else if (is_vk) {
    if (!env.hw_requested || !env.vk_negotiated || !InitHeadlessVulkanForEnv(env) || !env.hw_cb.context_reset) {
      std::fprintf(stderr, "FAIL [%s] Vulkan headless init/negotiation failed\n", scene.name);
      CleanupVulkanOracleResources(env);
      api.retro_unload_game();
      api.retro_deinit();
      DestroyVulkanInstance(env);
      g_gpu = nullptr;
      return false;
    }
    env.hw_cb.context_reset();
  }

  for (int f = 0; f < 10; f++) {
    env.capture_enabled = (f == 9);
    if (is_gl) eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
    api.retro_run();
  }

  if (is_gl) {
    if (env.hw_cb.context_destroy) {
      eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
      env.hw_cb.context_destroy();
    }
    api.retro_unload_game();
    api.retro_deinit();
    ShutdownHeadlessEgl(env);
  } else if (is_vk) {
    CleanupVulkanOracleResources(env);
    if (env.hw_cb.context_destroy) env.hw_cb.context_destroy();
    api.retro_unload_game();
    api.retro_deinit();
    DestroyVulkanInstance(env);
  } else {
    api.retro_unload_game();
    api.retro_deinit();
  }

  *out = std::move(env.capture);
  g_gpu = nullptr;
  return out->width > 0 && out->height > 0 && !out->rgb.empty();
}

RgbImage ScaleNearest(const RgbImage& src, unsigned target_w, unsigned target_h) {
  if (src.width == target_w && src.height == target_h) return src;
  RgbImage out;
  out.width = target_w;
  out.height = target_h;
  out.rgb.resize(static_cast<size_t>(target_w) * target_h * 3u);
  for (unsigned y = 0; y < target_h; y++) {
    const unsigned sy = (y * src.height) / target_h;
    for (unsigned x = 0; x < target_w; x++) {
      const unsigned sx = (x * src.width) / target_w;
      const size_t si = (static_cast<size_t>(sy) * src.width + sx) * 3u;
      const size_t di = (static_cast<size_t>(y) * target_w + x) * 3u;
      out.rgb[di + 0] = src.rgb[si + 0];
      out.rgb[di + 1] = src.rgb[si + 1];
      out.rgb[di + 2] = src.rgb[si + 2];
    }
  }
  return out;
}

void DumpDiffPng(const std::string& path, const RgbImage& a, const RgbImage& b) {
  if (a.width != b.width || a.height != b.height || a.rgb.size() != b.rgb.size()) return;
  std::vector<uint8_t> diff(a.rgb.size());
  for (size_t i = 0; i < a.rgb.size(); i++) {
    int d = std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i])) * 4;
    diff[i] = static_cast<uint8_t>(std::min(255, d));
  }
  WriteRgbPng(path, a.width, a.height, diff.data());
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

      const int d5 = std::abs((a >> 3) - (b >> 3));
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

double ForegroundRatio(const RgbImage& img, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
  const size_t pixels = static_cast<size_t>(img.width) * img.height;
  size_t fg = 0;
  for (size_t i = 0; i < pixels; i++) {
    const int dr = std::abs(static_cast<int>(img.rgb[i * 3u + 0]) - bg_r);
    const int dg = std::abs(static_cast<int>(img.rgb[i * 3u + 1]) - bg_g);
    const int db = std::abs(static_cast<int>(img.rgb[i * 3u + 2]) - bg_b);
    if (dr > 12 || dg > 12 || db > 12) fg++;
  }
  return 100.0 * static_cast<double>(fg) / static_cast<double>(pixels);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <libecr_android.so> [--no-vulkan] [--dump-dir <dir>]\n", argv[0]);
    return 2;
  }

  bool run_vulkan = true;
  std::string dump_dir;
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--no-vulkan") == 0) run_vulkan = false;
    else if (std::strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) dump_dir = argv[++i];
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

  std::printf("=== EmuCoreR ECR Expanded GPU & PGXP Accuracy Oracle (Golden: Software 1-Thread) ===\n");
  for (const auto& s : scenes) {
    if (s.is_pgxp) {
      // PGXP 3D suite: capture Software (Recompiler, PGXP OFF), GL (PGXP OFF / Mem / CPU / Depth), Vulkan (PGXP CPU + Depth)
      GpuRunOptions sw_opt;
      sw_opt.renderer = "Software"; sw_opt.resolution_scale = s.resolution_scale; sw_opt.cpu_mode = "Recompiler";
      RgbImage sw_1x;
      if (!CaptureScene(api, sys_dir, s, sw_opt, &sw_1x)) {
        std::fprintf(stderr, "FAIL %-26s Golden Software failed\n", s.name);
        total_failures++;
        continue;
      }
      if (!dump_dir.empty()) WriteRgbPng(dump_dir + "/" + s.name + "_sw_off.png", sw_1x.width, sw_1x.height, sw_1x.rgb.data());

      struct PgxpVariant { const char* label; const char* renderer; bool pgxp; bool cpu; bool depth; };
      std::vector<PgxpVariant> variants = {
        {"OpenGL_PGXP_OFF",  "OpenGL", false, false, false},
        {"OpenGL_PGXP_Mem",  "OpenGL", true,  false, false},
        {"OpenGL_PGXP_CPU",  "OpenGL", true,  true,  false},
        {"OpenGL_PGXP_Dep",  "OpenGL", true,  true,  true},
      };
      if (run_vulkan) {
        variants.push_back({"Vulkan_PGXP_Mem", "Vulkan", true, false, false});
        variants.push_back({"Vulkan_PGXP_CPU", "Vulkan", true, true,  false});
        variants.push_back({"Vulkan_PGXP_Dep", "Vulkan", true, true,  true});
      }

      RgbImage gl_pgxp_cpu, vk_pgxp_cpu;
      for (const auto& v : variants) {
        total_checks++;
        GpuRunOptions opt;
        opt.renderer = v.renderer;
        opt.resolution_scale = s.resolution_scale;
        opt.cpu_mode = "Recompiler";
        opt.pgxp_enable = v.pgxp;
        opt.pgxp_cpu = v.cpu;
        opt.pgxp_depth_buffer = v.depth;
        opt.pgxp_preserve_proj_fp = true;
        opt.pgxp_texture_correction = true;
        opt.pgxp_culling = true;
        RgbImage cand;
        if (!CaptureScene(api, sys_dir, s, opt, &cand)) {
          std::printf("FAIL %-26s variant=%-16s (init/render error)\n", s.name, v.label);
          total_failures++;
          continue;
        }
        if (!dump_dir.empty()) WriteRgbPng(dump_dir + "/" + s.name + "_" + v.label + ".png", cand.width, cand.height, cand.rgb.data());
        const RgbImage ref_scaled = ScaleNearest(sw_1x, cand.width, cand.height);
        const auto m = CompareImages(ref_scaled, cand);
        const double fg_pct = ForegroundRatio(cand, 0x24, 0x18, 0x10);
        const bool ok = (cand.width == ref_scaled.width && cand.height == ref_scaled.height &&
                         fg_pct >= 5.0 && m.psnr_db >= 21.0);
        std::printf("%s %-26s variant=%-16s (%ux%u) fg=%.1f%% MAE=%.2f PSNR=%.2f dB\n",
                    ok ? "PASS" : "FAIL", s.name, v.label, cand.width, cand.height,
                    fg_pct, m.mae_8bit, m.psnr_db);
        if (!ok) total_failures++;
        if (std::strcmp(v.label, "OpenGL_PGXP_CPU") == 0) gl_pgxp_cpu = std::move(cand);
        if (std::strcmp(v.label, "Vulkan_PGXP_CPU") == 0) vk_pgxp_cpu = std::move(cand);
      }

      if (!gl_pgxp_cpu.rgb.empty() && !vk_pgxp_cpu.rgb.empty()) {
        total_checks++;
        const auto cross = CompareImages(gl_pgxp_cpu, vk_pgxp_cpu);
        const bool cross_ok = (gl_pgxp_cpu.width == vk_pgxp_cpu.width &&
                               gl_pgxp_cpu.height == vk_pgxp_cpu.height &&
                               cross.psnr_db >= 26.0 && cross.within1lsb5_pct >= 94.0);
        std::printf("%s %-26s variant=GL_vs_VK_PGXP   (%ux%u) exact5=%.2f%% +/-1LSB5=%.2f%% MAE=%.3f PSNR=%.2f dB\n",
                    cross_ok ? "PASS" : "FAIL", s.name, gl_pgxp_cpu.width, gl_pgxp_cpu.height,
                    cross.exact5_pct, cross.within1lsb5_pct, cross.mae_8bit, cross.psnr_db);
        if (!dump_dir.empty()) DumpDiffPng(dump_dir + "/" + s.name + "_gl_vs_vk_diff4x.png", gl_pgxp_cpu, vk_pgxp_cpu);
        if (!cross_ok) total_failures++;
      }
      continue;
    }

    // Standard SW vs ThreadedSW vs OpenGL vs Vulkan accuracy checks
    GpuRunOptions sw_opt;
    sw_opt.renderer = "Software"; sw_opt.resolution_scale = s.resolution_scale;
    RgbImage golden;
    if (!CaptureScene(api, sys_dir, s, sw_opt, &golden)) {
      std::fprintf(stderr, "FAIL %-26s Golden Software failed\n", s.name);
      total_failures++;
      continue;
    }
    if (!dump_dir.empty()) {
      WriteRgbPng(dump_dir + "/" + s.name + "_sw.png", golden.width, golden.height, golden.rgb.data());
    }

    // 1. SoftwareThreaded (only 1x scale differs or same; SW always bit-exact to itself)
    {
      total_checks++;
      GpuRunOptions thr_opt = sw_opt;
      thr_opt.use_thread = true;
      RgbImage thr;
      if (!CaptureScene(api, sys_dir, s, thr_opt, &thr)) {
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

    // 2. HardwareOpenGL
    RgbImage gl;
    bool have_gl = false;
    {
      total_checks++;
      GpuRunOptions gl_opt;
      gl_opt.renderer = "OpenGL";
      gl_opt.resolution_scale = s.resolution_scale;
      if (!CaptureScene(api, sys_dir, s, gl_opt, &gl)) {
        std::printf("FAIL %-26s backend=HardwareOpenGL   (EGL/GLES error)\n", s.name);
        total_failures++;
      } else {
        have_gl = true;
        const RgbImage ref_scaled = ScaleNearest(golden, gl.width, gl.height);
        const auto m = CompareImages(ref_scaled, gl);
        const bool dims_ok = (gl.width == golden.width * static_cast<unsigned>(s.resolution_scale) &&
                              gl.height == golden.height * static_cast<unsigned>(s.resolution_scale));
        const bool acc_ok = dims_ok && (m.psnr_db >= s.min_psnr_db) && (m.within1lsb5_pct >= s.min_within1_pct);
        std::printf("%s %-26s backend=HardwareOpenGL   (%ux%u) exact5=%.2f%% +/-1LSB5=%.2f%% MAE=%.3f maxDiff=%d PSNR=%.2f dB\n",
                    acc_ok ? "PASS" : "FAIL", s.name, gl.width, gl.height,
                    m.exact5_pct, m.within1lsb5_pct, m.mae_8bit, m.max_diff_8bit, m.psnr_db);
        if (!dump_dir.empty()) {
          WriteRgbPng(dump_dir + "/" + s.name + "_gl.png", gl.width, gl.height, gl.rgb.data());
          DumpDiffPng(dump_dir + "/" + s.name + "_gl_vs_sw_diff4x.png", ref_scaled, gl);
        }
        if (!acc_ok) total_failures++;
      }
    }

    // 3. HardwareVulkan
    if (run_vulkan) {
      total_checks++;
      GpuRunOptions vk_opt;
      vk_opt.renderer = "Vulkan";
      vk_opt.resolution_scale = s.resolution_scale;
      RgbImage vk;
      if (!CaptureScene(api, sys_dir, s, vk_opt, &vk)) {
        std::printf("FAIL %-26s backend=HardwareVulkan   (Vulkan headless error)\n", s.name);
        total_failures++;
      } else {
        const RgbImage ref_scaled = ScaleNearest(golden, vk.width, vk.height);
        const auto m = CompareImages(ref_scaled, vk);
        const bool dims_ok = (vk.width == golden.width * static_cast<unsigned>(s.resolution_scale) &&
                              vk.height == golden.height * static_cast<unsigned>(s.resolution_scale));
        const bool acc_ok = dims_ok && (m.psnr_db >= s.min_psnr_db) && (m.within1lsb5_pct >= s.min_within1_pct);
        std::printf("%s %-26s backend=HardwareVulkan   (%ux%u) exact5=%.2f%% +/-1LSB5=%.2f%% MAE=%.3f maxDiff=%d PSNR=%.2f dB\n",
                    acc_ok ? "PASS" : "FAIL", s.name, vk.width, vk.height,
                    m.exact5_pct, m.within1lsb5_pct, m.mae_8bit, m.max_diff_8bit, m.psnr_db);
        if (!dump_dir.empty()) {
          WriteRgbPng(dump_dir + "/" + s.name + "_vk.png", vk.width, vk.height, vk.rgb.data());
          DumpDiffPng(dump_dir + "/" + s.name + "_vk_vs_sw_diff4x.png", ref_scaled, vk);
        }
        if (!acc_ok) total_failures++;

        if (have_gl && gl.width == vk.width && gl.height == vk.height) {
          total_checks++;
          const auto cross = CompareImages(gl, vk);
          const bool cross_ok = (cross.psnr_db >= 26.0) && (cross.within1lsb5_pct >= 95.0);
          std::printf("%s %-26s backend=GL_vs_Vulkan     (%ux%u) exact5=%.2f%% +/-1LSB5=%.2f%% MAE=%.3f PSNR=%.2f dB\n",
                      cross_ok ? "PASS" : "FAIL", s.name, vk.width, vk.height,
                      cross.exact5_pct, cross.within1lsb5_pct, cross.mae_8bit, cross.psnr_db);
          if (!cross_ok) total_failures++;
        }
      }
    }
  }

  std::remove((sys_dir + "/gpu_oracle_bios.bin").c_str());
  rmdir(tmp_dir);
  std::printf("GPU Oracle Summary: %d comparisons, %d failures\n", total_checks, total_failures);
  return total_failures == 0 ? 0 : 1;
}

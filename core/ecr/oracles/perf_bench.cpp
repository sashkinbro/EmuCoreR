// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EmuCoreR ECR Headless Performance Benchmark Suite
// Measures CPU JIT, GTE/PGXP, BIOS, and Real PS1 Game throughput across:
//   - Interpreter / CachedInterpreter / Recompiler (MMap) / Recompiler + PGXP Mem / PGXP CPU
//   - Software / OpenGL ES 3.0 / Vulkan 1.0 (1x and 2x internal resolution)
// Reports: Avg ms/frame, P95 ms/frame, Wall FPS, Emulation % (vs 59.94 fps NTSC), Speedup.
#include "mips_emitter.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>
#include <libretro_vulkan.h>

#include <algorithm>
#include <chrono>
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

struct BenchConfig {
  const char* label;
  std::string cpu_mode = "Recompiler";
  std::string fastmem_mode = "MMap";
  std::string renderer = "Software";
  bool use_thread = false;
  int resolution_scale = 1;
  bool pgxp_enable = false;
  bool pgxp_cpu = false;
  bool pgxp_depth = false;
  bool fast_boot = false;
};

struct RgbCapture {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<uint8_t> rgb;
};

struct BenchEnv {
  std::string sys_dir;
  std::string bios_file = "bench_bios.bin";
  BenchConfig cfg{"default"};
  std::string res_scale_str = "1";
  bool capture_last = false;
  bool dump_requested = false;
  retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_RGB565;
  retro_hw_render_callback hw_cb{};
  bool hw_requested = false;

  // EGL / GLES
  GLuint fbo = 0, color_tex = 0, depth_rb = 0;
  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  EGLContext egl_context = EGL_NO_CONTEXT;

  // Vulkan
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

  RgbCapture capture;
};

BenchEnv* g_env = nullptr;

inline uint8_t Expand5To8(uint32_t v5) {
  return static_cast<uint8_t>((v5 << 3) | (v5 >> 2));
}

bool InitHeadlessEgl(BenchEnv& env) {
  env.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (env.egl_display == EGL_NO_DISPLAY) return false;
  EGLint major = 0, minor = 0;
  if (eglInitialize(env.egl_display, &major, &minor) != EGL_TRUE) return false;
  const EGLint cfg_attribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_NONE
  };
  EGLConfig cfg = nullptr;
  EGLint num_cfg = 0;
  if (eglChooseConfig(env.egl_display, cfg_attribs, &cfg, 1, &num_cfg) != EGL_TRUE || num_cfg < 1) return false;
  const EGLint pb_attribs[] = {EGL_WIDTH, 1280, EGL_HEIGHT, 960, EGL_NONE};
  env.egl_surface = eglCreatePbufferSurface(env.egl_display, cfg, pb_attribs);
  if (env.egl_surface == EGL_NO_SURFACE) return false;
  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  env.egl_context = eglCreateContext(env.egl_display, cfg, EGL_NO_CONTEXT, ctx_attribs);
  if (env.egl_context == EGL_NO_CONTEXT) return false;
  if (eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context) != EGL_TRUE) return false;

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
  return status == GL_FRAMEBUFFER_COMPLETE;
}

void ShutdownHeadlessEgl(BenchEnv& env) {
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

void VkSetImage(void* handle, const struct retro_vulkan_image* image, uint32_t, const VkSemaphore*, uint32_t) {
  auto* env = static_cast<BenchEnv*>(handle);
  if (image) { env->vk_current_image = *image; env->vk_has_image = true; }
  else { env->vk_has_image = false; }
}
uint32_t VkGetSyncIndex(void*) { return 0; }
uint32_t VkGetSyncIndexMask(void*) { return 1; }
void VkSetCommandBuffers(void*, uint32_t, const VkCommandBuffer*) {}
void VkWaitSyncIndex(void* handle) {
  auto* env = static_cast<BenchEnv*>(handle);
  if (env->vk_ctx.queue != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(env->vk_queue_mutex);
    vkQueueWaitIdle(env->vk_ctx.queue);
  }
}
void VkLockQueue(void* handle) { static_cast<BenchEnv*>(handle)->vk_queue_mutex.lock(); }
void VkUnlockQueue(void* handle) { static_cast<BenchEnv*>(handle)->vk_queue_mutex.unlock(); }
void VkSetSignalSemaphore(void*, VkSemaphore) {}

bool InitHeadlessVulkanForEnv(BenchEnv& env) {
  const VkApplicationInfo* app_info = (env.vk_negotiated && env.vk_neg.get_application_info) ?
                                      env.vk_neg.get_application_info() : nullptr;
  VkApplicationInfo fallback{};
  fallback.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  if (!app_info) {
    fallback.pApplicationName = "EcrPerfBench";
    fallback.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    fallback.pEngineName = "EmuCoreR";
    fallback.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    fallback.apiVersion = VK_API_VERSION_1_0;
    app_info = &fallback;
  }
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = app_info;
  if (vkCreateInstance(&ici, nullptr, &env.vk_instance) != VK_SUCCESS) return false;
  if (!env.vk_negotiated || !env.vk_neg.create_device) return false;
  if (!env.vk_neg.create_device(&env.vk_ctx, env.vk_instance, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                vkGetInstanceProcAddr, nullptr, 0, nullptr, 0, nullptr)) return false;

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
  if (vkCreateCommandPool(env.vk_ctx.device, &cpci, nullptr, &env.vk_cmd_pool) != VK_SUCCESS) return false;

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = env.vk_cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  return vkAllocateCommandBuffers(env.vk_ctx.device, &cbai, &env.vk_cmd_buf) == VK_SUCCESS;
}

void CleanupVulkanOracleResources(BenchEnv& env) {
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

void DestroyVulkanInstance(BenchEnv& env) {
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

bool ReadbackVulkanFrame(BenchEnv& env, unsigned width, unsigned height) {
  if (!env.vk_has_image || env.vk_current_image.create_info.image == VK_NULL_HANDLE) return false;
  const VkDeviceSize byte_size = static_cast<VkDeviceSize>(width) * height * 4u;
  if (env.vk_staging_buf == VK_NULL_HANDLE || env.vk_staging_size < byte_size) {
    if (env.vk_staging_buf) vkDestroyBuffer(env.vk_ctx.device, env.vk_staging_buf, nullptr);
    if (env.vk_staging_mem) vkFreeMemory(env.vk_ctx.device, env.vk_staging_mem, nullptr);
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = byte_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(env.vk_ctx.device, &bci, nullptr, &env.vk_staging_buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(env.vk_ctx.device, env.vk_staging_buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(env.vk_ctx.gpu, &mp);
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((req.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        idx = i;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
      }
    }
    if (idx == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = idx;
    if (vkAllocateMemory(env.vk_ctx.device, &mai, nullptr, &env.vk_staging_mem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(env.vk_ctx.device, env.vk_staging_buf, env.vk_staging_mem, 0) != VK_SUCCESS) return false;
    env.vk_staging_size = byte_size;
  }

  std::lock_guard<std::mutex> lock(env.vk_queue_mutex);
  vkQueueWaitIdle(env.vk_ctx.queue);
  vkResetCommandBuffer(env.vk_cmd_buf, 0);
  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(env.vk_cmd_buf, &cbbi);

  const VkImage img = env.vk_current_image.create_info.image;
  const VkImageLayout old_layout = env.vk_current_image.image_layout;
  VkImageMemoryBarrier b1{};
  b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b1.oldLayout = old_layout;
  b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.image = img;
  b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(env.vk_cmd_buf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(env.vk_cmd_buf, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         env.vk_staging_buf, 1, &region);
  b1.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b1.newLayout = old_layout;
  vkCmdPipelineBarrier(env.vk_cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);
  vkEndCommandBuffer(env.vk_cmd_buf);

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &env.vk_cmd_buf;
  vkQueueSubmit(env.vk_ctx.queue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(env.vk_ctx.queue);

  void* mapped = nullptr;
  if (vkMapMemory(env.vk_ctx.device, env.vk_staging_mem, 0, byte_size, 0, &mapped) != VK_SUCCESS) return false;
  VkMappedMemoryRange rng{};
  rng.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  rng.memory = env.vk_staging_mem;
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
      *static_cast<const char**>(data) = g_env->sys_dir.c_str();
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* var = static_cast<retro_variable*>(data);
      const std::string key(var->key ? var->key : "");
      const char* val = nullptr;
      if (key == "ecr_GPU_Renderer") val = g_env->cfg.renderer.c_str();
      else if (key == "ecr_GPU_UseThread") val = g_env->cfg.use_thread ? "true" : "false";
      else if (key == "ecr_GPU_ResolutionScale") val = g_env->res_scale_str.c_str();
      else if (key == "ecr_GPU_TextureFilter") val = "Nearest";
      else if (key == "ecr_GPU_MSAA") val = "1";
      else if (key == "ecr_GPU_PGXPEnable") val = g_env->cfg.pgxp_enable ? "true" : "false";
      else if (key == "ecr_GPU_PGXPCPU") val = g_env->cfg.pgxp_cpu ? "true" : "false";
      else if (key == "ecr_GPU_PGXPCulling") val = "true";
      else if (key == "ecr_GPU_PGXPTextureCorrection") val = "true";
      else if (key == "ecr_GPU_PGXPPreserveProjFP") val = "true";
      else if (key == "ecr_GPU_PGXPDepthBuffer") val = g_env->cfg.pgxp_depth ? "true" : "false";
      else if (key == "ecr_CPU_ExecutionMode") val = g_env->cfg.cpu_mode.c_str();
      else if (key == "ecr_CPU_RecompilerBlockLinking") val = "true";
      else if (key == "ecr_CPU_RecompilerICache") val = "false";
      else if (key == "ecr_CPU_RecompilerMemoryExceptions") val = "false";
      else if (key == "ecr_CPU_FastmemMode") val = g_env->cfg.fastmem_mode.c_str();
      else if (key == "ecr_Console_Region") val = "NTSC-U";
      else if (key == "ecr_BIOS_PathNTSCU") val = g_env->bios_file.c_str();
      else if (key == "ecr_BIOS_PathNTSCJ") val = g_env->bios_file.c_str();
      else if (key == "ecr_BIOS_PathPAL") val = g_env->bios_file.c_str();
      else if (key == "ecr_BIOS_PatchFastBoot") val = g_env->cfg.fast_boot ? "true" : "false";
      else if (key == "ecr_MemoryCards_Card1Type" || key == "ecr_MemoryCards_Card2Type") val = "None";
      else if (key == "ecr_Main_ApplyGameSettings") val = "false";
      else if (key == "ecr_CDROM_ReadThread") val = "false";
      else if (key == "ecr_Audio_FastHook") val = "true";
      var->value = val;
      return val != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *static_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
      *static_cast<int*>(data) = 3;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      g_env->pixel_format = *static_cast<const retro_pixel_format*>(data);
      return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
      if (!data) return false;
      if (g_env->cfg.renderer == "Vulkan") { *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_VULKAN; return true; }
      if (g_env->cfg.renderer == "OpenGL") { *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGLES_VERSION; return true; }
      return false;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      if (!data) return false;
      auto* cb = static_cast<retro_hw_render_callback*>(data);
      if (g_env->cfg.renderer == "OpenGL") {
        cb->get_proc_address = [](const char* sym) -> retro_proc_address_t {
          return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym));
        };
        cb->get_current_framebuffer = []() -> uintptr_t { return static_cast<uintptr_t>(g_env->fbo); };
        cb->depth = true;
        g_env->hw_cb = *cb;
        g_env->hw_requested = true;
        return true;
      }
      if (g_env->cfg.renderer == "Vulkan") {
        g_env->hw_cb = *cb;
        g_env->hw_requested = true;
        return true;
      }
      return false;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
      if (!data || g_env->cfg.renderer != "Vulkan") return false;
      g_env->vk_neg = *static_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(data);
      g_env->vk_negotiated = true;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
      if (!data || g_env->cfg.renderer != "Vulkan" || g_env->vk_ctx.device == VK_NULL_HANDLE) return false;
      *static_cast<const retro_hw_render_interface**>(data) =
        reinterpret_cast<const retro_hw_render_interface*>(&g_env->vk_iface);
      return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
      return true;
    default:
      return false;
  }
}

void VideoCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (!g_env || !g_env->capture_last || !g_env->dump_requested || !data || width == 0 || height == 0) return;

  if (data == RETRO_HW_FRAME_BUFFER_VALID) {
    if (g_env->cfg.renderer == "Vulkan") {
      ReadbackVulkanFrame(*g_env, width, height);
      return;
    }
    glFinish();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_env->fbo);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u, 0u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    g_env->capture.width = width;
    g_env->capture.height = height;
    g_env->capture.rgb.resize(static_cast<size_t>(width) * height * 3u);
    for (unsigned y = 0; y < height; y++) {
      const unsigned sy = g_env->hw_cb.bottom_left_origin ? (height - 1u - y) : y;
      const uint8_t* sr = rgba.data() + static_cast<size_t>(sy) * width * 4u;
      uint8_t* dr = g_env->capture.rgb.data() + static_cast<size_t>(y) * width * 3u;
      for (unsigned x = 0; x < width; x++) {
        dr[x * 3u + 0u] = sr[x * 4u + 0u];
        dr[x * 3u + 1u] = sr[x * 4u + 1u];
        dr[x * 3u + 2u] = sr[x * 4u + 2u];
      }
    }
    return;
  }

  g_env->capture.width = width;
  g_env->capture.height = height;
  g_env->capture.rgb.resize(static_cast<size_t>(width) * height * 3u);
  const auto* src = static_cast<const uint8_t*>(data);
  for (unsigned y = 0; y < height; y++) {
    const uint8_t* srow = src + y * pitch;
    uint8_t* drow = g_env->capture.rgb.data() + static_cast<size_t>(y) * width * 3u;
    if (g_env->pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
      for (unsigned x = 0; x < width; x++) {
        const uint32_t px = *reinterpret_cast<const uint32_t*>(srow + x * 4u);
        drow[x * 3u + 0u] = static_cast<uint8_t>((px >> 16) & 0xFFu);
        drow[x * 3u + 1u] = static_cast<uint8_t>((px >> 8) & 0xFFu);
        drow[x * 3u + 2u] = static_cast<uint8_t>(px & 0xFFu);
      }
    } else if (g_env->pixel_format == RETRO_PIXEL_FORMAT_0RGB1555) {
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
}

struct BenchStats {
  bool ok = false;
  double total_ms = 0.0;
  double avg_ms = 0.0;
  double p95_ms = 0.0;
  double fps = 0.0;
  RgbCapture last_frame;
};

BenchStats RunBenchmark(EcrApi& api, const std::string& sys_dir, const std::string& bios_name,
                        const std::string& game_path, const BenchConfig& cfg,
                        uint32_t warmup_frames, uint32_t measure_frames, bool want_dump) {
  BenchStats st;
  BenchEnv env;
  env.sys_dir = sys_dir;
  env.bios_file = bios_name;
  env.cfg = cfg;
  env.res_scale_str = std::to_string(cfg.resolution_scale);
  env.dump_requested = want_dump;
  g_env = &env;

  const bool is_gl = (cfg.renderer == "OpenGL");
  const bool is_vk = (cfg.renderer == "Vulkan");
  if (is_gl && !InitHeadlessEgl(env)) { g_env = nullptr; return st; }

  api.retro_set_environment(EnvironmentCallback);
  api.retro_set_video_refresh(VideoCallback);
  api.retro_set_audio_sample([](int16_t, int16_t) {});
  api.retro_set_audio_sample_batch([](const int16_t*, size_t n) { return n; });
  api.retro_set_input_poll([]() {});
  api.retro_set_input_state([](unsigned, unsigned, unsigned, unsigned) { return int16_t(0); });

  api.retro_init();
  retro_game_info gi{};
  gi.path = game_path.empty() ? nullptr : game_path.c_str();
  if (!api.retro_load_game(game_path.empty() ? nullptr : &gi)) {
    api.retro_deinit();
    ShutdownHeadlessEgl(env);
    g_env = nullptr;
    return st;
  }

  if (is_gl) {
    if (!env.hw_requested || !env.hw_cb.context_reset) {
      api.retro_unload_game(); api.retro_deinit(); ShutdownHeadlessEgl(env); g_env = nullptr; return st;
    }
    eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
    env.hw_cb.context_reset();
  } else if (is_vk) {
    if (!env.hw_requested || !env.vk_negotiated || !InitHeadlessVulkanForEnv(env) || !env.hw_cb.context_reset) {
      CleanupVulkanOracleResources(env); api.retro_unload_game(); api.retro_deinit();
      DestroyVulkanInstance(env); g_env = nullptr; return st;
    }
    env.hw_cb.context_reset();
  }

  for (uint32_t f = 0; f < warmup_frames; f++) {
    if (is_gl) eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
    api.retro_run();
  }

  std::vector<double> frame_ms;
  frame_ms.reserve(measure_frames);
  const auto t0 = std::chrono::steady_clock::now();
  for (uint32_t f = 0; f < measure_frames; f++) {
    env.capture_last = (f + 1u == measure_frames);
    if (is_gl) eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
    const auto f0 = std::chrono::steady_clock::now();
    api.retro_run();
    const auto f1 = std::chrono::steady_clock::now();
    frame_ms.push_back(std::chrono::duration<double, std::milli>(f1 - f0).count());
  }
  if (is_gl) glFinish();
  else if (is_vk && env.vk_ctx.queue != VK_NULL_HANDLE) vkQueueWaitIdle(env.vk_ctx.queue);
  const auto t1 = std::chrono::steady_clock::now();

  if (is_gl) {
    if (env.hw_cb.context_destroy) {
      eglMakeCurrent(env.egl_display, env.egl_surface, env.egl_surface, env.egl_context);
      env.hw_cb.context_destroy();
    }
    api.retro_unload_game(); api.retro_deinit(); ShutdownHeadlessEgl(env);
  } else if (is_vk) {
    CleanupVulkanOracleResources(env);
    if (env.hw_cb.context_destroy) env.hw_cb.context_destroy();
    api.retro_unload_game(); api.retro_deinit(); DestroyVulkanInstance(env);
  } else {
    api.retro_unload_game(); api.retro_deinit();
  }

  st.total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  st.avg_ms = st.total_ms / static_cast<double>(measure_frames);
  st.fps = (static_cast<double>(measure_frames) * 1000.0) / st.total_ms;
  std::sort(frame_ms.begin(), frame_ms.end());
  st.p95_ms = frame_ms[std::min(frame_ms.size() - 1u, static_cast<size_t>(frame_ms.size() * 0.95))];
  st.last_frame = std::move(env.capture);
  st.ok = true;
  g_env = nullptr;
  return st;
}

void PrintRow(const char* workload, const BenchConfig& cfg, uint32_t frames,
              const BenchStats& st, double base_fps) {
  if (!st.ok) {
    std::printf("ERR  %-22s %-28s FAILED\n", workload, cfg.label);
    return;
  }
  const double pct = (st.fps / 59.94) * 100.0;
  const double speedup = (base_fps > 0.0) ? (st.fps / base_fps) : 1.0;
  std::printf("BENCH %-21s %-28s %4u fr | %6.2f ms (p95 %6.2f) | %7.1f FPS (%6.0f%%) | %5.2fx\n",
              workload, cfg.label, frames, st.avg_ms, st.p95_ms, st.fps, pct, speedup);
}

void BuildCpuAluKernel(BiosBuilder& b) {
  b.Seek(0x0u); b.J(0xBFC00400u); b.Nop();
  b.Seek(0x400u);
  b.Li32(S0, 0x80020000u);
  b.Li32(S1, 0x12345678u);
  b.Li32(T0, 1u);
  b.Li32(T1, 2u);
  b.Li32(T2, 3u);
  b.Li32(T3, 0x9E3779B9u);

  // Hot tight ALU + shift + RAM read/write inner loop repeated forever (driven for N frames)
  BiosBuilder::Label outer, inner;
  b.Bind(outer);
  b.Li32(S2, 500u);
  b.Bind(inner);
  b.Addu(T0, T0, T1);
  b.Xor(T1, T1, T3);
  b.Sll(T4, T0, 5);
  b.Srl(T5, T1, 7);
  b.Subu(T2, T4, T5);
  b.Sw(T2, 0, S0);
  b.Lw(T6, 0, S0);
  b.Addiu(S2, S2, -1);
  b.Bne(S2, ZERO, inner);
  b.Addu(T0, T0, T6);  // branch delay slot does useful work + exercises load delay timing
  b.Beq(ZERO, ZERO, outer);
  b.Nop();
  b.HaltLoop();
}

void BuildGte3DKernel(BiosBuilder& b) {
  b.Seek(0x0u); b.J(0xBFC00400u); b.Nop();
  b.Seek(0x400u);
  b.Li32(T0, 0x40400000u);  // CU2=1
  b.Mtc0(T0, 12u);
  b.Nop();

  b.Li32(S0, 0x80020000u);
  b.SwImm32(0x80020000u, 0xFF800050u);
  b.SwImm32(0x80020004u, 0x00000040u);
  b.SwImm32(0x80020008u, 0x0060FF90u);
  b.SwImm32(0x8002000Cu, 0x00000050u);
  b.SwImm32(0x80020010u, 0x00300040u);
  b.SwImm32(0x80020014u, 0x00000060u);

  auto SetCtrl = [&](uint32_t r, uint32_t v) { b.Li32(T0, v); b.Ctc2(T0, r); };
  SetCtrl(0u, 0x03200E00u); SetCtrl(1u, 0xFD400000u); SetCtrl(2u, 0x01800D00u);
  SetCtrl(3u, 0xFB000400u); SetCtrl(4u, 0x00000E80u);
  SetCtrl(7u, 520u); SetCtrl(24u, 160u << 16); SetCtrl(25u, 120u << 16); SetCtrl(26u, 220u);
  b.Nop();

  BiosBuilder::Label loop;
  b.Bind(loop);
  b.Lwc2(0u, 0, S0); b.Lwc2(1u, 4, S0);
  b.Lwc2(2u, 8, S0); b.Lwc2(3u, 12, S0);
  b.Lwc2(4u, 16, S0); b.Lwc2(5u, 20, S0);
  b.Nop();
  b.GteCmd(0x0280030u);  // RTPT
  b.GteCmd(0x1400006u);  // NCLIP
  b.GteCmd(0x158002Du);  // AVSZ3
  b.Mfc2(V0, 24u);       // MAC0
  b.Swc2(12u, 32, S0);   // SXY0 -> RAM
  b.Swc2(13u, 36, S0);   // SXY1 -> RAM
  b.Swc2(14u, 40, S0);   // SXY2 -> RAM
  b.Lw(T1, 32, S0);
  b.Lw(T2, 36, S0);
  b.Nop();
  b.Addu(V1, T1, T2);
  b.Beq(ZERO, ZERO, loop);
  b.Sw(V1, 44, S0);
}

bool CopyFile(const std::string& src, const std::string& dst) {
  FILE* in = std::fopen(src.c_str(), "rb");
  if (!in) return false;
  FILE* out = std::fopen(dst.c_str(), "wb");
  if (!out) { std::fclose(in); return false; }
  std::vector<uint8_t> buf(64 * 1024);
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) std::fwrite(buf.data(), 1, n, out);
  std::fclose(in);
  std::fclose(out);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <libecr_android.so> [--bios <path>] [--game <name=cue>]... [--dump-dir <dir>] [--skip-interp]\n", argv[0]);
    return 2;
  }

  std::string bios_path;
  struct GameArg { std::string label; std::string path; };
  std::vector<GameArg> games;
  std::string dump_dir;
  bool skip_interp = false;
  uint32_t game_frames = 360u;

  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) bios_path = argv[++i];
    else if (std::strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) dump_dir = argv[++i];
    else if (std::strcmp(argv[i], "--skip-interp") == 0) skip_interp = true;
    else if (std::strcmp(argv[i], "--game-frames") == 0 && i + 1 < argc) {
      game_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      if (game_frames < 60u) game_frames = 60u;
    } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
      std::string s = argv[++i];
      auto eq = s.find('=');
      if (eq != std::string::npos) games.push_back({s.substr(0, eq), s.substr(eq + 1)});
      else games.push_back({"game", s});
    }
  }

  EcrApi api;
  if (!api.Load(argv[1])) return 2;

  char tmp_template[] = "/data/local/tmp/ecr-perf-bench-XXXXXX";
  char* tmp_dir = mkdtemp(tmp_template);
  if (!tmp_dir) { std::perror("mkdtemp"); return 2; }
  const std::string sys_dir(tmp_dir);
  const std::string synth_bios = sys_dir + "/bench_bios.bin";

  std::printf("=== EmuCoreR ECR Headless Performance Benchmark ===\n");

  const std::vector<BenchConfig> cpu_cfgs = {
    {"Interp_SW",         "Interpreter",       "Disabled", "Software", false, 1, false, false, false},
    {"Cached_SW",         "CachedInterpreter", "Disabled", "Software", false, 1, false, false, false},
    {"JIT_MMap_SW",       "Recompiler",        "MMap",     "Software", false, 1, false, false, false},
    {"JIT_LUT_SW",        "Recompiler",        "LUT",      "Software", false, 1, false, false, false},
    {"JIT_PGXP_Mem_SW",   "Recompiler",        "MMap",     "Software", false, 1, true,  false, false},
    {"JIT_PGXP_CPU_SW",   "Recompiler",        "MMap",     "Software", false, 1, true,  true,  false},
  };

  // 1. CPU ALU/Mem kernel
  {
    BiosBuilder b;
    BuildCpuAluKernel(b);
    b.WriteFile(synth_bios);
    double base_fps = 0.0;
    for (const auto& c : cpu_cfgs) {
      if (skip_interp && c.cpu_mode == "Interpreter") continue;
      auto st = RunBenchmark(api, sys_dir, "bench_bios.bin", "", c, 15, 60, false);
      if (base_fps == 0.0 && st.ok) base_fps = st.fps;
      PrintRow("synth_cpu_alu_mem", c, 60, st, base_fps);
    }
  }

  // 2. GTE 3D transform + PGXP load-delay kernel
  {
    BiosBuilder b;
    BuildGte3DKernel(b);
    b.WriteFile(synth_bios);
    double base_fps = 0.0;
    for (const auto& c : cpu_cfgs) {
      if (skip_interp && c.cpu_mode == "Interpreter") continue;
      auto st = RunBenchmark(api, sys_dir, "bench_bios.bin", "", c, 15, 60, false);
      if (base_fps == 0.0 && st.ok) base_fps = st.fps;
      PrintRow("synth_gte_3d_pgxp", c, 60, st, base_fps);
    }
  }

  // 3. Real BIOS + Real PS1 Games
  if (!bios_path.empty() && CopyFile(bios_path, synth_bios)) {
    const std::vector<BenchConfig> bios_cfgs = {
      {"Cached_SW",         "CachedInterpreter", "Disabled", "Software", false, 1, false, false, false},
      {"JIT_MMap_SW",       "Recompiler",        "MMap",     "Software", false, 1, false, false, false},
      {"JIT_MMap_GL_1x",    "Recompiler",        "MMap",     "OpenGL",   false, 1, false, false, false},
      {"JIT_MMap_VK_1x",    "Recompiler",        "MMap",     "Vulkan",   false, 1, false, false, false},
      {"JIT_PGXP_CPU_VK_2x","Recompiler",        "MMap",     "Vulkan",   false, 2, true,  true,  false},
    };
    double base_fps = 0.0;
    for (const auto& c : bios_cfgs) {
      auto st = RunBenchmark(api, sys_dir, "bench_bios.bin", "", c, 30, 150, !dump_dir.empty());
      if (base_fps == 0.0 && st.ok) base_fps = st.fps;
      PrintRow("real_bios_boot", c, 150, st, base_fps);
      if (!dump_dir.empty() && st.ok && !st.last_frame.rgb.empty() &&
          std::strcmp(c.label, "JIT_PGXP_CPU_VK_2x") == 0) {
        WriteRgbPng(dump_dir + "/real_bios_boot_vk_2x_pgxp.png", st.last_frame.width, st.last_frame.height,
                    st.last_frame.rgb.data());
      }
    }

    const std::vector<BenchConfig> game_cfgs = {
      {"JIT_MMap_SW_1x",     "Recompiler", "MMap", "Software", false, 1, false, false, false},
      {"JIT_MMap_SW_Thr_1x", "Recompiler", "MMap", "Software", true,  1, false, false, false},
      {"JIT_MMap_GL_1x",     "Recompiler", "MMap", "OpenGL",   false, 1, false, false, false},
      {"JIT_MMap_GL_2x",     "Recompiler", "MMap", "OpenGL",   false, 2, false, false, false},
      {"JIT_PGXP_M_GL_2x",   "Recompiler", "MMap", "OpenGL",   false, 2, true,  false, false},
      {"JIT_PGXP_C_GL_2x",   "Recompiler", "MMap", "OpenGL",   false, 2, true,  true,  false},
      {"JIT_MMap_VK_1x",     "Recompiler", "MMap", "Vulkan",   false, 1, false, false, false},
      {"JIT_MMap_VK_2x",     "Recompiler", "MMap", "Vulkan",   false, 2, false, false, false},
      {"JIT_PGXP_C_VK_2x",   "Recompiler", "MMap", "Vulkan",   false, 2, true,  true,  false},
    };
    for (const auto& g : games) {
      double g_base = 0.0;
      for (auto c : game_cfgs) {
        c.fast_boot = true;
        auto st = RunBenchmark(api, sys_dir, "bench_bios.bin", g.path, c, 560, game_frames, !dump_dir.empty());
        if (g_base == 0.0 && st.ok) g_base = st.fps;
        PrintRow(g.label.c_str(), c, game_frames, st, g_base);
        if (!dump_dir.empty() && st.ok && !st.last_frame.rgb.empty() &&
            (std::strcmp(c.label, "JIT_MMap_SW_1x") == 0 ||
             std::strcmp(c.label, "JIT_PGXP_C_GL_2x") == 0 ||
             std::strcmp(c.label, "JIT_PGXP_C_VK_2x") == 0)) {
          WriteRgbPng(dump_dir + "/" + g.label + "_" + c.label + ".png",
                      st.last_frame.width, st.last_frame.height, st.last_frame.rgb.data());
        }
      }
    }
  }

  std::remove(synth_bios.c_str());
  rmdir(tmp_dir);
  return 0;
}

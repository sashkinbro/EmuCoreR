// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary

#pragma once

#include <android/native_window.h>
#include <cstdint>

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#include "libretro.h"
#include "libretro_vulkan.h"

namespace emucorer::vulkan {

struct PresentRect {
    int x;
    int y;
    int width;
    int height;
};

// Per-frame timing reported by the Vulkan presenter. All values are for the
// most recent frame the frame worker completed.
struct PresentStats {
    // Wall time the presenter spent on the CPU for the frame (recording,
    // submission and any waiting included).
    uint64_t present_cpu_nanos;
    // Wall time blocked waiting for the previous frame's GPU work at the start
    // of the frame.
    uint64_t fence_wait_nanos;
    // Wall time the emulation thread blocked inside the core's frame sync,
    // waiting for the frontend to release the display texture.
    uint64_t core_sync_wait_nanos;
    // GPU execution time of the present pass, from device timestamps.
    uint64_t present_gpu_nanos;
    bool gpu_time_valid;
};

PresentStats GetLastPresentStats();
void RecordCoreSyncWait(uint64_t nanos);

bool Prepare(ANativeWindow* window, uint32_t window_generation);

bool AcceptHardwareRender(retro_hw_render_callback* callback);
bool AcceptNegotiationInterface(const retro_hw_render_context_negotiation_interface* negotiation);
bool FillHardwareRenderInterface(retro_hw_render_interface** out_interface);

bool IsRequested();
bool IsActive();

void NotifyContextDestroy();
void SetShaderEffect(int effect);
void SetDisplayCrop(int left, int top, int right, int bottom);

bool EnsureContext(ANativeWindow* window, uint32_t window_generation);
bool Present(uint32_t source_width, uint32_t source_height, double display_aspect, bool stretch);

void Destroy();

}  // namespace emucorer::vulkan

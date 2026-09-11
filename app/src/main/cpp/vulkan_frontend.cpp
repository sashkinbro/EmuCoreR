// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary

#include "vulkan_frontend.h"

#include <android/log.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#define VK_LOG_TAG "EmuCoreR-Vk"
#define VK_LOGI(...) __android_log_print(ANDROID_LOG_INFO, VK_LOG_TAG, __VA_ARGS__)
#define VK_LOGW(...) __android_log_print(ANDROID_LOG_WARN, VK_LOG_TAG, __VA_ARGS__)
#define VK_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VK_LOG_TAG, __VA_ARGS__)

namespace emucorer::vulkan {
namespace {

constexpr uint32_t kPreferredQueueFamilyNone = UINT32_MAX;
constexpr uint64_t kFenceWaitTimeoutNs = 2'000'000'000ull;
constexpr int kMaxSwapchainFailures = 3;

struct State {
    bool requested = false;
    bool failed = false;
    bool active = false;
    bool context_reset_pending = false;

    retro_hw_render_callback* callback = nullptr;
    const retro_hw_render_context_negotiation_interface_vulkan* negotiation = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t queue_family = kPreferredQueueFamilyNone;

    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    uint32_t present_queue_family = kPreferredQueueFamilyNone;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent{};
    VkSurfaceTransformFlagBitsKHR swapchain_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> swapchain_images;
    std::vector<bool> swapchain_initialized;
    int swapchain_failures = 0;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    VkSemaphore present_semaphore = VK_NULL_HANDLE;
    VkFence frame_fence = VK_NULL_HANDLE;

    ANativeWindow* preparation_window = nullptr;
    uint32_t window_generation = 0;
    uint32_t preparation_generation = 0;
    int window_width = 0;
    int window_height = 0;

    PFN_vkCreateAndroidSurfaceKHR pfn_create_android_surface = nullptr;
    PFN_vkDestroySurfaceKHR pfn_destroy_surface = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR pfn_surface_support = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pfn_surface_caps = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR pfn_surface_formats = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pfn_surface_present_modes = nullptr;
    PFN_vkCreateSwapchainKHR pfn_create_swapchain = nullptr;
    PFN_vkDestroySwapchainKHR pfn_destroy_swapchain = nullptr;
    PFN_vkAcquireNextImageKHR pfn_acquire = nullptr;
    PFN_vkQueuePresentKHR pfn_queue_present = nullptr;

    retro_hw_render_interface_vulkan interface = {};
    retro_vulkan_image frame_image = {};
    bool has_frame_image = false;
    uint32_t sync_index = 0;
};

State g_vk;
std::mutex g_queue_mutex;

template <typename T>
T LoadInstanceFunction(const char* name) {
    return reinterpret_cast<T>(vkGetInstanceProcAddr(g_vk.instance, name));
}

template <typename T>
T LoadDeviceFunction(const char* name) {
    return reinterpret_cast<T>(vkGetDeviceProcAddr(g_vk.device, name));
}

void ResetState() {
    g_vk = State{};
}

bool HasDeviceExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

bool HasInstanceExtension(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

bool EnsureInstance() {
    if (g_vk.instance != VK_NULL_HANDLE) return true;

    if (!HasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME) ||
        !HasInstanceExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        VK_LOGE("Vulkan instance surface extensions are unavailable");
        return false;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "EmuCoreR";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "EmuCoreR";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    const retro_hw_render_context_negotiation_interface_vulkan* negotiation = g_vk.negotiation;
    if (negotiation != nullptr && negotiation->get_application_info != nullptr) {
        const VkApplicationInfo* requested = negotiation->get_application_info();
        if (requested != nullptr) app_info = *requested;
        if (app_info.apiVersion < VK_API_VERSION_1_0) app_info.apiVersion = VK_API_VERSION_1_0;
    }

    const char* enabled_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = 2;
    create_info.ppEnabledExtensionNames = enabled_extensions;

    const VkResult result = vkCreateInstance(&create_info, nullptr, &g_vk.instance);
    if (result != VK_SUCCESS) {
        VK_LOGE("vkCreateInstance failed (0x%x)", result);
        return false;
    }

    g_vk.pfn_create_android_surface =
        LoadInstanceFunction<PFN_vkCreateAndroidSurfaceKHR>("vkCreateAndroidSurfaceKHR");
    g_vk.pfn_destroy_surface = LoadInstanceFunction<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR");
    g_vk.pfn_surface_support =
        LoadInstanceFunction<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR");
    g_vk.pfn_surface_caps =
        LoadInstanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    g_vk.pfn_surface_formats =
        LoadInstanceFunction<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
    g_vk.pfn_surface_present_modes =
        LoadInstanceFunction<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (g_vk.pfn_create_android_surface == nullptr || g_vk.pfn_destroy_surface == nullptr ||
        g_vk.pfn_surface_support == nullptr || g_vk.pfn_surface_caps == nullptr ||
        g_vk.pfn_surface_formats == nullptr || g_vk.pfn_surface_present_modes == nullptr) {
        VK_LOGE("Vulkan surface entry points are missing");
        return false;
    }
    VK_LOGI("Vulkan instance created");
    return true;
}

bool EnsureSurface(ANativeWindow* window, uint32_t window_generation) {
    if (window == nullptr) return false;
    if (g_vk.surface != VK_NULL_HANDLE) {
        if (g_vk.preparation_window == window && g_vk.preparation_generation == window_generation) return true;
        if (g_vk.device != VK_NULL_HANDLE && g_vk.pfn_destroy_swapchain != nullptr &&
            g_vk.swapchain != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_vk.device);
            g_vk.pfn_destroy_swapchain(g_vk.device, g_vk.swapchain, nullptr);
            g_vk.swapchain = VK_NULL_HANDLE;
            g_vk.swapchain_images.clear();
            g_vk.swapchain_initialized.clear();
        }
        g_vk.pfn_destroy_surface(g_vk.instance, g_vk.surface, nullptr);
        g_vk.surface = VK_NULL_HANDLE;
    }

    VkAndroidSurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    create_info.window = window;
    const VkResult result = g_vk.pfn_create_android_surface(g_vk.instance, &create_info, nullptr, &g_vk.surface);
    if (result != VK_SUCCESS) {
        VK_LOGE("vkCreateAndroidSurfaceKHR failed (0x%x)", result);
        return false;
    }

    g_vk.preparation_window = window;
    g_vk.preparation_generation = window_generation;
    g_vk.window_width = ANativeWindow_getWidth(window);
    g_vk.window_height = ANativeWindow_getHeight(window);
    VK_LOGI("Vulkan surface created (%dx%d)", g_vk.window_width, g_vk.window_height);
    return true;
}

bool PickPhysicalDevice() {
    if (g_vk.physical_device != VK_NULL_HANDLE) return true;

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(g_vk.instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        VK_LOGE("No Vulkan physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(g_vk.instance, &count, devices.data()) != VK_SUCCESS) {
        VK_LOGE("vkEnumeratePhysicalDevices failed");
        return false;
    }

    for (VkPhysicalDevice device : devices) {
        if (!HasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
        if (queue_count == 0) continue;
        std::vector<VkQueueFamilyProperties> queue_properties(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queue_properties.data());

        for (uint32_t i = 0; i < queue_count; ++i) {
            if ((queue_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
            VkBool32 present_supported = VK_FALSE;
            if (g_vk.pfn_surface_support(device, i, g_vk.surface, &present_supported) != VK_SUCCESS) continue;
            if (present_supported != VK_TRUE) continue;

            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            g_vk.physical_device = device;
            g_vk.queue_family = i;
            VK_LOGI("Vulkan physical device: %s (API %u.%u, queue family %u)", properties.deviceName,
                    VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), i);
            return true;
        }
    }

    VK_LOGE("No Vulkan device with graphics+present queue and swapchain support");
    return false;
}

bool CreateDevice() {
    if (g_vk.device != VK_NULL_HANDLE) return true;
    if (g_vk.negotiation == nullptr || g_vk.negotiation->create_device == nullptr) {
        VK_LOGE("Core did not provide a Vulkan context negotiation interface");
        return false;
    }

    retro_vulkan_context context{};
    const char* required_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const bool created =
        g_vk.negotiation->create_device(&context, g_vk.instance, g_vk.physical_device, g_vk.surface,
                                        &vkGetInstanceProcAddr, required_extensions, 1, nullptr, 0, nullptr);
    if (!created || context.device == VK_NULL_HANDLE) {
        VK_LOGE("Core failed to create the Vulkan device");
        return false;
    }

    g_vk.device = context.device;
    g_vk.queue = context.queue;
    g_vk.queue_family = context.queue_family_index;
    g_vk.present_queue = context.presentation_queue != VK_NULL_HANDLE ? context.presentation_queue : context.queue;
    g_vk.present_queue_family = context.presentation_queue != VK_NULL_HANDLE
                                    ? context.presentation_queue_family_index
                                    : context.queue_family_index;
    if (g_vk.present_queue_family != g_vk.queue_family) {
        VK_LOGE("Vulkan present queue family %u differs from graphics family %u", g_vk.present_queue_family,
                g_vk.queue_family);
        return false;
    }

    g_vk.pfn_create_swapchain = LoadDeviceFunction<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
    g_vk.pfn_destroy_swapchain = LoadDeviceFunction<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
    g_vk.pfn_acquire = LoadDeviceFunction<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
    g_vk.pfn_queue_present = LoadDeviceFunction<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
    if (g_vk.pfn_create_swapchain == nullptr || g_vk.pfn_destroy_swapchain == nullptr ||
        g_vk.pfn_acquire == nullptr || g_vk.pfn_queue_present == nullptr) {
        VK_LOGE("Vulkan swapchain entry points are missing");
        return false;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = g_vk.queue_family;
    if (vkCreateCommandPool(g_vk.device, &pool_info, nullptr, &g_vk.command_pool) != VK_SUCCESS) {
        VK_LOGE("vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    buffer_info.commandPool = g_vk.command_pool;
    buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_vk.device, &buffer_info, &g_vk.command_buffer) != VK_SUCCESS) {
        VK_LOGE("vkAllocateCommandBuffers failed");
        return false;
    }

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(g_vk.device, &semaphore_info, nullptr, &g_vk.acquire_semaphore) != VK_SUCCESS ||
        vkCreateSemaphore(g_vk.device, &semaphore_info, nullptr, &g_vk.present_semaphore) != VK_SUCCESS) {
        VK_LOGE("vkCreateSemaphore failed");
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(g_vk.device, &fence_info, nullptr, &g_vk.frame_fence) != VK_SUCCESS) {
        VK_LOGE("vkCreateFence failed");
        return false;
    }

    VK_LOGI("Vulkan render device ready");
    return true;
}

VkExtent2D ChooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != UINT32_MAX) return capabilities.currentExtent;

    VkExtent2D extent{};
    extent.width = static_cast<uint32_t>(
        g_vk.window_width > 0 ? g_vk.window_width : static_cast<int>(capabilities.minImageExtent.width));
    extent.height = static_cast<uint32_t>(
        g_vk.window_height > 0 ? g_vk.window_height : static_cast<int>(capabilities.minImageExtent.height));
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

VkSurfaceFormatKHR ChooseSwapchainFormat(uint32_t count, const VkSurfaceFormatKHR* formats) {
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) return formats[i];
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) return formats[i];
    }
    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        return VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, formats[0].colorSpace};
    }
    return formats[0];
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    const VkCompositeAlphaFlagBitsKHR preferred[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR candidate : preferred) {
        if ((supported & candidate) != 0) return candidate;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

void DestroySwapchain() {
    if (g_vk.device == VK_NULL_HANDLE || g_vk.swapchain == VK_NULL_HANDLE) return;
    g_vk.pfn_destroy_swapchain(g_vk.device, g_vk.swapchain, nullptr);
    g_vk.swapchain = VK_NULL_HANDLE;
    g_vk.swapchain_images.clear();
    g_vk.swapchain_initialized.clear();
}

bool CreateSwapchain() {
    if (g_vk.device == VK_NULL_HANDLE || g_vk.surface == VK_NULL_HANDLE) return false;
    if (g_vk.swapchain != VK_NULL_HANDLE) return true;

    VkSurfaceCapabilitiesKHR capabilities{};
    if (g_vk.pfn_surface_caps(g_vk.physical_device, g_vk.surface, &capabilities) != VK_SUCCESS) {
        VK_LOGE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
        return false;
    }

    uint32_t format_count = 0;
    if (g_vk.pfn_surface_formats(g_vk.physical_device, g_vk.surface, &format_count, nullptr) != VK_SUCCESS ||
        format_count == 0) {
        VK_LOGE("No Vulkan surface formats");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    g_vk.pfn_surface_formats(g_vk.physical_device, g_vk.surface, &format_count, formats.data());
    const VkSurfaceFormatKHR format = ChooseSwapchainFormat(format_count, formats.data());

    uint32_t mode_count = 0;
    if (g_vk.pfn_surface_present_modes(g_vk.physical_device, g_vk.surface, &mode_count, nullptr) != VK_SUCCESS ||
        mode_count == 0) {
        VK_LOGE("No Vulkan surface present modes");
        return false;
    }
    std::vector<VkPresentModeKHR> modes(mode_count);
    g_vk.pfn_surface_present_modes(g_vk.physical_device, g_vk.surface, &mode_count, modes.data());
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_FIFO_KHR) {
            present_mode = mode;
            break;
        }
    }

    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        VK_LOGE("Swapchain images do not support transfer destinations");
        return false;
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkExtent2D extent = ChooseSwapchainExtent(capabilities);
    if (extent.width == 0 || extent.height == 0) {
        VK_LOGE("Swapchain extent is empty");
        return false;
    }

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = g_vk.surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = format.format;
    create_info.imageColorSpace = format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;

    const VkResult result = g_vk.pfn_create_swapchain(g_vk.device, &create_info, nullptr, &g_vk.swapchain);
    if (result != VK_SUCCESS) {
        VK_LOGE("vkCreateSwapchainKHR failed (0x%x)", result);
        return false;
    }

    uint32_t actual_count = 0;
    if (vkGetSwapchainImagesKHR(g_vk.device, g_vk.swapchain, &actual_count, nullptr) != VK_SUCCESS ||
        actual_count == 0) {
        VK_LOGE("vkGetSwapchainImagesKHR failed");
        DestroySwapchain();
        return false;
    }
    g_vk.swapchain_images.resize(actual_count);
    vkGetSwapchainImagesKHR(g_vk.device, g_vk.swapchain, &actual_count, g_vk.swapchain_images.data());
    g_vk.swapchain_initialized.assign(actual_count, false);
    g_vk.swapchain_format = format.format;
    g_vk.swapchain_extent = extent;
    g_vk.swapchain_transform = capabilities.currentTransform;
    g_vk.swapchain_failures = 0;
    VK_LOGI("Vulkan swapchain %ux%u, %u images, format %u, transform %u", extent.width, extent.height, actual_count,
            static_cast<unsigned>(format.format), static_cast<unsigned>(capabilities.currentTransform));
    return true;
}

bool RecreateSwapchain() {
    if (g_vk.device == VK_NULL_HANDLE) return false;
    vkDeviceWaitIdle(g_vk.device);
    DestroySwapchain();
    if (CreateSwapchain()) return true;

    ++g_vk.swapchain_failures;
    if (g_vk.swapchain_failures >= kMaxSwapchainFailures) {
        VK_LOGE("Giving up on Vulkan swapchain after %d failures", g_vk.swapchain_failures);
        g_vk.failed = true;
    }
    return false;
}

PresentRect FitDisplayRect(const VkExtent2D& extent, double display_aspect, bool stretch) {
    PresentRect rect{0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height)};
    if (stretch) return rect;
    if (display_aspect <= 0.0 || !std::isfinite(display_aspect)) display_aspect = 4.0 / 3.0;

    const double window_aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    int width;
    int height;
    if (window_aspect > display_aspect) {
        height = static_cast<int>(extent.height);
        width = static_cast<int>(height * display_aspect + 0.5);
    } else {
        width = static_cast<int>(extent.width);
        height = static_cast<int>(width / display_aspect + 0.5);
    }
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    rect.width = width;
    rect.height = height;
    rect.x = (static_cast<int>(extent.width) - width) / 2;
    rect.y = (static_cast<int>(extent.height) - height) / 2;
    return rect;
}

void SetImage(void*, const retro_vulkan_image* image, uint32_t num_semaphores, const VkSemaphore* semaphores,
              uint32_t src_queue_family) {
    (void)num_semaphores;
    (void)semaphores;
    (void)src_queue_family;
    if (image == nullptr || image->image_view == VK_NULL_HANDLE) {
        g_vk.has_frame_image = false;
        return;
    }
    g_vk.frame_image = *image;
    g_vk.has_frame_image = true;
}

uint32_t GetSyncIndex(void*) {
    return g_vk.sync_index;
}

uint32_t GetSyncIndexMask(void*) {
    const uint32_t count = static_cast<uint32_t>(g_vk.swapchain_images.size());
    return count > 0 ? ((1u << count) - 1u) : 1u;
}

void SetCommandBuffers(void*, uint32_t, const VkCommandBuffer*) {}

void WaitSyncIndex(void*) {
    if (g_vk.device == VK_NULL_HANDLE || g_vk.frame_fence == VK_NULL_HANDLE) return;
    vkWaitForFences(g_vk.device, 1, &g_vk.frame_fence, VK_TRUE, kFenceWaitTimeoutNs);
}

void LockQueue(void*) {
    g_queue_mutex.lock();
}

void UnlockQueue(void*) {
    g_queue_mutex.unlock();
}

void SetSignalSemaphore(void*, VkSemaphore) {}

void FillInterface() {
    g_vk.interface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    g_vk.interface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    g_vk.interface.handle = &g_vk;
    g_vk.interface.instance = g_vk.instance;
    g_vk.interface.gpu = g_vk.physical_device;
    g_vk.interface.device = g_vk.device;
    g_vk.interface.get_device_proc_addr = &vkGetDeviceProcAddr;
    g_vk.interface.get_instance_proc_addr = &vkGetInstanceProcAddr;
    g_vk.interface.queue = g_vk.queue;
    g_vk.interface.queue_index = g_vk.queue_family;
    g_vk.interface.set_image = &SetImage;
    g_vk.interface.get_sync_index = &GetSyncIndex;
    g_vk.interface.get_sync_index_mask = &GetSyncIndexMask;
    g_vk.interface.set_command_buffers = &SetCommandBuffers;
    g_vk.interface.wait_sync_index = &WaitSyncIndex;
    g_vk.interface.lock_queue = &LockQueue;
    g_vk.interface.unlock_queue = &UnlockQueue;
    g_vk.interface.set_signal_semaphore = &SetSignalSemaphore;
}

bool RecordPresent(uint32_t swapchain_index, uint32_t source_width, uint32_t source_height, const PresentRect& dst) {
    VkCommandBuffer command_buffer = g_vk.command_buffer;
    vkResetCommandBuffer(command_buffer, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) return false;

    const VkImage source_image = g_vk.frame_image.create_info.image;
    const VkImageLayout source_layout = g_vk.frame_image.image_layout;
    const VkImage target_image = g_vk.swapchain_images[swapchain_index];

    VkImageMemoryBarrier source_barrier{};
    source_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    source_barrier.oldLayout = source_layout;
    source_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.image = source_image;
    source_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    source_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    source_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &source_barrier);

    VkImageMemoryBarrier target_barrier{};
    target_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    target_barrier.oldLayout =
        g_vk.swapchain_initialized[swapchain_index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    target_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    target_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    target_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    target_barrier.image = target_image;
    target_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    target_barrier.srcAccessMask = 0;
    target_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &target_barrier);

    VkClearColorValue clear{};
    const VkImageSubresourceRange clear_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command_buffer, target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                         &clear_range);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(source_width), static_cast<int32_t>(source_height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {dst.x, dst.y, 0};
    blit.dstOffsets[1] = {dst.x + dst.width, dst.y + dst.height, 1};
    vkCmdBlitImage(command_buffer, source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    source_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source_barrier.newLayout = source_layout;
    source_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &source_barrier);

    target_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    target_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    target_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    target_barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &target_barrier);

    return vkEndCommandBuffer(command_buffer) == VK_SUCCESS;
}

}  // namespace

bool Prepare(ANativeWindow* window, uint32_t window_generation) {
    if (g_vk.requested || g_vk.active) return true;
    if (window == nullptr) return false;

    if (!EnsureInstance() || !EnsureSurface(window, window_generation) || !PickPhysicalDevice()) {
        Destroy();
        return false;
    }
    g_vk.window_generation = window_generation;
    return true;
}

bool AcceptHardwareRender(retro_hw_render_callback* callback) {
    if (callback == nullptr) return false;
    g_vk.requested = true;
    g_vk.callback = callback;
    g_vk.context_reset_pending = true;
    if (!g_vk.active) g_vk.failed = false;
    VK_LOGI("Accepted Vulkan hardware renderer request");
    return true;
}

bool AcceptNegotiationInterface(const retro_hw_render_context_negotiation_interface* negotiation) {
    if (negotiation == nullptr ||
        negotiation->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) {
        return false;
    }
    g_vk.negotiation = reinterpret_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(negotiation);
    return true;
}

bool FillHardwareRenderInterface(retro_hw_render_interface** out_interface) {
    if (out_interface == nullptr || !g_vk.requested || g_vk.device == VK_NULL_HANDLE) return false;
    FillInterface();
    *out_interface = reinterpret_cast<retro_hw_render_interface*>(&g_vk.interface);
    return true;
}

bool IsRequested() {
    return g_vk.requested;
}

bool IsActive() {
    return g_vk.active && !g_vk.failed;
}

void NotifyContextDestroy() {
    if (g_vk.callback != nullptr && g_vk.callback->context_destroy != nullptr) {
        g_vk.callback->context_destroy();
    }
}

bool EnsureContext(ANativeWindow* window, uint32_t window_generation) {
    if (!g_vk.requested) return false;
    if (window == nullptr) return false;
    if (g_vk.failed) {
        if (window_generation == g_vk.window_generation) return false;
        g_vk.failed = false;
        g_vk.swapchain_failures = 0;
    }

    const bool window_changed =
        window_generation != g_vk.preparation_generation || g_vk.preparation_window != window;
    if (g_vk.active && !window_changed && g_vk.swapchain != VK_NULL_HANDLE &&
        !g_vk.context_reset_pending) {
        return true;
    }

    const bool was_active = g_vk.active;
    if (!EnsureInstance() || !EnsureSurface(window, window_generation) || !PickPhysicalDevice() ||
        !CreateDevice()) {
        g_vk.failed = true;
        return false;
    }

    if (window_changed || g_vk.swapchain == VK_NULL_HANDLE) {
        if (!RecreateSwapchain()) return false;
    }

    g_vk.window_generation = window_generation;
    g_vk.active = true;
    g_vk.has_frame_image = false;
    const bool need_reset = !was_active || g_vk.context_reset_pending;
    g_vk.context_reset_pending = false;
    if (need_reset) {
        if (g_vk.callback != nullptr && g_vk.callback->context_reset != nullptr) {
            g_vk.callback->context_reset();
        }
        VK_LOGI("Vulkan hardware context reset complete");
    } else if (window_changed) {
        VK_LOGI("Vulkan presentation surface rebound; core display kept");
    }
    return true;
}

bool Present(uint32_t source_width, uint32_t source_height, double display_aspect, bool stretch) {
    if (!IsActive() || g_vk.device == VK_NULL_HANDLE || g_vk.swapchain == VK_NULL_HANDLE || !g_vk.has_frame_image) {
        return false;
    }
    if (source_width == 0 || source_height == 0) return false;

    if (vkWaitForFences(g_vk.device, 1, &g_vk.frame_fence, VK_TRUE, kFenceWaitTimeoutNs) != VK_SUCCESS) {
        VK_LOGW("Vulkan frame fence timed out");
        return false;
    }
    vkResetFences(g_vk.device, 1, &g_vk.frame_fence);

    uint32_t swapchain_index = 0;
    VkResult result = g_vk.pfn_acquire(g_vk.device, g_vk.swapchain, UINT64_MAX, g_vk.acquire_semaphore,
                                       VK_NULL_HANDLE, &swapchain_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return false;
    }
    const bool recreate_after_present = (result == VK_SUBOPTIMAL_KHR);
    if (result != VK_SUCCESS && !recreate_after_present) {
        VK_LOGW("vkAcquireNextImageKHR failed (0x%x)", result);
        return false;
    }
    if (swapchain_index >= g_vk.swapchain_images.size()) return false;

    const PresentRect dst = FitDisplayRect(g_vk.swapchain_extent, display_aspect, stretch);
    if (!RecordPresent(swapchain_index, source_width, source_height, dst)) {
        VK_LOGW("Failed to record Vulkan present commands");
        return false;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &g_vk.acquire_semaphore;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &g_vk.command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &g_vk.present_semaphore;
    if (vkQueueSubmit(g_vk.queue, 1, &submit_info, g_vk.frame_fence) != VK_SUCCESS) {
        VK_LOGW("vkQueueSubmit failed");
        return false;
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &g_vk.present_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &g_vk.swapchain;
    present_info.pImageIndices = &swapchain_index;
    result = g_vk.pfn_queue_present(g_vk.present_queue, &present_info);
    g_vk.swapchain_initialized[swapchain_index] = true;
    g_vk.sync_index = swapchain_index;

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || recreate_after_present) {
        RecreateSwapchain();
    } else if (result != VK_SUCCESS) {
        VK_LOGW("vkQueuePresentKHR failed (0x%x)", result);
        return false;
    }
    return true;
}

void Destroy() {
    if (g_vk.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_vk.device);
        DestroySwapchain();
        if (g_vk.command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(g_vk.device, g_vk.command_pool, nullptr);
        if (g_vk.acquire_semaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(g_vk.device, g_vk.acquire_semaphore, nullptr);
        if (g_vk.present_semaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(g_vk.device, g_vk.present_semaphore, nullptr);
        if (g_vk.frame_fence != VK_NULL_HANDLE) vkDestroyFence(g_vk.device, g_vk.frame_fence, nullptr);
        vkDestroyDevice(g_vk.device, nullptr);
        VK_LOGI("Vulkan render device destroyed");
    }
    if (g_vk.surface != VK_NULL_HANDLE && g_vk.instance != VK_NULL_HANDLE && g_vk.pfn_destroy_surface != nullptr) {
        g_vk.pfn_destroy_surface(g_vk.instance, g_vk.surface, nullptr);
    }
    if (g_vk.instance != VK_NULL_HANDLE) vkDestroyInstance(g_vk.instance, nullptr);
    ResetState();
}

}  // namespace emucorer::vulkan

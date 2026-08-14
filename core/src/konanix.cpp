//  NO_AI POLICY
//  
//  This file is subject to the repository's strict NO_AI policy.
//  See: NO_AI.md
//  
//  AI/ML systems of any kind are prohibited from being used to:
//  
//      - generate, modify, review, analyze, or transform this file;
//      - index, embed, ingest, train on, or otherwise process its contents;
//      - provide automated assistance or contributions to this repository.
//  
//  This prohibition applies to generative AI, machine learning,
//  language models, embeddings, AI coding assistants, AI indexing,
//  automated agents, and substantially similar technologies.
//  
//  
//  No exceptions are implied. Any exception requires prior written
//  authorization from the repository owner.
//  
//  
//  Copyright © 2026 konacode. All rights reserved.

#include "core.h"
#include "konanix.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>
#include <thread>
#include <vulkan/vulkan_core.h>
#include <GLFW/glfw3.h>

#include "./util/logger.h"

// global variables
#include "globals/allocator.h"
#include "globals/instance.h"
#include "globals/window.h"
#include "globals/device.h"
#include "globals/commands.h"
#include "globals/descriptors.h"
#include "globals/swapchain.h"
#include "globals/pipeline.h"
#include "globals/sync.h"
#include "globals/time.h"

#include "./shaders/frag.c"
#include "./shaders/vert.c"

using namespace konanix;

static constexpr short                  version[3]                      = {1, 0, 0};
static uint32_t                         current_frame                   = 1;
static uint32_t                         current_gif_frame               = 0;

static bool                             DEBUG                           = false;
static bool                             RESIZABLE                       = false;
static uint32_t                         GIF_FRAME_COUNT                 = 0;

static const char**                     exts;
static uint32_t                         n_exts                          = 0;

static VkSampleCountFlagBits            g_msaa_samples                  = VK_SAMPLE_COUNT_1_BIT;

struct gif_frame {
    VkImage                             image                           = nullptr;
    VkDeviceMemory                      image_memory                    = nullptr;
    VkImageView                         image_view                      = nullptr;
};

VkSampler                               g_gif_sampler                   = nullptr;

static gif_frame                        active_frame                    = {};
static std::vector<gif_frame>           gif_frames;

#ifdef KONANIX_BUILD_WITH_VALIDATION
static VkDebugUtilsMessengerEXT         g_debug_messenger               = nullptr;

constexpr static VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
    switch (messageSeverity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            logger::log("<vulkan validation> "+std::string(pCallbackData->pMessage),logger::dbg);
            return VK_FALSE;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            logger::log("<vulkan validation> "+std::string(pCallbackData->pMessage),logger::inf);
            return VK_FALSE;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            logger::log("<vulkan validation> "+std::string(pCallbackData->pMessage),logger::wrn);
            return VK_FALSE;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            logger::log("<vulkan validation> "+std::string(pCallbackData->pMessage),logger::err);
            return VK_FALSE;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT:
            logger::log("<vulkan validation> "+std::string(pCallbackData->pMessage),logger::exc);
            return VK_FALSE;
        default:
            logger::log("<vulkan validation> "+std::string(pCallbackData->pMessage),logger::dbg);
            return VK_FALSE;
    }
    
}
#endif

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
};

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    std::optional<uint32_t> transferFamily;
    bool complete() const {
        return (graphicsFamily.has_value()&&presentFamily.has_value());
    }
};

// ---------------------------------------------------------------------------
// drawing functions
// ---------------------------------------------------------------------------

void konanix::overlay::set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= globals::width || y >= globals::height) return;
    size_t i = (size_t(y) * size_t(globals::width) + size_t(x)) * 4;
    float sa = a / 255.0f;
    float da = rgba[i + 3] / 255.0f;
    float outA = sa + da * (1.0f - sa);
    if (outA <= 0.0f) {
        rgba[i + 0] = rgba[i + 1] = rgba[i + 2] = rgba[i + 3] = 0;
        return;
    }
    auto blend = [&](uint8_t src, uint8_t dst) -> uint8_t {
        float s = src / 255.0f;
        float d = dst / 255.0f;
        float out = (s * sa + d * da * (1.0f - sa)) / outA;
        int v = int(out * 255.0f + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint8_t)v;
    };
    rgba[i + 0] = blend(r, rgba[i + 0]);
    rgba[i + 1] = blend(g, rgba[i + 1]);
    rgba[i + 2] = blend(b, rgba[i + 2]);
    rgba[i + 3] = (uint8_t)(outA * 255.0f + 0.5f);
}

void konanix::overlay::rect(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int yy = 0; yy < rh; ++yy) {
        for (int xx = 0; xx < rw; ++xx) {
            set_pixel(x + xx, y + yy, r, g, b, a);
        }
    }
}

void konanix::overlay::stroke_rect(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rect(x, y, rw, 1, r, g, b, a);
    rect(x, y + rh - 1, rw, 1, r, g, b, a);
    rect(x, y, 1, rh, r, g, b, a);
    rect(x + rw - 1, y, 1, rh, r, g, b, a);
}

// font map
std::array<uint8_t, 7> glyph(char c) {
    switch (c) {
        case ' ': return {0,0,0,0,0,0,0};
        case 'C': return {0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110};
        case 'E': return {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111};
        case 'I': return {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b11111};
        case 'L': return {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111};
        case 'N': return {0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001};
        case 'O': return {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110};
        case 'P': return {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000};
        case 'S': return {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110};
        case 'T': return {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100};
        case 'X': return {0b10001,0b01010,0b00100,0b00100,0b00100,0b01010,0b10001};
        default:  return {0b11111,0b10001,0b00010,0b00100,0b00100,0b00000,0b00100}; // ?
    }
}

void konanix::overlay::draw_char(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale) {
    c = (char)std::toupper((unsigned char)c);
    auto g7 = glyph(c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (g7[row] & (1 << (4 - col))) {
                rect(x + col * scale, y + row * scale, scale, scale, r, g, b, a);
            }
        }
    }
}
void konanix::overlay::draw_text(int x, int y, const std::string& s, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale) {
    int cx = x;
    for (char c : s) {
        draw_char(cx, y, c, r, g, b, a, scale);
        cx += 6 * scale; // 5 pixels + 1 pixel spacing
    }
}

// ---------------------------------------------------------------------------
// context menu functions, callbacks and variables
// ---------------------------------------------------------------------------

struct DragState {
    bool dragging = false;
    double press_cursor_x = 0.0;
    double press_cursor_y = 0.0;
    int press_window_x = 0;
    int press_window_y = 0;
};

struct MenuItem {
    std::string label;
    std::function<void()> action;
};

struct ContextMenu {
    bool visible = false;
    double x = 0.0;
    double y = 0.0;
    int hovered = -1;
    std::vector<MenuItem> items;
};

static constexpr double kItemH = 24.0;
static constexpr double kMenuW  = 180.0;
static constexpr double kPad     = 6.0;

static ContextMenu g_menu;
static DragState g_drag;

void konanix::draw_context_menu() {
    if (!g_menu.visible) return;

    int x = (int)g_menu.x;
    int y = (int)g_menu.y;
    int h = kItemH * (int)g_menu.items.size();

    overlay::rect(x, y,kMenuW, h, 28, 28, 28, 230);
    overlay::stroke_rect(x, y, kMenuW, h, 90, 90, 90, 255);

    for (int i = 0; i < (int)g_menu.items.size(); ++i) {
        int iy = y + i * kItemH;
        if (i == g_menu.hovered) {
            overlay::rect(x + 1, iy + 1, kMenuW - 2, kItemH - 2, 70, 70, 70, 255);
        }

        overlay::draw_text(x + kPad, iy + 6, g_menu.items[i].label, 235, 235, 235, 255, 2);
    }
}

static void open_context_menu(GLFWwindow* window, double x, double y) {
    g_menu.visible = true;
    g_menu.x = x;
    g_menu.y = y;
    g_menu.hovered = -1;
}

static void close_context_menu() {
    g_menu.visible = false;
    g_menu.hovered = -1;
}

static int menu_item_at(double mx, double my) {
    if (!g_menu.visible) return -1;

    const double left   = g_menu.x;
    const double top    = g_menu.y;
    const double right   = left + kMenuW;
    const double bottom  = top + kItemH * g_menu.items.size();

    if (mx < left || mx > right || my < top || my > bottom)
        return -1;

    return static_cast<int>((my - top) / kItemH);
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    double cx, cy;
    glfwGetCursorPos(window,&cx,&cy);
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        int idx = menu_item_at(cx, cy);
        if (idx >= 0 && idx < static_cast<int>(g_menu.items.size())) {
            auto action = g_menu.items[idx].action;
            close_context_menu();
            if (action) action();
        } else {
            if (g_menu.visible) close_context_menu();
            g_drag.dragging = true;
            g_drag.press_cursor_x = cx;
            g_drag.press_cursor_y = cy;
            glfwGetWindowPos(window,&g_drag.press_window_x,&g_drag.press_window_y);
        }
    } else if (action == GLFW_RELEASE) {
        g_drag.dragging = false;
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
        if (g_menu.visible) close_context_menu();
        open_context_menu(window,cx,cy);

    } else return;
}

static void cursor_pos_callback(GLFWwindow* window, double xpos, double ypos) {
    if (g_drag.dragging)
        glfwSetWindowPos(window, 
            g_drag.press_window_x + static_cast<int>(xpos - g_drag.press_cursor_x), 
            g_drag.press_window_y + static_cast<int>(ypos - g_drag.press_cursor_y));
    if (g_menu.visible)
        g_menu.hovered = menu_item_at(xpos, ypos);

}

// ---------------------------------------------------------------------------
// miscellaneous vulkan helpers
// ---------------------------------------------------------------------------

static VkCommandBuffer begin_single_time_commands() {
    const VkCommandBufferAllocateInfo alloc_info {
        .sType= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = konanix::globals::command::pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    VkCommandBuffer cmd = nullptr;
    if (vkAllocateCommandBuffers(konanix::globals::device::device, &alloc_info, &cmd) != VK_SUCCESS) {
        logger::log("<konanix> Failed to allocate transient command buffer!",logger::exc);
        throw std::runtime_error("failed to allocate transient command buffer");
    }

    const VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr
    };

    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        vkFreeCommandBuffers(konanix::globals::device::device, konanix::globals::command::pool, 1, &cmd);
        logger::log("<konanix> Failed to begin transient command buffer!",logger::exc);
        throw std::runtime_error("failed to begin transient command buffer");
    }

    return std::move(cmd);
}

static void end_single_time_commands(VkCommandBuffer &cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(konanix::globals::device::device, konanix::globals::command::pool, 1, &cmd);
        logger::log("<konanix> Failed to end transient command buffer!",logger::exc);
        throw std::runtime_error("failed to end transient command buffer");
    }

    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,

        .waitSemaphoreCount = 0, 
        .pWaitSemaphores = nullptr, 
        .pWaitDstStageMask = nullptr,

        .commandBufferCount = 1, 
        .pCommandBuffers = &cmd,

        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr
    };

    if (vkQueueSubmit(konanix::globals::device::graphics_queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        vkFreeCommandBuffers(konanix::globals::device::device, konanix::globals::command::pool, 1, &cmd);
        logger::log("<konanix> Failed to submit transient command buffer!",logger::exc);
        throw std::runtime_error("failed to submit transient command buffer");
    }

    vkQueueWaitIdle(konanix::globals::device::graphics_queue);
    vkFreeCommandBuffers(konanix::globals::device::device, konanix::globals::command::pool, 1, &cmd);
}


static SwapChainSupportDetails query_swap_chain_support(const VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapChainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device,surface,&details.capabilities);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device,surface,&format_count,nullptr);
    if (format_count != 0) {
        details.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device,surface,&format_count,details.formats.data());
    }

    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device,surface,&present_mode_count,nullptr);
    if (present_mode_count != 0) {
        details.present_modes.resize(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device,surface,&present_mode_count,details.present_modes.data());
    }
    return details;
}

static QueueFamilyIndices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilyIndices indices;
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device,&queue_family_count,nullptr);
    if (queue_family_count == 0) return indices;
    std::vector<VkQueueFamilyProperties> qfs(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device,&queue_family_count,qfs.data());
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) 
            if (!indices.graphicsFamily.has_value()) indices.graphicsFamily = i;
        
        if (surface != VK_NULL_HANDLE) {

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport == VK_TRUE) {
                if (!indices.presentFamily.has_value()) indices.presentFamily = i;
            }
        }
    } 

    return indices;
}

static bool check_device_extension_support(VkPhysicalDevice device, const std::vector<const char*>& required_extensions) {
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(device,nullptr,&extension_count,nullptr);
    std::vector<VkExtensionProperties> available(extension_count);

    for (const char* needed : required_extensions) {
        bool found = false;
        for (const VkExtensionProperties& av : available) {
            if (std::string(av.extensionName) == needed) { found = true; break; };
        }
        if (!found) return false;
    }
    return true;
}

static int rate_device(VkPhysicalDevice device, VkSurfaceKHR surface, const std::vector<const char*>& required_extensions, const VkPhysicalDeviceFeatures& required_features) {
    VkPhysicalDeviceProperties p_dev{};
    VkPhysicalDeviceFeatures f_dev{};

    vkGetPhysicalDeviceProperties(device,&p_dev);
    vkGetPhysicalDeviceFeatures(device,&f_dev);

    logger::log("<Vulkan> Rating device \""+std::string(p_dev.deviceName)+"\"...",logger::dbg);

    if (required_features.geometryShader && !f_dev.geometryShader) return -1;
    if (required_features.samplerAnisotropy && !f_dev.samplerAnisotropy) return -1;
        logger::log("<Vulkan> Base requirements (geometry shader and sampler anisotropy) are supported!",logger::dbg);

    if (check_device_extension_support(device, required_extensions)) {
            logger::log("<Vulkan> Checking if swap chain extensions are supported on this device...",logger::dbg);{
            SwapChainSupportDetails sc_support = query_swap_chain_support(device, surface);
            if (sc_support.formats.empty() || sc_support.present_modes.empty()) {
                logger::log("<Vulkan> Swap chain extensions are NOT supported by \""+std::string(p_dev.deviceName)+"\"!",logger::dbg);
                return -1;
            }
            logger::log("<Vulkan> Swap chain extensions are supported!",logger::dbg);
        }
    } else {
        logger::log("<Vulkan> Device \""+std::string(p_dev.deviceName)+"\" does not support extensions!",logger::dbg);
        return -1;
    }

    QueueFamilyIndices qfi = find_queue_families(device, surface);
    if (!qfi.complete()) {
        logger::log("<Vulkan> Device \""+std::string(p_dev.deviceName)+"\" does not support queue families!",logger::dbg);
        return -1;
    }

    int score = 0;

    switch (p_dev.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score += 10000; break; // prefer
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 3000; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score += 1000; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score += 100; break;
        default: score += 10; break;
    }

    score += static_cast<int>(p_dev.limits.maxImageDimension2D / 1024);

    // add device-local heap size
    VkPhysicalDeviceMemoryProperties memp_dev{};
    vkGetPhysicalDeviceMemoryProperties(device, &memp_dev);
    for (uint32_t i = 0; i < memp_dev.memoryHeapCount; ++i) {
        if (memp_dev.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            uint64_t mb = memp_dev.memoryHeaps[i].size / (1024ull * 1024ull);
            score += static_cast<int>(std::min<uint64_t>(mb / 256, 2000));
            break;
        }
    }

    if (f_dev.geometryShader) score += 500;
    if (f_dev.samplerAnisotropy) score += 200;

    // prefer dedicated transfer queue
    if (qfi.graphicsFamily.has_value()) {
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(queueFamilyCount);
        if (queueFamilyCount > 0) vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, qfs.data());
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if ((qfs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                score += 50; // dedicated transfer
                break;
            }
        }
    }
    logger::log("<Vulkan> Device \""+std::string(p_dev.deviceName)+"\" scored "+std::to_string(score)+"!",logger::dbg);
    return score;
}

static VkPhysicalDevice pick_device(VkInstance instance, VkSurfaceKHR surface,const std::vector<const char*>& required_extensions = {},const VkPhysicalDeviceFeatures& required_features = VkPhysicalDeviceFeatures{}) {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0) {
        logger::log("No physical devices could be found!",logger::exc);
        logger::log("Check if your graphics card or integrated graphics support Vulkan!",logger::exc);
        logger::log("Check if you have supported Vulkan drivers to run this program (VK_API_VERSION_1_0)!",logger::exc);
        return VK_NULL_HANDLE;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    struct candidate { VkPhysicalDevice dev; int score; };
    std::vector<candidate> candidates;

    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties p_dev {};
        vkGetPhysicalDeviceProperties(dev,&p_dev);
        logger::log("<Vulkan> Device \""+std::string(p_dev.deviceName)+"\" supports Vulkan! Rating device...",logger::dbg);\

        const int score = rate_device(dev,surface,required_extensions,required_features);
        if (score>0) {
            candidates.push_back({dev,score});
            logger::log("<Vulkan> Device \""+std::string(p_dev.deviceName)+"\" accepted! Device scored: "+std::to_string(score)+" points!",logger::dbg);
        } else {
            logger::log("<Vulkan> Device rejected! Reason: score <= 0",logger::dbg);
        }
    }


    if (candidates.empty()) return VK_NULL_HANDLE;


    std::sort(candidates.begin(), candidates.end(), [](const candidate& a, const candidate& b){
        return a.score > b.score; // descending order
    });


    VkPhysicalDevice best = candidates.front().dev;
    VkPhysicalDeviceProperties bestp_dev{};
    vkGetPhysicalDeviceProperties(best, &bestp_dev);
    logger::log("<Vulkan> Selected device: \""+std::string(bestp_dev.deviceName)+"\". Device score: "+std::to_string(candidates.front().score)+" points.", logger::dbg);
    return best;
}


static VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR &capabilities, GLFWwindow* window) {
    if (capabilities.currentExtent.width!=std::numeric_limits<uint32_t>::max() || capabilities.currentExtent.height!=std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    } else {
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);

        VkExtent2D actual = {static_cast<uint32_t>(w),static_cast<uint32_t>(h)};
        actual.width = std::clamp(actual.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actual.height = std::clamp(actual.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return actual;
    }
}

static VkSurfaceFormatKHR choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR> available_formats) {
    // for (const VkSurfaceFormatKHR& availableFormat : available_formats) {
    //     if (availableFormat.format==VK_FORMAT_B8G8R8_SRGB&&availableFormat.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return availableFormat;
    // }
    // return available_formats[0];
    for (const VkSurfaceFormatKHR& f : available_formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }

    for (const VkSurfaceFormatKHR& f : available_formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM ||
             f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }

    for (const VkSurfaceFormatKHR& f : available_formats) {
        if (f.format == VK_FORMAT_B8G8R8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return available_formats[0];

    // for (const VkSurfaceFormatKHR &f : available_formats) {
    //     if ((f.format == VK_FORMAT_B8G8R8A8_SRGB ||
    //          f.format == VK_FORMAT_R8G8B8A8_SRGB ||
    //          f.format == VK_FORMAT_B8G8R8A8_UNORM ||
    //          f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
    //         f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
    //         return f;
    //     }
    // }
    // return available_formats[0];

}

static VkPresentModeKHR choose_swap_present_mode(const std::vector<VkPresentModeKHR> &available_present_modes) {
    for (const VkPresentModeKHR& availablePresentMode : available_present_modes) {
        if (availablePresentMode==VK_PRESENT_MODE_MAILBOX_KHR) {
#ifdef KONANIX_BUILD_WITH_VALIDATION
            logger::log("<konanix> Using present mode VK_PRESENT_MODE_MAILBOX_KHR",logger::dbg);
#endif
            return availablePresentMode;
        }
        if (availablePresentMode==VK_PRESENT_MODE_MAILBOX_KHR) return availablePresentMode;
    }
    for (const VkPresentModeKHR& availablePresentMode : available_present_modes) {
        if (availablePresentMode==VK_PRESENT_MODE_IMMEDIATE_KHR) {
#ifdef KONANIX_BUILD_WITH_VALIDATION
            logger::log("<konanix> Using present mode VK_PRESENT_MODE_IMMEDIATE_KHR",logger::dbg);
#endif
            return availablePresentMode;
        }
    }
#ifdef KONANIX_BUILD_WITH_VALIDATION
    logger::log("<konanix> Using present mode VK_PRESENT_MODE_FIFO_KHR",logger::dbg);
#endif
    return VK_PRESENT_MODE_FIFO_KHR; // best default option
}

static std::vector<char> read_file(const std::string& file) {
    std::ifstream f(file, std::ios::ate | std::ios::binary);
    if (!f.is_open()) {
        logger::log("<Vulkan> Failed to open file at \""+file+"\"!",logger::exc);
        throw std::runtime_error("failed to open file");
    }

    size_t f_size = (size_t) f.tellg();
    std::vector<char> buffer(f_size);

    f.seekg(0);
    f.read(buffer.data(),f_size);
    f.close();

    return buffer;
}

static VkShaderModule create_shader_module(const VkDevice device, const std::vector<char> &code) { // not used anymore
    const VkShaderModuleCreateInfo shader_info {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        code.size(),
        reinterpret_cast<const uint32_t*>(code.data())
    };

    VkShaderModule s_module;
    if (vkCreateShaderModule(device, &shader_info, nullptr, &s_module)) {
        logger::log("<Vulkan> Failed to create a shader module!",logger::exc);
        throw std::runtime_error("failed to create a shader module");
    }

    return s_module;
}

static uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(globals::device::physical_device, &mem_properties);
    
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    logger::log("<Vulkan> Failed to find suitable memory type!",logger::exc);
    throw std::runtime_error("failed to find suitable memory type");
}

static uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties, VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties mem_properties {};
    vkGetPhysicalDeviceMemoryProperties(device,&mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    logger::log("Failed to find a suitable memory type!",logger::exc);
    throw std::runtime_error("failed to find a suitable memory type");
}

static void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer &buffer, VkDeviceMemory &buffer_memory) {
    VkBufferCreateInfo buffer_info {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        size,
        usage,
        VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(globals::device::device,&buffer_info,nullptr,&buffer) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create buffer!",logger::exc);
        throw std::runtime_error("failed to create buffer");
    }

    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(globals::device::device,buffer,&mem_requirements);

    VkMemoryAllocateInfo alloc_info {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        VK_NULL_HANDLE,

        mem_requirements.size,
        find_memory_type(mem_requirements.memoryTypeBits, properties)
    };

    if (vkAllocateMemory(globals::device::device, &alloc_info, nullptr, &buffer_memory) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to allocate memory!",logger::exc);
        throw std::runtime_error("failed to allocate memory");
    }

    vkBindBufferMemory(globals::device::device,buffer,buffer_memory,0);
}

// ---------------------------------------------------------------------------
// glfw rendering callbacks
// ---------------------------------------------------------------------------

static void framebuffer_resize_callback(GLFWwindow* window, int width, int height) {
   g_swapchain_rebuild = true;
   globals::width = width;
   globals::height = height;
};

// ---------------------------------------------------------------------------
// core vulkan initialization functions
// ---------------------------------------------------------------------------

// create a GLFW window and store the global instance
static void create_window() {
    logger::log("<konanix> Creating window instance...",logger::dbg);

    if (!glfwInit()) {
        logger::log("<konanix> Could not initialize GLFW!",logger::exc);
        throw std::runtime_error("failed to initialize glfw3");
    }

    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    if (!RESIZABLE)
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    const GLFWvidmode* m_properties = glfwGetVideoMode(glfwGetPrimaryMonitor());
   
    glfwWindowHint(GLFW_RED_BITS,m_properties->redBits);
    glfwWindowHint(GLFW_GREEN_BITS,m_properties->greenBits);
    glfwWindowHint(GLFW_BLUE_BITS,m_properties->blueBits);
    glfwWindowHint(GLFW_REFRESH_RATE,m_properties->refreshRate);
    logger::log("<konanix> Window will be capped at "+std::to_string(m_properties->refreshRate)+" FPS.",logger::dbg);

    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);

    globals::window = glfwCreateWindow(globals::width,globals::height,"Konata Dancer Remake",nullptr,nullptr);

    if (!globals::window) {
        logger::log("<konanix> Failed to create a GLFW window!",logger::exc);
        glfwTerminate();
        throw std::runtime_error("failed to create a glfw window");
    }

    g_menu.items = {
        {"OPEN",[]{}},
        {"CLOSE",[]{glfwSetWindowShouldClose(globals::window,GLFW_TRUE);}},
    };

    exts = glfwGetRequiredInstanceExtensions(&n_exts);
    glfwSetWindowSize(globals::window,globals::width,globals::height);
    glfwSetFramebufferSizeCallback(globals::window,framebuffer_resize_callback);
    glfwSetCursorPosCallback(globals::window, cursor_pos_callback);
  
    logger::log("<konanix> Window created!",logger::dbg);
}

// create the vulkan instance
static void create_instance() {
    logger::log("<konanix> Creating instance...",logger::dbg);

    static constexpr VkApplicationInfo app_info {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,

        .pApplicationName = konacore::project,
        .applicationVersion = VK_MAKE_VERSION(konacore::version[0],konacore::version[1],konacore::version[2]),
        
        .pEngineName = "konanix",
        .engineVersion = VK_MAKE_VERSION(version[0],version[1],version[2]),

        .apiVersion = VK_API_VERSION_1_3
    };

    static VkInstanceCreateInfo instance_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        
        .pApplicationInfo = &app_info,

        .enabledLayerCount = 0,
        .ppEnabledLayerNames = 0,

        .enabledExtensionCount = n_exts,
        .ppEnabledExtensionNames = exts        
    };

#ifdef KONANIX_BUILD_WITH_VALIDATION
    if (DEBUG) {
        logger::log("<konanix> Debug mode is enabled! You will receive validation logs, but may notice a performance drop!",logger::wrn);
        constexpr static VkDebugUtilsMessengerCreateInfoEXT validation_info {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,

            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = validation_callback,
            .pUserData = nullptr

        };
        const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
        std::vector<const char*> extensions;
        extensions.reserve(n_exts + 1);
        for (uint32_t i = 0; i < n_exts; ++i)
            extensions.push_back(exts[i]);
        extensions.push_back("VK_EXT_debug_utils");
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = layers;
        instance_info.enabledExtensionCount = extensions.size();
        instance_info.ppEnabledExtensionNames = extensions.data();
        instance_info.pNext = &validation_info;

        if (vkCreateInstance(&instance_info,nullptr,&globals::instance) != VK_SUCCESS) {
            logger::log("<konanix> Unable to create a Vulkan instance!",logger::exc);
            throw std::runtime_error("failed to create instance");
        }
        auto create_layers = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(globals::instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_layers(globals::instance, &validation_info, nullptr, &g_debug_messenger) != VK_SUCCESS) {
            logger::log("<konanix> Could not setup the debug messenger!",logger::exc);
            throw std::runtime_error("failed to setup debug messenger!");
        }
    } else {
        if (vkCreateInstance(&instance_info,nullptr,&globals::instance) != VK_SUCCESS) {
            logger::log("<konanix> Unable to create a Vulkan instance!",logger::exc);
            throw std::runtime_error("failed to create instance");
        }
    }
#else

    if (vkCreateInstance(&instance_info,nullptr,&globals::instance) != VK_SUCCESS) {
        logger::log("<konanix> Unable to create a Vulkan instance!",logger::exc);
        throw std::runtime_error("failed to create instance");
    }

#endif
    logger::log("<konanix> Instance created!",logger::dbg);
}

// create the global logical device
static void create_device() {
    logger::log("<konanix> Creating logical device...",logger::dbg);

    const static std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    constexpr static VkPhysicalDeviceFeatures features {
        .geometryShader = VK_TRUE,
        .sampleRateShading = VK_TRUE,
        .samplerAnisotropy = VK_TRUE
    };

    globals::device::physical_device = pick_device(globals::instance,globals::surface);
    QueueFamilyIndices qfi = find_queue_families(globals::device::physical_device,globals::surface);

    if (!qfi.complete()) {
        logger::log("<konanix> Selected physical device does not expose required queue families (graphics/present).", logger::exc);
        throw std::runtime_error("selected device missing required queue families");
    }
    logger::log("<konanix> Queue families are present!",logger::dbg);


    std::vector<VkDeviceQueueCreateInfo> queue_create_info_vec;
    std::set<uint32_t> uniqueQueueFamilies = {qfi.graphicsFamily.value(),qfi.presentFamily.value()};
    float queue_priority = 1.0f;
    for (uint32_t queue_family : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queue_info {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,

            .queueFamilyIndex = queue_family,
            .queueCount = 1,
            .pQueuePriorities = &queue_priority
        };
        queue_create_info_vec.push_back(queue_info);
    }

    const static VkDeviceCreateInfo dev_info {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,

        .queueCreateInfoCount = static_cast<uint32_t>(queue_create_info_vec.size()),
        .pQueueCreateInfos = queue_create_info_vec.data(),

        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,

        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dev_exts.data(),
        .pEnabledFeatures = &features
    };
    
    if (vkCreateDevice(globals::device::physical_device,&dev_info,nullptr,&globals::device::device) != VK_SUCCESS) {
        logger::log("<konanix> Failed to create a logical device!",logger::exc);
        throw std::runtime_error("failed to create a logical device");
    }
    logger::log("<konanix> Logical device created!",logger::dbg);
    vkGetDeviceQueue(globals::device::device,qfi.graphicsFamily.value(),0,&globals::device::graphics_queue);
    vkGetDeviceQueue(globals::device::device,qfi.presentFamily.value(),0,&globals::device::present_queue);
    logger::log("<konanix> Graphics and present queue set!",logger::dbg);

    // if (vkCreateDevice(globals::device::physical_device,, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice))
    logger::log("<konanix> Device created!",logger::dbg);
}

static void create_swap_chain() {
    logger::log("<konanix> Creating swap chain...",logger::dbg);
   
    VkSwapchainKHR old_swapchain = globals::swapchain::swapchain;

    SwapChainSupportDetails swap_chain_support = query_swap_chain_support(globals::device::physical_device, globals::surface);
    VkSurfaceFormatKHR swap_surface = choose_swap_surface_format(swap_chain_support.formats);
    VkPresentModeKHR present_mode = choose_swap_present_mode(swap_chain_support.present_modes);
    VkExtent2D extent = choose_swap_extent(swap_chain_support.capabilities, globals::window);

    uint32_t image_count = swap_chain_support.capabilities.minImageCount+1; // using fifo
    if (swap_chain_support.capabilities.maxImageCount > 0 && image_count > swap_chain_support.capabilities.maxImageCount) { // check max
        image_count = swap_chain_support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sc_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,

        .surface = globals::surface,
        .minImageCount = image_count,

        .imageFormat = swap_surface.format,
        .imageColorSpace = swap_surface.colorSpace,

        .imageExtent = extent,

        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        // VK_IMAGE_USAGE_SAMPLED_BIT

        .oldSwapchain = old_swapchain
    };

    //logger::log("<konanix> Swapchain format selected: " + std::to_string(swap_surface.format)+", color space: " + std::to_string(swap_surface.colorSpace),logger::dbg);
    //logger::log("<konanix> Composite alpha selected: " + std::to_string(sc_info.compositeAlpha),logger::dbg);
    //logger::log("<konanix> Supported composite alpha flags: "+std::to_string(swap_chain_support.capabilities.supportedCompositeAlpha),logger::dbg);

    const QueueFamilyIndices indices = find_queue_families(globals::device::physical_device, globals::surface);
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily) {
        sc_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sc_info.queueFamilyIndexCount = 2;
        sc_info.pQueueFamilyIndices = queueFamilyIndices;
        //logger::log("<konanix> Swap chain will be using \"VK_SHARING_MODE_CONCURRENT\".",logger::dbg);
    } else {
        sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sc_info.queueFamilyIndexCount = 0;
        sc_info.pQueueFamilyIndices = nullptr;
        //logger::log("<konanix> Swap chain will be using \"VK_SHARING_MODE_EXCLUSIVE\".",logger::dbg);
    }
    sc_info.preTransform = swap_chain_support.capabilities.currentTransform;
    sc_info.compositeAlpha =
    (swap_chain_support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
        : (swap_chain_support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR
            : (swap_chain_support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    sc_info.presentMode = present_mode;
    sc_info.clipped = VK_TRUE; // enable if full rendering is required in the future https://vulkan-tutorial.com/Drawing_a_triangle/Presentation/Swap_chain

    //sc_info.oldSwapchain = nullptr; // TODO: MAKE REBUILDABLE SWAPCHAINS
    if (vkCreateSwapchainKHR(globals::device::device,&sc_info,nullptr,&globals::swapchain::swapchain)!=VK_SUCCESS) {
        logger::log("<konanix> Failed to create swap chain!",logger::exc);
        throw std::runtime_error("failed to create swap chain");
    }

    vkDestroySwapchainKHR(globals::device::device,old_swapchain,globals::allocator);

    vkGetSwapchainImagesKHR(globals::device::device, globals::swapchain::swapchain, &image_count, nullptr);
    globals::swapchain::images.resize(image_count);
    vkGetSwapchainImagesKHR(globals::device::device, globals::swapchain::swapchain, &image_count, globals::swapchain::images.data());
#ifdef KONANIX_BUILD_WITH_VALIDATION 
    logger::log("<konanix> Populated \"globals::swapchain::images\" vector!",logger::dbg);
#endif

    globals::swapchain::format = swap_surface.format;
    globals::swapchain::extent = extent;

    logger::log("<konanix> Swap chain created!",logger::dbg);
}

static void create_image_views() {
    globals::swapchain::image_views.resize(globals::swapchain::images.size());
    for (size_t i = 0; i < globals::swapchain::images.size(); i++) {
        const VkImageViewCreateInfo imageview_info {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            VK_NULL_HANDLE,
            0,
            
            globals::swapchain::images[i],
            VK_IMAGE_VIEW_TYPE_2D,
            globals::swapchain::format,

            {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY
            },
            {
                VK_IMAGE_ASPECT_COLOR_BIT,
                0,
                1,
                0,
                1
            }
        };

        if (vkCreateImageView(globals::device::device,&imageview_info,nullptr,&globals::swapchain::image_views[i])) {
            logger::log("<Vulkan> Failed to create swap chain!",logger::exc);
            throw std::runtime_error("failed to create swap chain");
        }
    }
}

// ---------------------------------------------------------------------------
// descriptor functions
// ---------------------------------------------------------------------------

static void create_descriptor_pool() {
    constexpr VkDescriptorPoolSize pool_size {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) /*4*/
        };

    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,

        .maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) /*4*/,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };

    if (vkCreateDescriptorPool(globals::device::device, &pool_info, nullptr, &globals::descriptor::pool) != VK_SUCCESS) {
        logger::log("<konanix> Failed to create the descriptor pool!",logger::exc);
        throw std::runtime_error("failed to create descriptor pool");
    }

    logger::log("<konanix> Descriptor pool created!",logger::dbg);
}

void konanix::create_descriptor_set() {
    const static std::array<VkDescriptorSetLayout,MAX_FRAMES_IN_FLIGHT /*4*/> layouts({
            // globals::descriptor::layout,
            // globals::descriptor::layout,
            globals::descriptor::layout,
            globals::descriptor::layout
    });

    const VkDescriptorSetAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        
        .descriptorPool = globals::descriptor::pool,
        .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
        .pSetLayouts = layouts.data()
    };

    globals::descriptor::sets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(globals::device::device, &alloc_info, globals::descriptor::sets.data()) != VK_SUCCESS) {
        logger::log("<konanix> Unable to allocate descriptor sets!",logger::exc);
        throw std::runtime_error("vkAllocateDescriptorSets failed!");
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        const VkDescriptorImageInfo image_info{
            g_gif_sampler,
            gif_frames[0].image_view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkWriteDescriptorSet descriptor_write {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                
                .dstSet = globals::descriptor::sets[i],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_info,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr
        };

        vkUpdateDescriptorSets(globals::device::device, 1, &descriptor_write, 0, nullptr);

    }
    logger::log("<konanix> Descriptor sets created!",logger::dbg);
}

static void create_descriptor_set_layout() {
     constexpr VkDescriptorSetLayoutBinding gif_binding {
        0,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        1,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        nullptr
     };
     
#ifdef KONANIX_BUILD_WITH_VALIDATION
    logger::log("<konanix> Descriptor set binding is of type "+std::to_string(gif_binding.descriptorType)+" with count "+std::to_string(gif_binding.descriptorCount),logger::dbg);
#endif
    const VkDescriptorSetLayoutCreateInfo layout_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &gif_binding
    };

    if (vkCreateDescriptorSetLayout(globals::device::device,&layout_info,globals::allocator,&globals::descriptor::layout) != VK_SUCCESS) {
        logger::log("<konanix> Failed to create global descriptor set layout!",logger::exc);
        throw std::runtime_error("failed to create global descriptor set layout!");
    }

    logger::log("<konanix> Descriptor set layout created!",logger::dbg);
}

static void update_descriptor_set() {

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        const VkDescriptorImageInfo image_info {
            g_gif_sampler,
            gif_frames[0].image_view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };

        const VkWriteDescriptorSet write {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,

            .dstSet = globals::descriptor::sets[i],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr
        };

        vkUpdateDescriptorSets(globals::device::device, 1, &write, 0, nullptr);

    }
    logger::log("<konanix> Descriptor sets updated!",logger::dbg);
}

void set_gif_frame(uint32_t frame) 
{
    const VkDescriptorImageInfo image_info{
        g_gif_sampler,
        gif_frames[frame].image_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    const VkWriteDescriptorSet write {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = globals::descriptor::sets[current_frame],
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info
    };

    vkUpdateDescriptorSets(globals::device::device, 1, &write, 0, nullptr);
}

// ---------------------------------------------------------------------------
// fixed functions
// ---------------------------------------------------------------------------

static void create_graphics_pipeline() {
    logger::log("<Vulkan> Creating the graphics pipeline...",logger::dbg);

    const VkShaderModuleCreateInfo vertex_info {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        vert_spv_len,
        reinterpret_cast<const uint32_t*>(vert_spv)
    };

    VkShaderModule vertex_shader;
    if (vkCreateShaderModule(globals::device::device, &vertex_info, nullptr, &vertex_shader)) {
        logger::log("<Vulkan> Failed to create a shader module!",logger::exc);
        throw std::runtime_error("failed to create a shader module");
    }

    const VkShaderModuleCreateInfo fragment_info {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        frag_spv_len,
        reinterpret_cast<const uint32_t*>(frag_spv)
    };

    VkShaderModule fragment_shader;
    if (vkCreateShaderModule(globals::device::device, &fragment_info, nullptr, &fragment_shader)) {
        logger::log("<Vulkan> Failed to create a shader module!",logger::exc);
        throw std::runtime_error("failed to create a shader module");
    }

        VkPipelineShaderStageCreateInfo v_shader_info{};
        v_shader_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        v_shader_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        v_shader_info.module = vertex_shader;
        v_shader_info.pName = "main";

        VkPipelineShaderStageCreateInfo f_shader_info{};
        f_shader_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        f_shader_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        f_shader_info.module = fragment_shader;
        f_shader_info.pName = "main";


    const VkPipelineShaderStageCreateInfo shader_stages[2] = {v_shader_info,f_shader_info};
    logger::log("<Vulkan> Pipeline shader stages set!",logger::dbg);
    
    const std::vector<VkDynamicState> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic_state {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        static_cast<uint32_t>(dynamic_states.size()),
        dynamic_states.data()
    };

    const VkPipelineVertexInputStateCreateInfo vertex_input_info {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        0,
        nullptr,
        0,
        nullptr
    };

    constexpr VkPipelineInputAssemblyStateCreateInfo input_assembly_state_info {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,

        VK_FALSE
    };

    const VkViewport viewport {
        0.0f,
        0.0f,
        (float) globals::swapchain::extent.width,
        (float) globals::swapchain::extent.height,
        0.0f,
        1.0f
    };

    const VkRect2D scissor {
        {0,0},
            globals::swapchain::extent
    };

    const VkPipelineViewportStateCreateInfo viewport_state_info {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        
        1,
        &viewport,
        
        1,
        &scissor
    };


    const VkPipelineRasterizationStateCreateInfo rasterizer {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        VK_FALSE,VK_FALSE,
        VK_POLYGON_MODE_FILL,
        0,
        VK_FRONT_FACE_CLOCKWISE,
        VK_FALSE,
        1.0f,
        0.0f,
        0.0f,
        1.0f
    };


    constexpr VkPipelineMultisampleStateCreateInfo multisampling {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,
        1.0f,
        nullptr,
        VK_FALSE,
        VK_FALSE
    };


    constexpr VkPipelineColorBlendAttachmentState color_blend_attachment {
        VK_TRUE,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_OP_ADD,
        // VK_FALSE,
        // VK_BLEND_FACTOR_ONE,
        // VK_BLEND_FACTOR_ZERO,
        // VK_BLEND_OP_ADD,

        VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_OP_ADD,

        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    }; 

    const VkPipelineColorBlendStateCreateInfo color_blending {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        VK_FALSE,
        VK_LOGIC_OP_COPY,
        1,
        &color_blend_attachment,
        
        {
            0.0f, 0.0f, 0.0f, 0.0f
        }
    };

    const VkPipelineLayoutCreateInfo pipeline_layout_info {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        // 0,
        // nullptr,
        1,
        &globals::descriptor::layout,
        0,
        nullptr
    };

    if (vkCreatePipelineLayout(globals::device::device,&pipeline_layout_info,nullptr,&globals::pipeline::graphics_pipeline_layout) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create the pipeline layout!",logger::exc);
        throw std::runtime_error("failed to create the pipeline layout");
    }

    logger::log("<Vulkan> Creating graphics pipeline object...",logger::dbg);

    const VkGraphicsPipelineCreateInfo pipeline_info {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        2,
        shader_stages,
        &vertex_input_info,
        &input_assembly_state_info,
        VK_NULL_HANDLE,
        &viewport_state_info,
        &rasterizer,
        &multisampling,
        nullptr,
        &color_blending,
        &dynamic_state,

        globals::pipeline::graphics_pipeline_layout,
        globals::pipeline::renderpass,
        0,

        VK_NULL_HANDLE,
        -1
    };

    if (vkCreateGraphicsPipelines(globals::device::device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &globals::pipeline::graphics_pipeline) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create the graphics pipeline!",logger::exc);
        throw std::runtime_error("failed to create the graphics pipeline");
    }

    logger::log("<Vulkan> Graphics pipeline created! Cleaning shader data...",logger::dbg);
    vkDestroyShaderModule(globals::device::device,fragment_shader,nullptr);
    vkDestroyShaderModule(globals::device::device,vertex_shader,nullptr);

    logger::log("<Vulkan> Graphics pipeline created!",logger::dbg);
}

static void create_render_pass() {
    logger::log("<Vulkan> Creating the render pass...",logger::dbg);
    // comments from https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Render_passes
    const VkAttachmentDescription color_attachment {
        0,
        globals::swapchain::format,
        VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,     // Existing contents are undefined; don't care about them
        VK_ATTACHMENT_STORE_OP_DONT_CARE,   // Contents of the framebuffer will be undefined after the rendering operation
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR        // Images to be presented in the swap chain
    };

    constexpr VkAttachmentReference color_attachment_ref {
        0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    const VkSubpassDescription subpass {
        0,

        VK_PIPELINE_BIND_POINT_GRAPHICS,
        0,
        VK_NULL_HANDLE,
        1,
        &color_attachment_ref
    };

    constexpr VkSubpassDependency dependency {
        VK_SUBPASS_EXTERNAL,
        0,


        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,

        // VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, // .srcAccessMask
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    const VkRenderPassCreateInfo renderpass_info {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        1,
        &color_attachment,

        1,
        &subpass,

        1,
        &dependency
    };

    if (vkCreateRenderPass(globals::device::device, &renderpass_info, nullptr, &globals::pipeline::renderpass) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create render pass!",logger::exc);
        throw std::runtime_error("failed to create render pass");
    }

    logger::log("<Vulkan> Render pass created!",logger::dbg);
}

static void create_framebuffers() {
    globals::swapchain::framebuffers.resize(globals::swapchain::image_views.size());

    for (size_t i = 0; i < globals::swapchain::image_views.size(); ++i) {

        const VkFramebufferCreateInfo framebuffer_info {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,

            .renderPass = globals::pipeline::renderpass,
            .attachmentCount = 1,
            .pAttachments = &globals::swapchain::image_views[i],
            .width = globals::swapchain::extent.width,
            .height = globals::swapchain::extent.height,
            .layers = 1
        };

        if (vkCreateFramebuffer(globals::device::device, &framebuffer_info, nullptr, &globals::swapchain::framebuffers[i]) != VK_SUCCESS) {
            logger::log("<konanix> Failed to a framebuffer!",logger::exc);
            throw std::runtime_error("failed to create a framebuffer");
        }
    }
}

static void create_command_pool() {
    logger::log("<konanix> Creating command pool...",logger::dbg);

    QueueFamilyIndices qfi = find_queue_families(globals::device::physical_device, globals::surface);

    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,

        // VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,               // Hint that command buffers are rerecorded with new commands very often (may change memory allocation behavior)
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,    // Allow command buffers to be rerecorded individually, without this flag they all have to be reset together

        .queueFamilyIndex = qfi.graphicsFamily.value()
    };

    if (vkCreateCommandPool(globals::device::device,&pool_info,nullptr,&globals::command::pool) != VK_SUCCESS) {
            logger::log("<konanix> Failed to the command pool!",logger::exc);
            throw std::runtime_error("failed to create command pool");
    }
    logger::log("<konanix> Command pool created!",logger::dbg);

}

// ---------------------------------------------------------------------------
// image-based functions
// ---------------------------------------------------------------------------

static void create_image(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                            VkMemoryPropertyFlags properties, VkImage &image, VkDeviceMemory &image_memory) {
    const VkImageCreateInfo image_info {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        VK_IMAGE_TYPE_2D,
        format,
        {width,height,1},
        1,
        1,
        VK_SAMPLE_COUNT_1_BIT,
        tiling,
        usage,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        nullptr,
        VK_IMAGE_LAYOUT_UNDEFINED
    };

    if (vkCreateImage(globals::device::device,&image_info,nullptr,&image) != VK_SUCCESS) {
        logger::log("Failed to create image!",logger::exc);
        throw std::runtime_error("failed to create image");
    }
    logger::log("Vulkan image created!",logger::dbg);
    VkMemoryRequirements mem_requirements{};
    vkGetImageMemoryRequirements(globals::device::device, image, &mem_requirements);

    const VkMemoryAllocateInfo alloc_info{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        VK_NULL_HANDLE,
        mem_requirements.size,
        find_memory_type(mem_requirements.memoryTypeBits, properties)
    };

    if (vkAllocateMemory(globals::device::device, &alloc_info, nullptr, &image_memory) != VK_SUCCESS) {
        vkDestroyImage(globals::device::device,image,nullptr);
        image = VK_NULL_HANDLE;
        logger::log("Failed to allocate image memory!",logger::exc);
        throw std::runtime_error("failed to allocate image memory");
    }
    if (vkBindImageMemory(globals::device::device, image, image_memory, 0) != VK_SUCCESS) {
        vkFreeMemory(globals::device::device, image_memory, nullptr);
        vkDestroyImage(globals::device::device, image, nullptr);
        image = VK_NULL_HANDLE;
        image_memory = VK_NULL_HANDLE;
        logger::log("Failed to bind image memory!",logger::exc);
        throw std::runtime_error("failed to bind image memory");
    }
    logger::log("Allocated memory for Vulkan image!",logger::dbg);
}

static VkImageView create_image_view(VkImage image, VkFormat format) {
    const VkImageViewCreateInfo view_info {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        image,
        VK_IMAGE_VIEW_TYPE_2D,
        format,
        {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY
        },
        {
            VK_IMAGE_ASPECT_COLOR_BIT,
            0,
            1,
            0,
            1
        }
    };

    VkImageView image_view = VK_NULL_HANDLE;
    if (vkCreateImageView(globals::device::device,&view_info,nullptr,&image_view) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image view");
    } 

    return image_view;
}

static VkSampler create_sampler() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(globals::device::physical_device,&properties);

    const VkSamplerCreateInfo sampler_info {
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        VK_FILTER_LINEAR,
        VK_FILTER_LINEAR,
        VK_SAMPLER_MIPMAP_MODE_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        0.0f,
        VK_FALSE,
        1.0f,
        VK_FALSE,
        VK_COMPARE_OP_ALWAYS,
        0.0f,
        0.0f,
        // VK_BORDER_COLOR_INT_TRANSPARENT_BLACK,
        VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        VK_FALSE
    };

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(globals::device::device,&sampler_info,nullptr,&sampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create sampler");
    }
    return sampler;
}

static void create_image_views(const std::vector<VkImage> &images, const VkFormat format, std::vector<VkImageView> &image_views, const VkImageAspectFlagBits aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT) {
    image_views.resize(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        VkImageViewCreateInfo image_views_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,

            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = aspect_flags,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        if (vkCreateImageView(globals::device::device,&image_views_info,nullptr,&image_views[i])) {
            logger::log("<konanix> Failed to create image views!",logger::exc);
            throw std::runtime_error("failed to create image views");
        }
    }
}

static void create_image_view(const VkImage &image, const VkFormat format, VkImageView &image_view, const VkImageAspectFlagBits aspect_flag = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageViewCreateInfo imageview_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = {
            .aspectMask = aspect_flag,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    if (vkCreateImageView(globals::device::device,&imageview_info,nullptr,&image_view)) {
        logger::log("<konanix> Failed to create image views!",logger::exc);
        throw std::runtime_error("failed to create image views");
    }
    
}

static void transition_image_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout) {
    VkCommandBuffer cmd = begin_single_time_commands();

    VkImageMemoryBarrier barrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        0, 0,
        old_layout,
        new_layout,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        image,
        {
            VK_IMAGE_ASPECT_COLOR_BIT,
            0, 1,
            0, 1
        }
    };

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else {
        vkFreeCommandBuffers(globals::device::device, globals::command::pool, 1, &cmd);
        logger::log("Unsupported image layout transition!",logger::exc);
        throw std::runtime_error("unsupported image layout transition");
    }

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    end_single_time_commands(cmd);
}

static void copy_buffer_to_image(
    VkBuffer buffer,
    VkImage image,
    uint32_t width,
    uint32_t height
) {
    VkCommandBuffer cmd = begin_single_time_commands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(
        cmd,
        buffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    end_single_time_commands(cmd);
}

void konanix::upload_rgba_frame_to_gif_image(const uint8_t* rgba_pixels, size_t pixel_bytes) {
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;

    create_buffer(
        static_cast<VkDeviceSize>(pixel_bytes),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging_buffer,
        staging_memory
    );

    void* mapped = nullptr;
    if (vkMapMemory(globals::device::device, staging_memory, 0, pixel_bytes, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(globals::device::device, staging_buffer, nullptr);
        vkFreeMemory(globals::device::device, staging_memory, nullptr);
        logger::log("Failed to map staging memory!",logger::exc);
        throw std::runtime_error("failed to map staging memory");
    }

    memcpy(mapped, rgba_pixels, pixel_bytes);
    vkUnmapMemory(globals::device::device, staging_memory);

    transition_image_layout(
        active_frame.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    );

    copy_buffer_to_image(
        staging_buffer,
        active_frame.image,
        globals::width,
        globals::height
    );

    transition_image_layout(
        active_frame.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    );

    vkDestroyBuffer(globals::device::device, staging_buffer, nullptr);
    vkFreeMemory(globals::device::device, staging_memory, nullptr);
}

void create_image(const uint32_t w, const uint32_t h, const VkSampleCountFlagBits samples, uint32_t mip_levels, 
                            const VkFormat format, const VkImageTiling tiling,
                            const VkImageUsageFlags usage, const VkMemoryPropertyFlags properties,
                            VkImage &image, VkDeviceMemory &memory) {
    if (mip_levels < 1) {
        mip_levels = 1;
        logger::log("<konanix> Corrected image mip levels to 1.",logger::wrn);
    }

    const VkImageCreateInfo image_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,

        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = w,
            .height = h,
            .depth = 1
        },
        .mipLevels = mip_levels,
        .arrayLayers = 1,
        .samples = samples,
        .tiling = tiling,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateImage(konanix::globals::device::device,&image_info,globals::allocator,&image) != VK_SUCCESS) {
        logger::log("<konanix> Failed to create image!",logger::exc);
        throw std::runtime_error("failed to create image");
    }

    VkMemoryRequirements m_req;
    vkGetImageMemoryRequirements(konanix::globals::device::device,image,&m_req);

    const VkMemoryAllocateInfo alloc_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,

        .allocationSize = m_req.size,
        .memoryTypeIndex = konanix::globals::device::find_memory_type(m_req.memoryTypeBits,properties)
    };

    if (vkAllocateMemory(konanix::globals::device::device,&alloc_info,globals::allocator,&memory) != VK_SUCCESS) {
        logger::log("<konanix> Could not allocate texture memory!",logger::exc);
        throw std::runtime_error("failed to allocate memory");
    }
    vkBindImageMemory(konanix::globals::device::device,image,memory,0);

}

void transition_image_layout(VkCommandBuffer &cmd, const VkImage &image, const VkImageLayout old_layout, const VkImageLayout new_layout, uint32_t mip_levels) {
    VkImageMemoryBarrier barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,

        .srcAccessMask = 0,
        .dstAccessMask = 0,

        .oldLayout = old_layout,
        .newLayout = new_layout,

        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,

        .image = image,
        .subresourceRange = {
            .aspectMask= VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = {};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        logger::log("<konanix> Unsupported image layout transition!",logger::exc);
        throw std::invalid_argument("unsupported layout transition");
    }

    vkCmdPipelineBarrier(cmd,
        src_stage,dst_stage,
        {},{},{},
        0,nullptr,
        1,&barrier
    );
}

void copy_buffer_to_image(VkCommandBuffer &cmd, const VkBuffer &buffer, const VkImage &image, const uint32_t width, const uint32_t height) {
    const VkBufferImageCopy region {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageOffset = {
            .x = 0,
            .y = 0,
            .z = 0,
        },
        .imageExtent ={
            .width = width,
            .height = height,
            .depth = 1
        }
    };

    vkCmdCopyBufferToImage(cmd,
        buffer,image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,&region
    );
}

void konanix::create_gif_image(const unsigned char *pixels, const uint32_t &frame, uint32_t width, uint32_t height) {
    if (!pixels) {
            logger::log("<konanix> Received invalid pixels!",logger::exc);
            throw std::runtime_error("failed to load texture");
    }
    
    const VkDeviceSize size = width * height * 4;

    VkBuffer staging_buffer;
    VkDeviceMemory staging_buffer_mem;
    create_buffer(size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging_buffer,staging_buffer_mem
    );

    void* data;
    vkMapMemory(konanix::globals::device::device,staging_buffer_mem,0,size,0,&data);
    memcpy(data,pixels,size);
    vkUnmapMemory(konanix::globals::device::device,staging_buffer_mem);

    create_image(width,height,VK_SAMPLE_COUNT_1_BIT,1,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        gif_frames[frame].image, gif_frames[frame].image_memory
    );

    VkCommandBuffer cmd = begin_single_time_commands();

    transition_image_layout(cmd,gif_frames[frame].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1);
    copy_buffer_to_image(cmd,staging_buffer,gif_frames[frame].image,static_cast<uint32_t>(width),static_cast<uint32_t>(height));
    transition_image_layout(cmd,gif_frames[frame].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,1);
    // generate_mipmaps(cmd, image, VK_FORMAT_R8G8B8A8_SRGB, t_width, t_height, 1);

    end_single_time_commands(cmd);

    vkDestroyBuffer(konanix::globals::device::device,staging_buffer,konanix::globals::allocator);
    vkFreeMemory(konanix::globals::device::device,staging_buffer_mem,konanix::globals::allocator);

    // create_image(width,height,surface_format.format,
    // VK_IMAGE_TILING_OPTIMAL,
    // VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    // gif_frames[frame].image,gif_frames[frame].image_memory);
    //
    gif_frames[frame].image_view = create_image_view(gif_frames[frame].image,VK_FORMAT_R8G8B8A8_SRGB);


    // transition_image_layout(g_gif_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ---------------------------------------------------------------------------
// buffer functions
// ---------------------------------------------------------------------------

static void create_command_buffers() {
    logger::log("<konanix> Allocating command buffer...",logger::dbg);
    globals::command::buffers.resize(MAX_FRAMES_IN_FLIGHT);

    const VkCommandBufferAllocateInfo alloc_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        
        .commandPool = globals::command::pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = (uint32_t) globals::command::buffers.size()
    };

    if (vkAllocateCommandBuffers(globals::device::device,&alloc_info,globals::command::buffers.data()) != VK_SUCCESS) {
            logger::log("<konanix> Failed to allocate the command buffer!",logger::exc);
            throw std::runtime_error("failed to allocate command buffer");
    }

    logger::log("<konanix> Command buffer allocated successfully!",logger::dbg);

}

// ---------------------------------------------------------------------------
// miscellaneous objects
// ---------------------------------------------------------------------------

static void create_sync_objects() {
    // globals::sync::image_available_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
    globals::sync::render_finished_semaphores.resize(globals::swapchain::images.size());
    // globals::sync::in_flight_fences.resize(MAX_FRAMES_IN_FLIGHT);
    globals::sync::images_in_flight.resize(globals::swapchain::images.size(),VK_NULL_HANDLE);

    constexpr VkSemaphoreCreateInfo semaphore_info {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0
    };

    constexpr VkFenceCreateInfo fence_info {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(globals::device::device,&semaphore_info,nullptr,&globals::sync::image_available_semaphores[i]) != VK_SUCCESS) {
            logger::log("<konanix> Failed create a semaphore!",logger::exc);
            throw std::runtime_error("failed to create a semaphore");
        }     

        if (vkCreateFence(globals::device::device,&fence_info,nullptr,&globals::sync::in_flight_fences[i]) != VK_SUCCESS) {
            logger::log("<konanix> Failed create a fance!",logger::exc);
            throw std::runtime_error("failed to create a fence");
        }

        //if (vkCreateSemaphore(globals::device::device,&semaphore_info,nullptr,&globals::sync::render_finished_semaphores[i]) != VK_SUCCESS) {
        //    logger::log("<konanix> Failed create a semaphore!",logger::exc);
        //    throw std::runtime_error("failed to create a semaphore");
        //}

    } 

    for (size_t i = 0; i < globals::swapchain::images.size(); ++i)
        if (vkCreateSemaphore(globals::device::device,&semaphore_info,nullptr,&globals::sync::render_finished_semaphores[i]) != VK_SUCCESS) {
            logger::log("<konanix> Failed create a semaphore!",logger::exc);
            throw std::runtime_error("failed to create a semaphore");
        }
}

void konanix::recreate_swap_chain() {
    logger::log("<konanix> Recreating swap chain, waiting for device...",logger::dbg);
    vkDeviceWaitIdle(globals::device::device);
    // vkDeviceWaitIdle(globals::device);
    glfwGetWindowSize(globals::window,&globals::width,&globals::height);
    if (globals::width == 0 || globals::height == 0) {
        glfwWaitEvents();
        recreate_swap_chain();
    }

    //globals::swapchain::image_views.clear();
    //globals::swapchain::swapchain = nullptr;

    // for (VkFramebuffer f : globals::swapchain::framebuffers)
    //     vkDestroyFramebuffer(globals::device::device, f, nullptr);

    // for (VkImageView iw : globals::swapchain::image_views)
    //     vkDestroyImageView(globals::device::device, iw, nullptr);

    // vkDestroySwapchainKHR(globals::device::device, globals::swapchain::swapchain, nullptr);
    
    // vkDestroyImage(globals::device::device,globals::swapchain::images[current_frame],nullptr);
    // vkDestroyImageView(globals::device::device, globals::swapchain::image_views[current_frame], nullptr);
    // vkDestroySampler(globals::device::device,g_sampler,nullptr);

    //create_image_view(g_depth_image,find_depth_format(),g_depth_image_view,VK_IMAGE_ASPECT_DEPTH_BIT);

    // destroy/free old objects 
    // vkDestroyImage(globals::device::device,g_color_image,globals::allocator);
    // vkDestroyImageView(globals::device::device,g_color_image_view,globals::allocator);
    // vkFreeMemory(globals::device::device,g_color_memory,globals::allocator);
    //
    // vkDestroyImage(globals::device::device,g_depth_image,globals::allocator);
    // vkDestroyImageView(globals::device::device,g_depth_image_view,globals::allocator);
    // vkFreeMemory(globals::device::device,g_depth_memory,globals::allocator);
    
    for (VkImageView &iw : globals::swapchain::image_views)
        vkDestroyImageView(globals::device::device,iw,globals::allocator);

    for (VkFramebuffer &fb : globals::swapchain::framebuffers)
        vkDestroyFramebuffer(globals::device::device,fb,globals::allocator);

    // recreate
    create_swap_chain();

    create_image_views(globals::swapchain::images,globals::swapchain::format,globals::swapchain::image_views,VK_IMAGE_ASPECT_COLOR_BIT);

    // create_color_resources(g_color_image,g_color_memory,g_color_image_view);
    // create_depth_resources(g_depth_image,g_depth_memory,g_depth_image_view);

    create_framebuffers();

    //globals::swapchain::framebuffers.clear();
    //globals::swapchain::framebuffers.resize(globals::swapchain::image_views.size());
    // update_descriptor_set();
    // create_framebuffers();

    globals::sync::images_in_flight.assign(globals::swapchain::images.size(),VK_NULL_HANDLE);

    // logger::log("extent width: "+std::to_string(globals::swapchain::extent.width));
    // logger::log("extent height: "+std::to_string(globals::swapchain::extent.height));
    // logger::log("aspect ratio: "+std::to_string(globals::swapchain::aspect_ratio()));

    logger::log("<konanix> Swap chain recreated!",logger::dbg);
}

// ---------------------------------------------------------------------------
// initialization and cleanup
// ---------------------------------------------------------------------------

void konanix::initialize(const uint32_t &w, const uint32_t &h, const bool &debug, const bool &resizable) {
    logger::log("<konanix> Initializing renderer...");
    globals::width = std::move(w);
    globals::height = std::move(h);
    DEBUG = debug;
    RESIZABLE = resizable;

    gif_frames.clear();
    GIF_FRAME_COUNT = static_cast<uint32_t>(konanix::g_raw_anim_data.frames.size());
    gif_frames.resize(GIF_FRAME_COUNT);

    create_window();
    create_instance();

    logger::log("<konanix> Creating window surface...",logger::dbg);
    glfwCreateWindowSurface(globals::instance,globals::window,globals::allocator,&globals::surface);
    logger::log("<konanix> Window surface created!",logger::dbg);
    
    create_device();

    create_swap_chain();
    create_image_views(globals::swapchain::images,globals::swapchain::format,globals::swapchain::image_views,VK_IMAGE_ASPECT_COLOR_BIT);

    create_render_pass();

    create_command_pool();

    //create_texture_image(*static_cast<stbi_uc*>(texture),texture_len,g_texture,g_texture_mem);
    //create_texture_image_view(g_texture,VK_FORMAT_R8G8B8A8_SRGB,g_texture_image_view,1);
    //create_texture_sampler(g_texture_sampler);

    // create_gif_image(0,globals::width, globals::height);

    for (size_t i = 0; i < konanix::g_raw_anim_data.frames.size(); ++i) {
        konanix::create_gif_image(konanix::g_raw_anim_data.frames[i].rgba.data(), i, konanix::g_raw_anim_data.width, konanix::g_raw_anim_data.height);
        // logger::log("Loaded frame "+std::to_string(i),logger::dbg);
    }

    // konanix::g_raw_anim_data.frames.clear();

    g_gif_sampler = create_sampler();

    create_descriptor_pool();
    create_descriptor_set_layout();
    create_descriptor_set();
    create_graphics_pipeline();
    create_framebuffers();

    create_command_buffers();

    create_sync_objects();

    logger::log("<konanix> Renderer initialized!");
}

void konanix::cleanup() {
    logger::log("<konanix> Cleaning up Vulkan resources...",logger::dbg);

    vkDeviceWaitIdle(globals::device::device);

#ifdef KONANIX_BUILD_WITH_VALIDATION
    if (DEBUG) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(globals::instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func)
            func(globals::instance,g_debug_messenger,nullptr);
    }
    logger::log("<konanix> Debug messenger destroyed!",logger::dbg);
#endif
    if (globals::descriptor::layout != nullptr) {
        vkDestroyDescriptorSetLayout(globals::device::device,globals::descriptor::layout,nullptr);
        logger::log("<konanix> Descriptor set destroyed!",logger::dbg);
    }

    if (globals::descriptor::pool != nullptr) {
        vkDestroyDescriptorPool(globals::device::device,globals::descriptor::pool,nullptr);
        logger::log("<konanix> Descriptor pool destroyed!",logger::dbg);
    }

    if (globals::descriptor::layout != nullptr) {
        vkDestroyDescriptorSetLayout(globals::device::device,globals::descriptor::material::layout,nullptr);
        logger::log("<konanix> Material descriptor set destroyed!",logger::dbg);
    }

    if (globals::descriptor::pool != nullptr) {
        vkDestroyDescriptorPool(globals::device::device,globals::descriptor::material::pool,nullptr);
        logger::log("<konanix> Material descriptor pool destroyed!",logger::dbg);
    }
    
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (globals::sync::image_available_semaphores[i]) {
            vkDestroySemaphore(globals::device::device,globals::sync::image_available_semaphores[i],nullptr);
            logger::log("<konanix> "+std::to_string(i+1)+"/"+std::to_string(MAX_FRAMES_IN_FLIGHT)+" semaphores destroyed!",logger::dbg);
        } else { logger::log("<konanix> Could not destroy semaphore \"globals::sync::image_available_semaphores["+std::to_string(i)+"]\"!",logger::wrn); }

        if (globals::sync::in_flight_fences[i]) {
            vkDestroyFence(globals::device::device,globals::sync::in_flight_fences[i],nullptr);
            logger::log("<konanix> "+std::to_string(i+1)+"/"+std::to_string(MAX_FRAMES_IN_FLIGHT)+" fences destroyed!",logger::dbg);
        } else { logger::log("<konanix> Could not destroy fence \"globals::sync::in_flight_fences["+std::to_string(i)+"]\"!",logger::wrn); }
    }

    for (size_t i = 0; i < globals::swapchain::images.size(); ++i)        
        if (globals::sync::render_finished_semaphores[i]) {
            vkDestroySemaphore(globals::device::device,globals::sync::render_finished_semaphores[i],nullptr);
            logger::log("<konanix> "+std::to_string(i+1)+"/"+std::to_string(globals::swapchain::images.size())+" render finished semaphores destroyed!",logger::dbg);
        } else { logger::log("<konanix> Could not destroy semaphore \"globals::sync::render_finished_semaphores["+std::to_string(i)+"]\"!",logger::wrn); }



    if (globals::command::pool) {
        vkDestroyCommandPool(globals::device::device,globals::command::pool,nullptr);
        logger::log("<konanix> Command pool destroyed!",logger::dbg);
    }

    for (VkFramebuffer &fb : globals::swapchain::framebuffers)
        vkDestroyFramebuffer(globals::device::device,fb,nullptr);
    
    logger::log("<konanix> globals::swapchain::framebuffers cleaned up!",logger::dbg);

    if (globals::pipeline::graphics_pipeline) {
        vkDestroyPipeline(globals::device::device,globals::pipeline::graphics_pipeline,nullptr);
        logger::log("<konanix> Graphics pipeline destroyed!",logger::dbg);
    }

    if (globals::pipeline::graphics_pipeline_layout) {
        vkDestroyPipelineLayout(globals::device::device,globals::pipeline::graphics_pipeline_layout,nullptr);
        logger::log("<konanix> Pipeline layout destroyed!",logger::dbg);
    }

    if (globals::pipeline::line_pipeline) {
        vkDestroyPipeline(globals::device::device,globals::pipeline::line_pipeline,nullptr);
        logger::log("<konanix> Line pipeline destroyed!",logger::dbg);
    }

    if (globals::pipeline::line_pipeline_layout) {
        vkDestroyPipelineLayout(globals::device::device,globals::pipeline::line_pipeline_layout,nullptr);
        logger::log("<konanix> Line pipeline layout destroyed!",logger::dbg);
    }

    if (globals::pipeline::renderpass) {
        vkDestroyRenderPass(globals::device::device,globals::pipeline::renderpass,nullptr);
        logger::log("<konanix> Render Pass destroyed!",logger::dbg);
    }


//    for (size_t i = 0; i < globals::swapchain::images.size(); ++i) {
//        if (globals::swapchain::images[i]) {
//            vkDestroyImage(globals::device::device,globals::swapchain::images[i],globals::allocator);
//            logger::log("<konanix> Swapchain image "+std::to_string(i)+"/"+std::to_string(globals::swapchain::images.size()-1)+" destroyed!",logger::dbg);
//        }
//    }

    for (size_t i = 0; i < globals::swapchain::image_views.size(); ++i) {
        if (globals::swapchain::image_views[i]) {
            vkDestroyImageView(globals::device::device,globals::swapchain::image_views[i],globals::allocator);
            logger::log("<konanix> Swapchain image view "+std::to_string(i+1)+"/"+std::to_string(globals::swapchain::image_views.size())+" destroyed!",logger::dbg);
        }
    }

    if (globals::swapchain::swapchain) {
        vkDestroySwapchainKHR(globals::device::device,globals::swapchain::swapchain,nullptr);
        logger::log("<konanix> Swap chain destroyed successfully!",logger::dbg);
    }

    if (g_gif_sampler) {
       vkDestroySampler(globals::device::device,g_gif_sampler,globals::allocator);
       logger::log("<konanix> GIF sampler destroyed!",logger::dbg);
    }

    logger::log("<konanix> Cleaning "+std::to_string(gif_frames.size())+" frames...",logger::dbg);
    for (size_t i = 0; i < gif_frames.size(); ++i) {
            
        if (gif_frames[i].image) {
            vkDestroyImage(globals::device::device,gif_frames[i].image,globals::allocator);
            // logger::log("<konanix> GIF image "+std::to_string(i)+"/"+std::to_string(gif_frames.size())+" destroyed!",logger::dbg);
        }
    
        if (gif_frames[i].image_view) {
            vkDestroyImageView(globals::device::device,gif_frames[i].image_view,globals::allocator);
            // logger::log("<konanix> GIF image view "+std::to_string(i)+"/"+std::to_string(gif_frames.size())+" destroyed!",logger::dbg);
        }
    
        vkFreeMemory(globals::device::device,gif_frames[i].image_memory,globals::allocator);
        // logger::log("<konanix> GIF image memory "+std::to_string(i)+"/"+std::to_string(gif_frames.size())+" freed up!",logger::dbg);

    }
    logger::log("<konanix> GIF frames cleaned up!",logger::dbg);

    if (globals::device::device) {
        vkDestroyDevice(globals::device::device,nullptr);
        globals::device::device = nullptr;
        logger::log("<konanix> Device instance destroyed successfully!",logger::dbg);
    }

    if (globals::surface) {
        vkDestroySurfaceKHR(globals::instance, globals::surface, nullptr);
        globals::surface = nullptr;
        logger::log("<konanix> Surface destroyed successfully!",logger::dbg);
    }

    if (globals::instance) {
        vkDestroyInstance(globals::instance, nullptr);
        globals::instance = nullptr;
        logger::log("<konanix> Instance destroyed successfully!",logger::dbg);
    }

    if (globals::window) {
        glfwDestroyWindow(globals::window);
        globals::window = nullptr;
    }
    glfwTerminate();
    logger::log("<konanix> Vulkan cleaned up!",logger::dbg);
}

// ---------------------------------------------------------------------------
// command buffer helpers
// ---------------------------------------------------------------------------


void konanix::record_command_buffer(VkCommandBuffer commandbuffer, uint32_t image_index) {
    VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pInheritanceInfo = nullptr
    };

    if (vkBeginCommandBuffer(commandbuffer, &begin_info) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer");
    }

    constexpr VkClearValue clear_color = {{{0.0f, 0.0f, 0.0f, 0.0f}}};

    const VkRenderPassBeginInfo renderpass_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        
        .renderPass = globals::pipeline::renderpass,
        .framebuffer = globals::swapchain::framebuffers[image_index],
        .renderArea = {
            .offset = {0, 0},
            .extent = globals::swapchain::extent
        },
        .clearValueCount = 1,
        .pClearValues = &clear_color
    };

    vkCmdBeginRenderPass(commandbuffer, &renderpass_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, globals::pipeline::graphics_pipeline);

    const VkViewport viewport{
        .x = 0.0f, .y = 0.0f,
        .width = static_cast<float>(globals::swapchain::extent.width),
        .height = static_cast<float>(globals::swapchain::extent.height),
        .minDepth = 0.0f, .maxDepth = 1.0f
    };
    vkCmdSetViewport(commandbuffer, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, globals::swapchain::extent};
    vkCmdSetScissor(commandbuffer, 0, 1, &scissor);

    vkCmdBindDescriptorSets(
        commandbuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        globals::pipeline::graphics_pipeline_layout,
        0,
        1,
        &globals::descriptor::sets[current_frame],
        0,
        nullptr
    );

    vkCmdDraw(commandbuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandbuffer);

    if (vkEndCommandBuffer(commandbuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to record command buffer");
    }
}

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------

void konanix::draw_frame() {
    // width = w; height = h;
    // glfwSetWindowSize(g_window,width,height);
    vkWaitForFences(globals::device::device,1,&globals::sync::in_flight_fences[current_frame],VK_TRUE,UINT64_MAX);
    // vkResetFences(globals::device::device,1,&g_in_flight_fences[current_frame]);
    
    if (g_swapchain_rebuild) {
        g_swapchain_rebuild = false; 
        recreate_swap_chain();
        return;
    }

    glfwGetWindowSize(globals::window,&globals::width,&globals::height);
    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(globals::device::device, globals::swapchain::swapchain, UINT64_MAX, globals::sync::image_available_semaphores[current_frame], nullptr, &image_index);
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR || g_swapchain_rebuild) {
        recreate_swap_chain();
        return;
    } 

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        logger::log("<konanix> Failed to acquire swapchain image!",logger::exc);
        throw std::runtime_error("failed to acquire swapchain image");
    }

    // if (glfwGetKey(globals::window, 32) == GLFW_PRESS) glfwSetWindowShouldClose(g_window,GLFW_TRUE);

    if (globals::sync::images_in_flight[image_index] != VK_NULL_HANDLE) {
        vkWaitForFences(globals::device::device,1,&globals::sync::images_in_flight[image_index],VK_TRUE,UINT64_MAX);
    }

    globals::sync::images_in_flight[image_index] = globals::sync::in_flight_fences[current_frame];

    vkResetFences(globals::device::device,1,&globals::sync::in_flight_fences[current_frame]);

    vkResetCommandBuffer(globals::command::buffers[current_frame],0);

    std::chrono::time_point<std::chrono::high_resolution_clock, std::chrono::duration<long, std::ratio<1,1000000000>>> time_now = std::chrono::high_resolution_clock::now();
    globals::time::delta_time = std::chrono::duration<float>(time_now - globals::time::last_frame).count();
    globals::time::last_frame = time_now;
    globals::time::elapsed = std::chrono::duration<float>(time_now - globals::time::start).count();
   
    set_gif_frame(current_gif_frame);

    

    logger::log("<konanix> Rendering GIF frame #"+std::to_string(current_gif_frame),logger::dbg);

    record_command_buffer(globals::command::buffers[current_frame], image_index);
    
    // const VkSemaphore wait_semaphores[] = {globals::sync::image_available_semaphores[current_frame]};
    const VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    // const VkSemaphore signal_semaphores[] = {globals::sync::render_finished_semaphores[current_frame]};

    const VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = VK_NULL_HANDLE,

        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &globals::sync::image_available_semaphores[current_frame],
        .pWaitDstStageMask = wait_stages,

        .commandBufferCount = 1,
        .pCommandBuffers = &globals::command::buffers[current_frame],

        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &globals::sync::render_finished_semaphores[image_index]
    };
    if (vkQueueSubmit(globals::device::graphics_queue,1,&submit_info,globals::sync::in_flight_fences[current_frame]) != VK_SUCCESS) {
        logger::log("<konanix> Failed to submit draw command buffer!",logger::exc);
        throw std::runtime_error("failed to submit draw command buffer");
    } 
   
    VkSwapchainKHR swapchains[] = {globals::swapchain::swapchain};

    const VkPresentInfoKHR present_info {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = VK_NULL_HANDLE,

        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &globals::sync::render_finished_semaphores[image_index],

        .swapchainCount = 1,
        .pSwapchains = swapchains,

        .pImageIndices = &image_index,

        .pResults = nullptr
    };
    vkQueuePresentKHR(globals::device::present_queue,&present_info);

    //if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    //    recreate_swap_chain();
    //} else if (result != VK_SUCCESS) {
    //    logger::log("Failed to present the swapchain image!",logger::exc);
    //    throw std::runtime_error("failed to present swapchain image");
    //}
    current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    current_gif_frame = (current_gif_frame + 1) % GIF_FRAME_COUNT;
    // logger::log("Current frame: "+std::to_string(current_frame));

};

void konanix::render() {

    active_frame.image = gif_frames[0].image;
    active_frame.image_view = gif_frames[0].image_view;

    const VkDescriptorImageInfo image_info{
        g_gif_sampler,
        active_frame.image_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    const VkWriteDescriptorSet write {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = globals::descriptor::sets[current_frame],
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info
    };

    vkUpdateDescriptorSets(globals::device::device, 1, &write, 0, nullptr);

    // std::vector<int> frame_delays_ms(g_raw_anim_data.frames.size());
    int delay_ms = g_raw_anim_data.frames[0].delay_ms;
    konanix::g_raw_anim_data.frames.clear();
    while (!glfwWindowShouldClose(konanix::globals::window)) {
        // std::this_thread::sleep_for(std::chrono::milliseconds(konanix::g_raw_anim_data.frames[/*frame_index*/0].delay_ms));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        // set_gif_frame(current_gif_frame);
        // active_frame = gif_frames[current_gif_frame];
        //
        // overlay::storage.assign(size_t(globals::width) * size_t(globals::height) * 4, 0);
        // overlay::rgba = overlay::storage.data();
        // overlay::clear();
        // std::vector<uint8_t> overlay_buffer;
        // overlay_buffer.assign(size_t(globals::width) * size_t(globals::height) * 4, 0);
        // overlay::rgba = overlay_buffer.data();
        // overlay::clear();
        //
        // // const auto& src = konanix::g_raw_anim_data.frames[current_gif_frame].rgba;
        // // const size_t copy_bytes = std::min(overlay_buffer.size(), src.size());
        // // memcpy(overlay_buffer.data(), src.data(), copy_bytes);
        // // overlay::rgba = overlay_buffer.data();
        // //
        // konanix::draw_context_menu();
        // konanix::upload_rgba_frame_to_gif_image(overlay::rgba, overlay_buffer.size());
        konanix::draw_frame();
        glfwPollEvents();
    }



}

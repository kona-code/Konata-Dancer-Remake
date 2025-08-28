// most initialization code was taken from: https://github.com/ocornut/imgui/

#include "Interface.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <imgui.h>
#include <imgui_internal.h>
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl2.h"
#include <linux/limits.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_shape.h>
#include <SDL2/SDL_vulkan.h>
#include <thread>
#include <vector>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>
#include <array>
#include <limits.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h> // for texture (image) loading
#include <sys/stat.h>
#include <chrono>
#include <algorithm>
#include <string>

#define USING_WAYLAND true

#ifdef USING_WAYLAND
#include <SDL2/SDL_syswm.h>   // SDL_GetWindowWMInfo
#include <wayland-client.h>   // low-level wayland input-region manipulation (remove if you don't run wayland)
#endif

extern "C" {
#include "gifdec.h" // from https://github.com/lecram/gifdec
}

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

//#define _DEBUG
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;       
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;
static VkCommandPool            g_CommandPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
SDL_Window*                     window;
std::atomic<bool>               g_RenderPaused{false};
static uint32_t                 g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

// TextureData struct code from https://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples
struct TextureData {
    VkDescriptorSet DS;         // descriptor set
    int             width;
    int             height;
    int             Channels;

    // for proper cleanup
    VkImageView     ImageView;
    VkImage         Image;
    VkDeviceMemory  ImageMemory;
    VkSampler       Sampler;
    VkBuffer        UploadBuffer;
    VkDeviceMemory  UploadBufferMemory;

    TextureData() { memset(this, 0, sizeof(*this)); }
};

struct GifFrame {
    TextureData tex;
    int delay_ms;
    std::vector<uint8_t> rgba;
};

TextureData texture; // store konata's texture data

static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) 
        return;
    fprintf(stderr, "[ERROR] (Vulkan) VkResult = %d\n", err);
    std::cout << "\n\n>────────────[EXCEPTION]────────────<\n\n[ERROR] (Vulkan) VkResult failed!" << std::endl;
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData) {
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix;
    fprintf(stderr, "[INFO] (Vulkan) Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif

// vulkan helpers
uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0xFFFFFFFF;
}

VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void endSingleTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat format,
                           VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    barrier.srcAccessMask = 0; // TODO: set based on oldLayout
    barrier.dstAccessMask = 0; // TODO: set based on newLayout

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        std::cout << "\n\n>────────────[EXCEPTION]────────────<\n\n[ERROR] (Vulkan) transitionImageLayout: Unsupported layout transition" << std::endl;
        throw std::invalid_argument("[ERROR] Unsupported layout transition!");
    }

    vkCmdPipelineBarrier(
        cmd,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

void createBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size,
                  VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                  VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        std::cout << "\n\n>────────────[EXCEPTION]────────────<\n\n[ERROR] (Vulkan) createBuffer: Failed to create buffer!" << std::endl;
        throw std::runtime_error("[ERROR] Failed to create buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physDevice, memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        std::cout << "\n\n>────────────[EXCEPTION]────────────<\n\n[ERROR] (Vulkan) vkGetBufferMemoryRequirements: Failed to allocate buffer memory!" << std::endl;
        throw std::runtime_error("[ERROR] Failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}
//
static void CreateCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                              | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = g_QueueFamily;   // same family as your graphics queue

    if (vkCreateCommandPool(g_Device, &poolInfo, nullptr, &g_CommandPool) != VK_SUCCESS) {
        std::cout << "\n\n>────────────[EXCEPTION]────────────<\n\n[ERROR] (Vulkan) CreateCommandPool: Failed to create command pool!" << std::endl;
        throw std::runtime_error("[ERROR] Failed to create command pool!");
    }
}

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension) {
    for (const VkExtensionProperties& p : properties) {\
        if (strcmp(p.extensionName, extension)==0) 
            return true;
    }
    return false;
}

static bool IsLayerAvailable(const char* layer_name) {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    if (layer_count)
        vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    for (auto &lp : layers)
        if (strcmp(lp.layerName, layer_name) == 0) return true;
    return false;
}

static bool IsInstanceExtensionAvailable(const char* extension) {
    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    if (ext_count)
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data());
    for (auto &e : exts)
        if (strcmp(e.extensionName, extension) == 0) return true;
    return false;
}

static void VkSetup(ImVector<const char*> instance_extensions) {
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
volkInitialize();
#endif
    {  
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);        
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);
    
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = {"VK_LAYER_KRONOS_validation"};
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(g_Instance);
#endif

    if (IsInstanceExtensionAvailable(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (IsInstanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
#endif


#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif

    }

    // select physical device
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);
    // create logical device
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // enumerate physical device extension
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
        CreateCommandPool();
        }

    // descriptor pool
    {
        // keep manual - avoid "out of pool" errors
        VkDescriptorPoolSize pool_sizes[] = {
            { VK_DESCRIPTOR_TYPE_SAMPLER,                1000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,   1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,       1000 }
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
        pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

static void SetupVkWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) {
    wd->Surface = surface;

    // check for WSI support
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) {
        fprintf(stderr, "[ERROR] (Vulkan) No WSI support on physical device 0\n");
        std::cout << "\n\n>────────────[EXCEPTION]────────────<\n\n[ERROR] (Vulkan) No WSI support on physical device 0" << std::endl;
        exit(-1);
    }
    // VkSurfaceCapabilitiesKHR surfCaps;
    // vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice, wd->Surface, &surfCaps);

    // select surface format
    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // select present mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
    //printf("[INFO] (Vulkan) Selected PresentMode = %d\n", wd->PresentMode);

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
    wd->ClearValue.color.float32[0] = 0.03f; // r
    wd->ClearValue.color.float32[1] = 0.03f; // g
    wd->ClearValue.color.float32[2] = 0.03f; // b
    wd->ClearValue.color.float32[3] = 0.0f;  // a (transparency)

    // debug output
    VkSurfaceCapabilitiesKHR caps; 
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice, wd->Surface, &caps);
    std::cout << "[INFO] (Vulkan) surfaceFormat = " << wd->SurfaceFormat.format <<", supportedCompositeAlpha = " << caps.supportedCompositeAlpha << std::endl;

}

static void VkCleanup() {
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // remove the debug report callback
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVkWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // record imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// create a VkSampler
VkSampler CreateFontSampler(VkDevice device) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.maxAnisotropy = 1.0f;
    VkSampler sampler;
    vkCreateSampler(device, &info, nullptr, &sampler);
    return sampler;
}

// create a VkImage + allocate & bind memory
VkImage CreateFontImage(VkDevice device, VkPhysicalDevice phys, int w, int h, VkDeviceMemory& outMemory) {
    VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgInfo.imageType   = VK_IMAGE_TYPE_2D;
    imgInfo.format      = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent      = { (uint32_t)w, (uint32_t)h, 1 };
    imgInfo.mipLevels   = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image;
    vkCreateImage(device, &imgInfo, nullptr, &image);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = req.size;
    // findMemoryType is your helper to pick a memory type index with VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    allocInfo.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkAllocateMemory(device, &allocInfo, nullptr, &outMemory);
    vkBindImageMemory(device, image, outMemory, 0);
    return image;
}

// create a VkImageView
VkImageView CreateFontImageView(VkDevice device, VkImage image) {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1, 0,1 };
    VkImageView view;
    vkCreateImageView(device, &viewInfo, nullptr, &view);
    return view;
}

void UploadFontPixels(VkDevice device, VkPhysicalDevice physDevice,
                      VkCommandPool cmdPool, VkQueue queue,
                      VkImage image, unsigned char* pixels, int width, int height) {
    // create a staging buffer
    VkDeviceSize imageSize = uint64_t(width) * height * 4; // RGBA8
    
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    createBuffer(device, physDevice,
                 imageSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    // copy pixel data into the buffer
    void* mapped;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
    memcpy(mapped, pixels, size_t(imageSize));
    vkUnmapMemory(device, stagingMemory);

    // record commands to transition image and copy buffer→image
    VkCommandBuffer cmd = beginSingleTimeCommands(device, cmdPool);

    //undef > transfer‐dst
    transitionImageLayout(cmd, image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // copy
    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = { 0, 0, 0 };
    region.imageExtent                     = { uint32_t(width), uint32_t(height), 1 };
    vkCmdCopyBufferToImage(cmd,
                           stagingBuffer,
                           image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);

    // transfer‐dst > shader‐read
    transitionImageLayout(cmd, image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    endSingleTimeCommands(device, cmdPool, queue, cmd);

    // cleanup staging resources
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
}

std::array<float,4> ImVec4ToFloats(const ImVec4 &c) {
    return { c.x, c.y, c.z, c.w };
}
inline ImVec4 FloatsToImVec4(const std::array<float,4>& a) noexcept {
    return ImVec4(a[0], a[1], a[2], a[3]);
}

static int Shutdown(VkSurfaceKHR g_Surface) {

        // wait for device idle if available
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }

        // shutdown ImGui renderer & platform backends (renderer first)
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();

        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }

#ifdef IMGUI_IMPL_VULKANH_HEADERS_AVAILABLE
        if (g_Instance != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
            ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
        }
#endif

        // destroy Vulkan objects
        if (g_CommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
            g_CommandPool = VK_NULL_HANDLE;
        }

        if (g_DescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
            g_DescriptorPool = VK_NULL_HANDLE;
        }

        if (g_PipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(g_Device, g_PipelineCache, g_Allocator);
            g_PipelineCache = VK_NULL_HANDLE;
        }

        if (g_Device != VK_NULL_HANDLE) {
            vkDestroyDevice(g_Device,g_Allocator);
            g_Device = VK_NULL_HANDLE;
        }

        if (g_Instance != VK_NULL_HANDLE) {
            vkDestroyInstance(g_Instance, g_Allocator);
            g_Instance = VK_NULL_HANDLE;
        }

        if (g_Surface != VK_NULL_HANDLE && g_Instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(g_Instance, g_Surface, g_Allocator);
            g_Surface = VK_NULL_HANDLE;
        }

        // clear globals
        g_PhysicalDevice = VK_NULL_HANDLE;
        g_Queue = VK_NULL_HANDLE;
        g_QueueFamily = (uint32_t)-1;
        g_MinImageCount = 2;
        g_SwapChainRebuild = false;

        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
        return 0;
}

void Interface::Minimize() {
    if (!window) return;
    // stop rendering loop
    g_RenderPaused.store(true, std::memory_order_release);

    // hide or minimize the window
    SDL_HideWindow(window);
    //SDL_MinimizeWindow(g_Window); // minimize to taskbar - scrapped (will not work on some WMs)
}

void Interface::Show() {
    if (!window) return;

    // show and bring front
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    SDL_RestoreWindow(window);

    // may need to recreate swapchain if window size changed while hidden

    g_RenderPaused.store(false, std::memory_order_release);
}

uint32_t findTextureMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {

    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++)
        if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;

    return 0xFFFFFFFF; // unable to find memoryType
}

// LoadTextureFromFile code from: https://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples
static bool LoadTextureFromFile(const char* filename, TextureData* tex_data) {
    // specifying 4 channels forces stb to load the image in RGBA which is an easy format for Vulkan
    tex_data->Channels = 4;
    unsigned char* image_data = stbi_load(filename, &tex_data->width, &tex_data->height, 0, tex_data->Channels);

    if (image_data == NULL)
        return false;

    // calculate allocation size (in number of bytes)
    size_t image_size = tex_data->width * tex_data->height * tex_data->Channels;

    VkResult err;

    // Create the Vulkan image.
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent.width = tex_data->width;
        info.extent.height = tex_data->height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        err = vkCreateImage(g_Device, &info, g_Allocator, &tex_data->Image);
        check_vk_result(err);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_Device, tex_data->Image, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        err = vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &tex_data->ImageMemory);
        check_vk_result(err);
        err = vkBindImageMemory(g_Device, tex_data->Image, tex_data->ImageMemory, 0);
        check_vk_result(err);
    }

    // Create the Image View
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = tex_data->Image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(g_Device, &info, g_Allocator, &tex_data->ImageView);
        check_vk_result(err);
    }

    // Create Sampler
    {
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; // outside image bounds just use border color
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.minLod = -1000;
        sampler_info.maxLod = 1000;
        sampler_info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(g_Device, &sampler_info, g_Allocator, &tex_data->Sampler);
        check_vk_result(err);
    }

    // Create Descriptor Set using ImGUI's implementation
    tex_data->DS = ImGui_ImplVulkan_AddTexture(tex_data->Sampler, tex_data->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Create Upload Buffer
    {
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = image_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        err = vkCreateBuffer(g_Device, &buffer_info, g_Allocator, &tex_data->UploadBuffer);
        check_vk_result(err);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_Device, tex_data->UploadBuffer, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        err = vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &tex_data->UploadBufferMemory);
        check_vk_result(err);
        err = vkBindBufferMemory(g_Device, tex_data->UploadBuffer, tex_data->UploadBufferMemory, 0);
        check_vk_result(err);
    }

    // Upload to Buffer:
    {
        void* map = NULL;
        err = vkMapMemory(g_Device, tex_data->UploadBufferMemory, 0, image_size, 0, &map);
        check_vk_result(err);
        memcpy(map, image_data, image_size);
        VkMappedMemoryRange range[1] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = tex_data->UploadBufferMemory;
        range[0].size = image_size;
        err = vkFlushMappedMemoryRanges(g_Device, 1, range);
        check_vk_result(err);
        vkUnmapMemory(g_Device, tex_data->UploadBufferMemory);
    }

    // Release image memory using stb
    stbi_image_free(image_data);

    // Create a command buffer that will perform following steps when hit in the command queue.
    // TODO: this works in the example, but may need input if this is an acceptable way to access the pool/create the command buffer.
    VkCommandPool command_pool = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandPool;
    VkCommandBuffer command_buffer;
    {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool = command_pool;
        alloc_info.commandBufferCount = 1;

        err = vkAllocateCommandBuffers(g_Device, &alloc_info, &command_buffer);
        check_vk_result(err);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(command_buffer, &begin_info);
        check_vk_result(err);
    }

    // Copy to Image
    {
        VkImageMemoryBarrier copy_barrier[1] = {};
        copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].image = tex_data->Image;
        copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_barrier[0].subresourceRange.levelCount = 1;
        copy_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, copy_barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = tex_data->width;
        region.imageExtent.height = tex_data->height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command_buffer, tex_data->UploadBuffer, tex_data->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier use_barrier[1] = {};
        use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].image = tex_data->Image;
        use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        use_barrier[0].subresourceRange.levelCount = 1;
        use_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, use_barrier);
    }

    // End command buffer
    {
        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;
        err = vkEndCommandBuffer(command_buffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &end_info, VK_NULL_HANDLE);
        check_vk_result(err);
        err = vkDeviceWaitIdle(g_Device);
        check_vk_result(err);
    }

    return true;
}


bool LoadTextureFromMemoryRGBA(const uint8_t* pixels, int width, int height, TextureData* out) {
    // create image
    out->width = width;
    out->height = height;
    out->Channels = 4;
    size_t image_size = (size_t)width * (size_t)height * 4;

    VkResult err;
    // image
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent.width = (uint32_t)width;
    info.extent.height = (uint32_t)height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    err = vkCreateImage(g_Device, &info, g_Allocator, &out->Image);
    check_vk_result(err);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_Device, out->Image, &req);
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    err = vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &out->ImageMemory);
    check_vk_result(err);
    err = vkBindImageMemory(g_Device, out->Image, out->ImageMemory, 0);
    check_vk_result(err);

    // image view
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = out->Image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &out->ImageView);
    check_vk_result(err);

    // sampler
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    sampler_info.maxAnisotropy = 1.0f;
    err = vkCreateSampler(g_Device, &sampler_info, g_Allocator, &out->Sampler);
    check_vk_result(err);

    // descriptor
    out->DS = ImGui_ImplVulkan_AddTexture(out->Sampler, out->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // create upload buffer
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = image_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    err = vkCreateBuffer(g_Device, &buffer_info, g_Allocator, &out->UploadBuffer);
    check_vk_result(err);
    vkGetBufferMemoryRequirements(g_Device, out->UploadBuffer, &req);
    VkMemoryAllocateInfo buf_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buf_alloc.allocationSize = req.size;
    buf_alloc.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    err = vkAllocateMemory(g_Device, &buf_alloc, g_Allocator, &out->UploadBufferMemory);
    check_vk_result(err);
    err = vkBindBufferMemory(g_Device, out->UploadBuffer, out->UploadBufferMemory, 0);
    check_vk_result(err);

    // copy pixels into mapped staging buffer
    void* mapped = nullptr;
    err = vkMapMemory(g_Device, out->UploadBufferMemory, 0, image_size, 0, &mapped);
    check_vk_result(err);
    memcpy(mapped, pixels, image_size);
    vkUnmapMemory(g_Device, out->UploadBufferMemory);

    // copy buffer -> image with a command buffer
    VkCommandBuffer cmd;
    {
        VkCommandBufferAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandPool;
        alloc_info.commandBufferCount = 1;
        err = vkAllocateCommandBuffers(g_Device, &alloc_info, &cmd);
        check_vk_result(err);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(cmd, &bi);
        check_vk_result(err);
    }

    // transition -> transfer dst
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = out->Image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    // copy
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
    vkCmdCopyBufferToImage(cmd, out->UploadBuffer, out->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // transition -> shader read
    VkImageMemoryBarrier use_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    use_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    use_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    use_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    use_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    use_barrier.image = out->Image;
    use_barrier.subresourceRange = barrier.subresourceRange;
    use_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    use_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &use_barrier);

    // submit & wait
    err = vkEndCommandBuffer(cmd);
    check_vk_result(err);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    err = vkQueueSubmit(g_Queue, 1, &si, VK_NULL_HANDLE);
    check_vk_result(err);
    err = vkQueueWaitIdle(g_Queue);
    check_vk_result(err);

    // free temporary command buffer
    vkFreeCommandBuffers(g_Device, g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandPool, 1, &cmd);

    return true;
}

void RemoveTexture(TextureData* tex_data) {
    vkFreeMemory(g_Device, tex_data->UploadBufferMemory, nullptr);
    vkDestroyBuffer(g_Device, tex_data->UploadBuffer, nullptr);
    vkDestroySampler(g_Device, tex_data->Sampler, nullptr);
    vkDestroyImageView(g_Device, tex_data->ImageView, nullptr);
    vkDestroyImage(g_Device, tex_data->Image, nullptr);
    vkFreeMemory(g_Device, tex_data->ImageMemory, nullptr);
    ImGui_ImplVulkan_RemoveTexture(tex_data->DS);
}

bool LoadGifAsFrames(const char* gif_path, std::vector<GifFrame>& out_frames) {
    gd_GIF *gif = gd_open_gif(gif_path);
    if (!gif) return false;

    // allocate rgb canvas
    const int canvas_w = gif->width;
    const int canvas_h = gif->height;
    std::vector<uint8_t> rgbbuf((size_t)canvas_w * canvas_h * 3);

    // iterate all frames
    for (;;) {
        int ret = gd_get_frame(gif);
        if (ret == 0) break; // no more frames
        // render full canvas (rgb)
        gd_render_frame(gif, rgbbuf.data()); // writes width*height*3 bytes

        // convert rgb -> rgba
        std::vector<uint8_t> rgbato((size_t)canvas_w * canvas_h * 4);
        uint8_t* in = rgbbuf.data();
        uint8_t* out = rgbato.data();
        for (int y = 0; y < canvas_h; ++y) {
            for (int x = 0; x < canvas_w; ++x) {
                // color pointer for pixel
                uint8_t r = *in++;
                uint8_t g = *in++;
                uint8_t b = *in++;
                uint8_t a = 255;
                
                if (gd_is_bgcolor(gif, (uint8_t[]){r,g,b}) ) { // note: prepare temporary array
                    a = 0;
                }
                *out++ = r;
                *out++ = g;
                *out++ = b;
                *out++ = a;
            }
        }

        // upload
        GifFrame gf{};
        gf.delay_ms = (gif->gce.delay > 0) ? (gif->gce.delay * 10) : 100; // gifdec stores hundredths
        if (!LoadTextureFromMemoryRGBA(rgbato.data(), canvas_w, canvas_h, &gf.tex)) {
            // cleanup previous frames
            for (auto &f : out_frames) RemoveTexture(&f.tex);
            gd_close_gif(gif);
            return false;
        }

        out_frames.push_back(std::move(gf));
    }

    // detect loop count
    gd_close_gif(gif);
    return !out_frames.empty();
}

static int ResizeCallback(ImGuiInputTextCallbackData* data) { // for string usage inside ImGui::InputText()
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        // user-data is the std::string pointer
        std::string* str = (std::string*)data->UserData;
        str->resize(data->BufTextLen); // resize string to new text length (not counting null)
        data->Buf = const_cast<char*>(str->c_str()); // update ImGui's buffer pointer
    }
    return 0;
}

static bool IsRunningWayland() {
    const char* xdg = getenv("XDG_SESSION_TYPE");
    if (xdg && strcmp(xdg, "wayland") == 0) return true;
    if (getenv("WAYLAND_DISPLAY")) return true;
    return false;
}

void UpdateWindowShapeFromRGBA(SDL_Window* win, const uint8_t* pixels, int width, int height, uint8_t alphaThreshold = 1) {
    if (!win) return;
    if (!pixels) {
        std::cerr << "[ERROR] UpdateWindowShapeFromRGBA: pixels == nullptr" << std::endl;
        return;
    }
    // check shaped-window support for this window
    if (!SDL_IsShapedWindow(win)) {
        return;
    }
    // create an SDL surface
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        std::cerr << "[ERROR] SDL_CreateRGBSurfaceWithFormat failed: \n[ERROR] " << SDL_GetError() << std::endl;
        return;
    }
    // copy incoming rgba pixels into the surface
    if (SDL_LockSurface(surf) != 0) {
        std::cerr << "[ERROR] SDL_LockSurface failed: \n[ERROR]" << SDL_GetError() << std::endl;
        SDL_FreeSurface(surf);
        return;
    }

    // pitch is surf->pitch bytes per row, data packed width*4
    // copy row-by-row in case pitch != width*4
    const int src_pitch = width * 4;
    uint8_t* dst = (uint8_t*)surf->pixels;
    const uint8_t* src = pixels;
    for (int y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * surf->pitch, src + (size_t)y * src_pitch, (size_t)src_pitch);
    }

    SDL_UnlockSurface(surf);
    SDL_WindowShapeMode shapeMode;
    shapeMode.mode = ShapeModeBinarizeAlpha;
    shapeMode.parameters.binarizationCutoff = alphaThreshold;

    if (SDL_SetWindowShape(win, surf, &shapeMode) != 0) {
        fprintf(stderr, "[WARN] SDL_SetWindowShape failed: %s\n", SDL_GetError());
    }

    SDL_FreeSurface(surf);
}

static void CopyPremultipliedToStagingAndUpload(const uint8_t* pixels, size_t image_size,
                                                VkDeviceMemory stagingMemory, VkDevice device) {

    // create temporary buffer with premultiplied pixels
    uint8_t* prem = (uint8_t*)malloc(image_size);
    if (!prem) {
        std::cerr << "[ERROR] Ran out of memory for premultiplied buffer!" << std::endl;
        return;
    }

    // premultiply r = (r * a) / 255
    for (size_t i = 0; i < image_size; i += 4) {
        uint8_t r = pixels[i + 0];
        uint8_t g = pixels[i + 1];
        uint8_t b = pixels[i + 2];
        uint8_t a = pixels[i + 3];
        // if alpha is 255 just copy
        if (a == 255) {
            prem[i + 0] = r;
            prem[i + 1] = g;
            prem[i + 2] = b;
            prem[i + 3] = 255;
        } else if (a == 0) {
            prem[i + 0] = 0;
            prem[i + 1] = 0;
            prem[i + 2] = 0;
            prem[i + 3] = 0;
        } else {
            // multiply with rounding (v*a + 127) / 255
            prem[i + 0] = (uint8_t)((((int)r * (int)a) + 127) / 255);
            prem[i + 1] = (uint8_t)((((int)g * (int)a) + 127) / 255);
            prem[i + 2] = (uint8_t)((((int)b * (int)a) + 127) / 255);
            prem[i + 3] = a;
        }
    }

    // map & copy premultiplied data into staging memory
    void* map = nullptr;
    VkResult err = vkMapMemory(device, stagingMemory, 0, image_size, 0, &map);
    if (err == VK_SUCCESS && map) {
        memcpy(map, prem, image_size);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = stagingMemory;
        range.size = image_size;
        vkFlushMappedMemoryRanges(device, 1, &range);
        vkUnmapMemory(device, stagingMemory);
    } else {
        std::cerr << "[ERROR] vkMapMemory failed while uploading premultiplied pixels:\n[ERROR] " << err << std::endl;
    }

    free(prem);
}

#ifdef USING_WAYLAND
static void premultiply_rgba8(const uint8_t* src, uint8_t* out, size_t pixels_count) { // for wayland sessions
    // pixels_count = w*h
    for (size_t i = 0; i < pixels_count; ++i) {
        const uint8_t r = src[i*4+0];
        const uint8_t g = src[i*4+1];
        const uint8_t b = src[i*4+2];
        const uint8_t a = src[i*4+3];
        if (a == 255) {
            out[i*4+0] = r;
            out[i*4+1] = g;
            out[i*4+2] = b;
            out[i*4+3] = 255;
        } else if (a == 0) {
            out[i*4+0] = 0;
            out[i*4+1] = 0;
            out[i*4+2] = 0;
            out[i*4+3] = 0;
        } else {
            // integer-rounded multiply
            out[i*4+0] = (uint8_t)((((int)r * (int)a) + 127) / 255);
            out[i*4+1] = (uint8_t)((((int)g * (int)a) + 127) / 255);
            out[i*4+2] = (uint8_t)((((int)b * (int)a) + 127) / 255);
            out[i*4+3] = a;
        }
    }
}

static struct wl_compositor* s_wl_compositor = nullptr;

static void registry_global_cb(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0) {
        s_wl_compositor = (struct wl_compositor*)wl_registry_bind(registry, id, &wl_compositor_interface, 1);
    }
}
static void registry_global_remove_cb(void* data, struct wl_registry* registry, uint32_t id) {
    (void)data; (void)registry; (void)id;
}
static const struct wl_registry_listener s_registry_listener = {
    registry_global_cb,
    registry_global_remove_cb
};

static void clickthrough(SDL_Window* window) {
    if (!window) return;

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) {
        fprintf(stderr, "[WARN] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return;
    }
    if (info.subsystem != SDL_SYSWM_WAYLAND) {
        return;
    }

    struct wl_display* display = info.info.wl.display;
    struct wl_surface* surface = info.info.wl.surface;
    if (!display || !surface) {
        fprintf(stderr, "[WARN] wayland display/surface null\n");
        return;
    }

    // bind compositor if not already bound
    struct wl_registry* registry = wl_display_get_registry(display);
    if (!registry) {
        fprintf(stderr, "[WARN] wl_display_get_registry failed\n");
        return;
    }
    wl_registry_add_listener(registry, &s_registry_listener, NULL);
    wl_display_roundtrip(display); // registry callbacks

    if (!s_wl_compositor) {
        fprintf(stderr, "[WARN] could not bind wl_compositor\n");
        wl_registry_destroy(registry);
        return;
    }

    // create empty region (no input/click-through)
    struct wl_region* region = wl_compositor_create_region(s_wl_compositor);
    // no rects (empty region)
    wl_surface_set_input_region(surface, region);
    wl_surface_commit(surface);
    wl_display_flush(display);

    wl_region_destroy(region);
    wl_registry_destroy(registry);
}
#endif

int Interface::Render(std::atomic<bool>* runningFlag) {


    // create window with Vulkan graphics context
    float main_scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!IsRunningWayland()) {
        window = SDL_CreateShapedWindow("Konata Dancer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, window_flags);
        if (window == nullptr) {
            std::cerr << "[ERROR] SDL_CreateShapedWindow failed: " << SDL_GetError() << "\n[INFO] Falling back to normal SDL_CreateWindow()" << std::endl;
            window = SDL_CreateWindow("Konata Dancer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, window_flags);
            if (window == nullptr) {
                std::cerr << "[ERROR] (Vulkan/SDL2) SDL_CreateWindow():\n" << SDL_GetError() << std::endl;
                return -1;
            }
        }
    }
    else {
        std::cout << "[INFO] Renderer will use SDL_CreateWindow() over SDL_CreateShapedWindow() due to Wayland compatibility!" << std::endl;
        window = SDL_CreateWindow("Konata Dancer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, window_flags);
        if (window == nullptr) {
            std::cerr << "[ERROR] (Vulkan/SDL2) SDL_CreateWindow():\n" << SDL_GetError() << std::endl;
            return -1;
        }
    }
    std::cout << "[INFO] (Vulkan/SDL2) Window created successfully!" << std::endl;

    ImVector<const char*> extensions;
    uint32_t extensions_count = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &extensions_count, nullptr);
    extensions.resize(extensions_count);
    SDL_Vulkan_GetInstanceExtensions(window, &extensions_count, extensions.Data);
    VkSetup(extensions);

    // create window surface
    VkSurfaceKHR surface;
    VkResult err;
    if (SDL_Vulkan_CreateSurface(window, g_Instance, &surface) == 0) {
        printf("[ERROR] (Vulkan/SDL2) Failed to create Vulkan/SDL2 surface.\n");
        return 1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) Successfully created Vulkan/SDL2 surface!" << std::endl;

    // create framebuffers
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVkWindow(wd, surface, w, h);
#ifdef USING_WAYLAND
    clickthrough(window);
#endif
    // setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // enable keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // enable gamepad Controls

    // setup ImGui style
    //ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();
    io.FontGlobalScale = main_scale;               // scales all fonts by main_scale
    io.Fonts->Clear();
    io.Fonts->AddFontDefault();
    
    // setup platform/renderer backends
    ImGui_ImplSDL2_InitForVulkan(window);
    
    ImGui_ImplVulkan_InitInfo init_info = {};

    //init_info.ApiVersion = VK_API_VERSION_1_3;              // pass in value of VkApplicationInfo::apiVersion, otherwise will default to header version.
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.Subpass = 0;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    {
        // manual font upload
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels; int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

        // create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        createBuffer(g_Device, g_PhysicalDevice,
                     w * h * 4,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingBufferMemory);
        
        // copy pixel data
        void* data;
        vkMapMemory(g_Device, stagingBufferMemory, 0, VK_WHOLE_SIZE, 0, &data);
        memcpy(data, pixels, (size_t)(w * h * 4));
        vkUnmapMemory(g_Device, stagingBufferMemory);
        
        // create the GPU image
        VkDeviceMemory fontImageMemory;
        VkImage fontImage = CreateFontImage(g_Device, g_PhysicalDevice, w, h, fontImageMemory);
        
        // transition, copy & transition back
        VkCommandBuffer cmd = beginSingleTimeCommands(g_Device, g_CommandPool);
        transitionImageLayout(cmd, fontImage, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{ };
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0,0,1 };
        region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyBufferToImage(cmd, stagingBuffer, fontImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transitionImageLayout(cmd, fontImage, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        endSingleTimeCommands(g_Device, g_CommandPool, g_Queue, cmd);
        
        // cleanup staging
        vkDestroyBuffer(g_Device, stagingBuffer, nullptr);
        vkFreeMemory(g_Device, stagingBufferMemory, nullptr);
        
        // create view & sampler
        VkImageView fontView = CreateFontImageView(g_Device, fontImage);
        VkSampler   fontSampler = CreateFontSampler(g_Device);
        
        // register with ImGui
        VkDescriptorSet desc = ImGui_ImplVulkan_AddTexture(
            fontSampler, fontView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
        
        io.Fonts->SetTexID((ImTextureID)desc);
        io.Fonts->ClearTexData();

    }

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;

    // rendering variables
    bool gifLoaded = false; // also used in loop (keep)
    bool waylandSession = SDL_IsShapedWindow(window);

    // load data into konata's texture variable
    std::vector<GifFrame> gif_frames;
    size_t gif_cur = 0;
    int gif_accum_ms = 0;
    int cur_delay;
    uint64_t last_tick = SDL_GetTicks64();
    if (!LoadGifAsFrames("./konata.gif", gif_frames)) { // temporary path
        // fallback - keep texture empty or load single frame
        std::cerr << "[ERROR] Failed to load gif frames!" << std::endl;
    } else {
        if (!gif_frames.empty()) {
            texture = gif_frames[0].tex; // copies handle values
            gifLoaded = true;
        }
    }
    if (gifLoaded) {
        SDL_SetWindowSize(window, gif_frames[0].tex.width, gif_frames[0].tex.height);
        // apply shape from the first frame
        UpdateWindowShapeFromRGBA(window, gif_frames[0].rgba.data(), gif_frames[0].tex.width, gif_frames[0].tex.height, /*alphaThreshold=*/1);
    }

    SDL_Event event;
    int fb_width, fb_height;
    while (runningFlag->load()) {
        uint64_t now_tick = SDL_GetTicks64();
        int dt_ms = (int)(now_tick - last_tick);
        last_tick = now_tick;
        dt_ms = std::min(dt_ms, 1000);

        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                break;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                break;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        if (g_RenderPaused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // resize swap chain?
        SDL_GetWindowSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }

        gif_accum_ms += dt_ms; // load GIF frames
        if (gifLoaded) {
            cur_delay = gif_frames[gif_cur].delay_ms;
            if (cur_delay <= 0) cur_delay = 100;
            while (gif_accum_ms >= cur_delay) {
                gif_accum_ms -= cur_delay;
                gif_cur = (gif_cur + 1) % gif_frames.size();
                cur_delay = gif_frames[gif_cur].delay_ms;
                if (waylandSession) {
                    SDL_GetWindowSize(window, &gif_frames[gif_cur].tex.width, &gif_frames[gif_cur].tex.height);
                    UpdateWindowShapeFromRGBA(window, gif_frames[gif_cur].rgba.data(), gif_frames[gif_cur].tex.width, gif_frames[gif_cur].tex.height, 1);
                }
                // update drawable texture to point to new descriptor set
                texture = gif_frames[gif_cur].tex; // shallow copy of handles is ok
            }
        }
        
        // start the ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        { // konata rendering cycle
            // remove padding
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            {
                ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2((float)fb_width,(float)fb_height), ImGuiCond_Always);
            
                ImGui::Begin("background", nullptr,
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoBackground);
                
                
                ImGui::Image(
                    (ImTextureID)texture.DS, 
                    ImVec2(fb_width, fb_height), 
                
                    ImVec2(ImClamp(((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f) - fb_width) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f))), 0.0f, 1.0f), ImClamp(((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f) - fb_height) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f))), 0.0f, 1.0f)), 
                    ImVec2(ImClamp(1.0f - ((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f) - fb_width) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f))), 0.0f, 1.0f), ImClamp(1.0f - ((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f) - fb_height) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f))), 0.0f, 1.0f)));
                
                
                
                ImGui::End();
            }
            ImGui::PopStyleVar();
        }
        if (stats) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f,0.0f,0.0f,0.0f));
            {
                ImGui::SetNextWindowPos(ImVec2(2, fb_height-16), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(fb_width,32), ImGuiCond_Always);
            
                ImGui::Begin("statistics", nullptr,
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoBackground);
                ImGui::GetForegroundDrawList();
                ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);

                ImGui::End();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        /*ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f,0.0f,0.0f,0.0f));
        {
            ImGui::SetNextWindowPos(ImVec2((float)fb_width-30, -1), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(16,32), ImGuiCond_Always);
        
            ImGui::Begin("close", nullptr,
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoBackground);
            ImGui::GetForegroundDrawList();
            
            if (ImGui::Button("_")) {
                Minimize();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Minimize to tray");
            }
            ImGui::SameLine();
            if (ImGui::Button("X")) {
                if (!Shutdown(surface)) {
                    std::cout << "[ERROR] (Vulkan/SDL2) Unable to shutdown properly!" << std::endl;
                    std::exit(EXIT_FAILURE);
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Quit");
            }
            
            ImGui::End();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();*/
        
        // rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            // VkSurfaceCapabilitiesKHR caps;
            // vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice, wd->Surface, &caps);
            // printf("[vulkan] supportedCompositeAlpha = 0x%x, chosen format = %d\n", caps.supportedCompositeAlpha, wd->SurfaceFormat.format);
            wd->ClearValue.color.float32[0] = 0.03f; // r
            wd->ClearValue.color.float32[1] = 0.03f; // g
            wd->ClearValue.color.float32[2] = 0.03f; // b
            wd->ClearValue.color.float32[3] = 0.0f;  // a (transparency)
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
    }

    // cleanup
    for (auto &f : gif_frames) {
        RemoveTexture(&f.tex); // clean GIF frames
    }
    gif_frames.clear();
    if (!Shutdown(surface)) {
        std::cout << "[ERROR] (Vulkan/SDL2) Unable to shutdown properly!" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    return 0;
}

int Interface::Initialize() {
    std::cout << "\n>────────────────[INITIALIZING GRAPHICAL USER INTERFACE]────────────────<\n" << std::endl;
    // setup SDL
#ifdef _WIN32
    ::SetProcessDPIAware();
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        printf("[ERROR] (Vulkan/SDL2) %s\n", SDL_GetError());
        return -1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) SDL dependencies loaded successfully!" << std::endl;

// from 2.0.18: Enable native IME
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif


    std::cout << "\n>───────────[INITIALIZED GRAPHICAL USER INTERFACE SUCCESSULLY]──────────<\n" << std::endl;
    return 0;
}


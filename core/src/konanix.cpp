#include "core.h"
#include "konanix.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>
#include <vulkan/vulkan_core.h>
#include <GLFW/glfw3.h>

#include "logger.h"

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

// helpers
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
        logger::log("Check if you have supported Vulkan drivers to run this program (VK_API_VERSION_1_4)!",logger::exc);
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
    for (const auto& availableFormat : available_formats) {
        if (availableFormat.format==VK_FORMAT_B8G8R8_SRGB&&availableFormat.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return availableFormat;
    }
    return available_formats[0];
    // for (const VkSurfaceFormatKHR& f : available_formats) {
    //     if ((f.format == VK_FORMAT_B8G8R8A8_SRGB ||
    //          f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
    //         f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
    //         return f;
    //     }
    // }

    // for (const VkSurfaceFormatKHR& f : available_formats) {
    //     if ((f.format == VK_FORMAT_B8G8R8A8_UNORM ||
    //          f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
    //         f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
    //         return f;
    //     }
    // }

    // for (const VkSurfaceFormatKHR& f : available_formats) {
    //     if (f.format == VK_FORMAT_B8G8R8_SRGB &&
    //         f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
    //         return f;
    //     }
    // }

    // return available_formats[0];

}

static VkPresentModeKHR choose_swap_present_mode(const std::vector<VkPresentModeKHR> &available_present_modes) {
    for (const VkPresentModeKHR& availablePresentMode : available_present_modes) {
        if (availablePresentMode==VK_PRESENT_MODE_MAILBOX_KHR) return availablePresentMode;
    }
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

static VkShaderModule create_shader_module(const VkDevice device, const std::vector<char> &code) {
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


// VkCommandBuffer konanix::begin_single_time_commands() { // moved to core
//     const VkCommandBufferAllocateInfo alloc_info {
//         VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
//         VK_NULL_HANDLE,

//         g_commandpool,
//         VK_COMMAND_BUFFER_LEVEL_PRIMARY,
//         1
//     };
    
//     VkCommandBuffer command_buffer;
//     vkAllocateCommandBuffers(g_device, &alloc_info, &command_buffer);
    
//     VkCommandBufferBeginInfo begin_info{
//     VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
//     VK_NULL_HANDLE,
//     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
//     VK_NULL_HANDLE
//     };

//     vkBeginCommandBuffer(command_buffer, &begin_info);
//     return command_buffer;
// }

// void konanix::end_single_time_commands(VkCommandBuffer buffer) { // moved to core
//     vkEndCommandBuffer(buffer);
    
//     const VkSubmitInfo submitInfo {
//         VK_STRUCTURE_TYPE_SUBMIT_INFO,
//         VK_NULL_HANDLE,

//         0,
//         VK_NULL_HANDLE,
//         0,

//         1,
//         &buffer,

//         0,
//         VK_NULL_HANDLE
//     };

//     vkQueueSubmit(g_graphicsqueue, 1, &submitInfo, VK_NULL_HANDLE);
//     vkQueueWaitIdle(g_graphicsqueue);
//     vkFreeCommandBuffers(g_device, g_commandpool, 1, &buffer);
// }

// void konanix::copy_buffer_to_image(VkBuffer  buffer, VkImage image, const uint32_t &width, const uint32_t &height) { // moved to core
//     VkCommandBuffer command_buffer = begin_single_time_commands();

//     const VkBufferImageCopy region {
//         0,
//         0,
//         0,
//         {
//             VK_IMAGE_ASPECT_COLOR_BIT,
//             0,
//             0,
//             1
//         },
//         {
//             0,
//             0,
//             0,
//         },
//         {
//             width,
//             height,
//             1
//         }
//     };

//     vkCmdCopyBufferToImage(command_buffer,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);

//     end_single_time_commands(command_buffer);
// }

uint32_t konanix::find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(g_physicaldevice, &mem_properties);
    
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    logger::log("<Vulkan> Failed to find suitable memory type!",logger::exc);
    throw std::runtime_error("failed to find suitable memory type");
}

void konanix::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer &buffer, VkDeviceMemory &buffer_memory) {
    const static VkBufferCreateInfo buffer_info {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        VK_NULL_HANDLE,
        0,

        size,
        usage,
        VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(g_device,&buffer_info,nullptr,&buffer) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create buffer!",logger::exc);
        throw std::runtime_error("failed to create buffer");
    }

    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(g_device,buffer,&mem_requirements);

    const static VkMemoryAllocateInfo alloc_info {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        VK_NULL_HANDLE,

        mem_requirements.size,
        find_memory_type(mem_requirements.memoryTypeBits, properties)
    };

    if (vkAllocateMemory(g_device, &alloc_info, nullptr, &buffer_memory) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to allocate memory!",logger::exc);
        throw std::runtime_error("failed to allocate memory");
    }

    vkBindBufferMemory(g_device,buffer,buffer_memory,0);
}





konanix::konanix(const uint32_t &w, const uint32_t &h)
                : width(std::move(w)), height(std::move(h)) {
    // width = w;
    // height = h;
    if (!glfwInit()) {
        logger::log("<Vulkan> GLFW failed to initialize!",logger::exc);
        throw std::runtime_error("failed to initialize glfw");
    }

    const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());

    glfwWindowHint(GLFW_RED_BITS,mode->redBits);
    glfwWindowHint(GLFW_GREEN_BITS,mode->greenBits);
    glfwWindowHint(GLFW_BLUE_BITS,mode->blueBits);
    glfwWindowHint(GLFW_REFRESH_RATE,mode->refreshRate);
    logger::log("<Vulkan> Renderer will output "+std::to_string(mode->refreshRate)+"FPS",logger::dbg);
    // logger::log("<Vulkan> Window will be created with size params: "+std::to_string(width)+"x"+std::to_string(height),logger::dbg);

    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    g_window = glfwCreateWindow(width,height, "Konata Dancer", nullptr,nullptr);
    if (!g_window) {
        logger::log("<Vulkan> Failed to create a GLFW window!",logger::exc);
        glfwTerminate();
        throw std::runtime_error("failed to create a glfw window");
    }
    glfwSetWindowSize(g_window,width,height);
}

konanix::~konanix() {
    cleanup();
}

void konanix::initialize() {
    create_instance();
    {
        if (glfwCreateWindowSurface(g_instance, g_window, nullptr, &g_surface) != VK_SUCCESS) {
            logger::log("<Vulkan> Failed to create a window surface!",logger::exc);
            throw std::runtime_error("failed to create a window surface");
        }
        logger::log("<Vulkan> Surface created successfully!",logger::dbg);
    }
    create_device();
    create_swap_chain();
    create_image_views();
    create_render_pass();
    create_descriptor_pool();
    create_descriptor_set_layout();

    create_gif_image(width, height);

    create_descriptor_set();
    create_graphics_pipeline();
    create_framebuffers();
    create_commandpool();
    create_commandbuffers();
    create_sync_objects();

}

void konanix::cleanup() {

    if (g_descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_device,g_descriptor_set_layout,nullptr);
    }

    if (g_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_device,g_descriptor_pool,nullptr);
    }
    
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (g_image_available_semaphores[i]!=VK_NULL_HANDLE) {
            vkDestroySemaphore(g_device,g_image_available_semaphores[i],nullptr);
            logger::log("<Vulkan> Semaphore \"g_image_available_semaphores["+std::to_string(i)+"]\" destroyed!",logger::dbg);
        } else { logger::log("<Vulkan> Could not destroy semaphore \"g_image_available_semaphores["+std::to_string(i)+"]\"!",logger::wrn); }

        if (g_render_finished_semaphores[i]!=VK_NULL_HANDLE) {
            vkDestroySemaphore(g_device,g_render_finished_semaphores[i],nullptr);
            logger::log("<Vulkan> Semaphore \"g_render_finished_semaphores["+std::to_string(i)+"]\" destroyed!",logger::dbg);
        } else { logger::log("<Vulkan> Could not destroy semaphore \"g_render_finished_semaphores["+std::to_string(i)+"]\"!",logger::wrn); }

        if (g_in_flight_fences[i]!=VK_NULL_HANDLE) {
            vkDestroyFence(g_device,g_in_flight_fences[i],nullptr);
            logger::log("<Vulkan> Fence \"g_in_flight_fences["+std::to_string(i)+"]\" destroyed!",logger::dbg);
        } else { logger::log("<Vulkan> Could not destroy fence \"g_in_flight_fences["+std::to_string(i)+"]\"!",logger::wrn); }

        // vkDestroySemaphore(g_device, g_render_finished_semaphores[i], nullptr);
        // vkDestroySemaphore(g_device, g_render_finished_semaphores[i], nullptr);
        // vkDestroyFence(g_device, g_in_flight_fences[i], nullptr);
    }

    if (g_commandpool!=VK_NULL_HANDLE) {
        vkDestroyCommandPool(g_device,g_commandpool,nullptr);
        logger::log("<Vulkan> Command pool destroyed!",logger::dbg);
    }

    if (g_device!=VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_device);
    }

    for (VkFramebuffer fb : g_swapchain_framebuffers) {
        vkDestroyFramebuffer(g_device,fb,nullptr);
    }
    logger::log("<Vulkan> g_swapchain_framebuffers vector cleaned up successfully!",logger::dbg);

    if (g_graphics_pipeline!=VK_NULL_HANDLE) {
        vkDestroyPipeline(g_device,g_graphics_pipeline,nullptr);
        logger::log("<Vulkan> Graphics pipeline destroyed!",logger::dbg);
    }

    if (g_pipeline_layout!=VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_device,g_pipeline_layout,nullptr);
        logger::log("<Vulkan> Pipeline layout destroyed!",logger::dbg);
    }

    if (g_renderpass!=VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_device,g_renderpass,nullptr);
        logger::log("<Vulkan> Render Pass destroyed!",logger::dbg);
    }

    for (VkImageView iw : g_swapchain_image_views) {
        vkDestroyImageView(g_device,iw,nullptr);
    }
    logger::log("<Vulkan> g_swapchain_image_views vector cleaned up successfully!",logger::dbg);

    if (g_swapchain!=VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g_device,g_swapchain,nullptr);
        logger::log("<Vulkan> Swap chain destroyed successfully!",logger::dbg);
    }

    if (g_device!=VK_NULL_HANDLE) {
        vkDestroyDevice(g_device,nullptr);
        g_device = VK_NULL_HANDLE;
        logger::log("<Vulkan> Device instance destroyed successfully!",logger::dbg);
    }

    if (g_surface!=VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
        g_surface = VK_NULL_HANDLE;
        logger::log("<Vulkan> Surface destroyed successfully!",logger::dbg);
    }

    if (g_instance!=VK_NULL_HANDLE) {
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
        logger::log("<Vulkan> Instance destroyed successfully!",logger::dbg);
    }

    if (g_window) {
        glfwDestroyWindow(g_window);
        g_window = nullptr;
    }
    glfwTerminate();
    logger::log("<Vulkan> Objects cleaned up!",logger::dbg);

}






void konanix::create_instance() {
    constexpr VkApplicationInfo appInfo {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        VK_NULL_HANDLE,

        konacore::project,
        VK_MAKE_VERSION(konacore::version[0],konacore::version[1],konacore::version[2]),
        
        "konanix",
        VK_MAKE_VERSION(version[0],version[1],version[2]),
        
        VK_API_VERSION_1_0
    };

    uint32_t extension_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&extension_count);
    
    // const VkInstanceCreateInfo createInfo {
    //     VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    //     VK_NULL_HANDLE,
    //     0,
    //     &appInfo,
    //     0,
    //     0,
    //     extension_count,
    //     glfwGetRequiredInstanceExtensions(&extension_count)
    // };

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &appInfo;
#ifdef __APPLE__
    // copy glfw extensions and add portability enumeration
    std::vector<const char*> requiredExtensions;
    for (uint32_t i = 0; i < extension_count; ++i) requiredExtensions.emplace_back(glfw_extensions[i]);
    requiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    instance_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#ifdef DEBUG
    requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    instance_info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
    instance_info.ppEnabledExtensionNames = requiredExtensions.data();
#else
#ifdef DEBUG
    std::vector<const char*> extensions(glfw_extensions,glfw_extensions+extension_count);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
#else
    instance_info.ppEnabledExtensionNames = glfw_extensions;
    instance_info.enabledExtensionCount = extension_count;
#endif
#endif

    if (vkCreateInstance(&instance_info,nullptr,&g_instance) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create a Vulkan instance!",logger::exc);
        throw std::runtime_error("failed to create instance");
    }
    logger::log("<Vulkan> Instance created successfully!",logger::dbg);
}






void konanix::create_device() {
    logger::log("<Vulkan> Setting up a logical device...",logger::dbg);

    const static std::vector<const char*> required_device_extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    constexpr static VkPhysicalDeviceFeatures required_features {
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
        VK_TRUE,
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
        VK_TRUE,
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
        VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,VK_FALSE,
    };

    logger::log("<Vulkan> Selecting the most optimal physical device...",logger::dbg);
    g_physicaldevice = pick_device(g_instance, g_surface);
    logger::log("<Vulkan> Physical device set!",logger::dbg);

    logger::log("<Vulkan> Discovering queue families for physical device...",logger::dbg);
    QueueFamilyIndices qfi = find_queue_families(g_physicaldevice, g_surface);
    if (!qfi.complete()) {
        logger::log("<Vulkan> Selected physical device does not expose required queue families (graphics/present).", logger::exc);
        throw std::runtime_error("selected device missing required queue families");
    }
    logger::log("<Vulkan> Queue families are present!",logger::dbg);

    std::vector<VkDeviceQueueCreateInfo> queue_create_info_vec;
    std::set<uint32_t> uniqueQueueFamilies = {qfi.graphicsFamily.value(),qfi.presentFamily.value()};
    float queue_priority = 1.0f;
    for (uint32_t queue_family : uniqueQueueFamilies) {
        const VkDeviceQueueCreateInfo queue_info {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            VK_NULL_HANDLE,
            0,
            queue_family,
            1,
            &queue_priority
        };
        queue_create_info_vec.push_back(queue_info);
    }

    const VkDeviceCreateInfo device_info {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        static_cast<uint32_t>(queue_create_info_vec.size()),
        queue_create_info_vec.data(),
        0,
        VK_NULL_HANDLE,
        static_cast<uint32_t>(required_device_extensions.size()),
        required_device_extensions.data(),
        &required_features
    };

    if (vkCreateDevice(g_physicaldevice,&device_info,nullptr,&g_device) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create a logical device!",logger::exc);
        throw std::runtime_error("failed to create a logical device");
    }
    logger::log("<Vulkan> Logical device created!",logger::dbg);
    vkGetDeviceQueue(g_device,qfi.graphicsFamily.value(),0,&g_graphicsqueue);
    vkGetDeviceQueue(g_device,qfi.presentFamily.value(),0,&g_presentqueue);
    logger::log("<Vulkan> Graphics and present queue set!",logger::dbg);
}






void konanix::create_swap_chain() {
    logger::log("<Vulkan> Creating swap chain...",logger::dbg);
    
    SwapChainSupportDetails swap_chain_support = query_swap_chain_support(g_physicaldevice, g_surface);
    VkSurfaceFormatKHR surface_format = choose_swap_surface_format(swap_chain_support.formats);
    VkPresentModeKHR present_mode = choose_swap_present_mode(swap_chain_support.present_modes);
    VkExtent2D extent = choose_swap_extent(swap_chain_support.capabilities, g_window);

    uint32_t image_count = swap_chain_support.capabilities.minImageCount+1; // using fifo
    if (swap_chain_support.capabilities.maxImageCount > 0 && image_count > swap_chain_support.capabilities.maxImageCount) { // check max
        image_count = swap_chain_support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchain_info{};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = g_surface;
    swapchain_info.minImageCount = image_count;

    swapchain_info.imageFormat = surface_format.format;
    swapchain_info.imageColorSpace = surface_format.colorSpace;

    swapchain_info.imageExtent = extent;

    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    logger::log("<Vulkan> Swapchain format selected: " + std::to_string(surface_format.format)+", color space: " + std::to_string(surface_format.colorSpace),logger::dbg);
    logger::log("<Vulkan> Composite alpha selected: " + std::to_string(swapchain_info.compositeAlpha),logger::dbg);
    logger::log("<Vulkan> Supported composite alpha flags: "+std::to_string(swap_chain_support.capabilities.supportedCompositeAlpha),logger::dbg);

    QueueFamilyIndices indices = find_queue_families(g_physicaldevice, g_surface);
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily) {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_info.queueFamilyIndexCount = 2;
        swapchain_info.pQueueFamilyIndices = queueFamilyIndices;
        logger::log("<Vulkan> Swap chain will be using \"VK_SHARING_MODE_CONCURRENT\".",logger::dbg);
    } else {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_info.queueFamilyIndexCount = 0;
        swapchain_info.pQueueFamilyIndices = nullptr;
        logger::log("<Vulkan> Swap chain will be using \"VK_SHARING_MODE_EXCLUSIVE\".",logger::dbg);
    }
    swapchain_info.preTransform = swap_chain_support.capabilities.currentTransform;
    swapchain_info.compositeAlpha =
    (swap_chain_support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
        : (swap_chain_support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR
            : (swap_chain_support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    swapchain_info.presentMode = present_mode;
    swapchain_info.clipped = VK_TRUE; // enable if full rendering is required in the future https://vulkan-tutorial.com/Drawing_a_triangle/Presentation/Swap_chain

    swapchain_info.oldSwapchain = VK_NULL_HANDLE; // TODO: MAKE REBUILDABLE SWAPCHAINS
    if (vkCreateSwapchainKHR(g_device,&swapchain_info,nullptr,&g_swapchain)!=VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create swap chain!",logger::exc);
        throw std::runtime_error("failed to create swap chain");
    }

    vkGetSwapchainImagesKHR(g_device, g_swapchain, &image_count, nullptr);
    g_swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &image_count, g_swapchain_images.data());
    logger::log("<Vulkan> Populated \"g_swapchain_images\" vector!",logger::dbg);

    g_swapchain_image_format = surface_format.format;
    g_swapchain_extent = extent;

    logger::log("<Vulkan> Swap chain created!",logger::dbg);
}

void konanix::create_image_views() {
    g_swapchain_image_views.resize(g_swapchain_images.size());
    for (size_t i = 0; i < g_swapchain_images.size(); i++) {
        const VkImageViewCreateInfo imageview_info {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            VK_NULL_HANDLE,
            0,
            
            g_swapchain_images[i],
            VK_IMAGE_VIEW_TYPE_2D,
            g_swapchain_image_format,

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

        if (vkCreateImageView(g_device,&imageview_info,nullptr,&g_swapchain_image_views[i])) {
            logger::log("<Vulkan> Failed to create swap chain!",logger::exc);
            throw std::runtime_error("failed to create swap chain");
        }
    }
}

void konanix::create_graphics_pipeline() {
    logger::log("<Vulkan> Creating the graphics pipeline...",logger::dbg);

    VkShaderModule v_shadermodule = create_shader_module(g_device,read_file("shaders/vert.spv"));
    VkShaderModule f_shadermodule = create_shader_module(g_device,read_file("shaders/frag.spv"));

        VkPipelineShaderStageCreateInfo v_shader_info{};
        v_shader_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        v_shader_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        v_shader_info.module = v_shadermodule;
        v_shader_info.pName = "main";

        VkPipelineShaderStageCreateInfo f_shader_info{};
        f_shader_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        f_shader_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        f_shader_info.module = f_shadermodule;
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
        
        // from https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Fixed_functions
        // VK_PRIMITIVE_TOPOLOGY_POINT_LIST,        // points from vertices
        // VK_PRIMITIVE_TOPOLOGY_LINE_LIST,         // line from every 2 vertices without reuse
        // VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,        // the end vertex of every line is used as start vertex for the next line
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,        // triangle from every 3 vertices without reuse
        // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,    // the second and third vertex of every triangle are used as first two vertices of the next triangle

        VK_FALSE
    };

    const VkViewport viewport {
        0.0f,
        0.0f,
        (float) g_swapchain_extent.width,
        (float) g_swapchain_extent.height,
        0.0f,
        1.0f
    };

    const VkRect2D scissor {
        {0,0},
        g_swapchain_extent
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
        0.0f
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
        &g_descriptor_set_layout,
        0,
        nullptr
    };

    if (vkCreatePipelineLayout(g_device,&pipeline_layout_info,nullptr,&g_pipeline_layout) != VK_SUCCESS) {
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

        g_pipeline_layout,
        g_renderpass,
        0,

        VK_NULL_HANDLE,
        -1
    };

    if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &g_graphics_pipeline) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create the graphics pipeline!",logger::exc);
        throw std::runtime_error("failed to create the graphics pipeline");
    }

    logger::log("<Vulkan> Graphics pipeline created! Cleaning shader data...",logger::dbg);
    vkDestroyShaderModule(g_device,v_shadermodule,nullptr);
    vkDestroyShaderModule(g_device,f_shadermodule,nullptr);

    logger::log("<Vulkan> Graphics pipeline created!",logger::dbg);
}

void konanix::create_render_pass() {
    logger::log("<Vulkan> Creating the render pass...",logger::dbg);
    // comments from https://vulkan-tutorial.com/en/Drawing_a_triangle/Graphics_pipeline_basics/Render_passes
    const VkAttachmentDescription color_attachment {
        0,
        g_swapchain_image_format,
        VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,

        // VK_ATTACHMENT_LOAD_OP_LOAD,          // Preserve the existing contents of the attachment
        // VK_ATTACHMENT_LOAD_OP_CLEAR,         // Clear the values to a constant at the start
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,     // Existing contents are undefined; don't care about them

        // VK_ATTACHMENT_STORE_OP_STORE,        // Rendered contents will be stored in memory and can be read later
        VK_ATTACHMENT_STORE_OP_DONT_CARE,    // Contents of the framebuffer will be undefined after the rendering operation

        VK_IMAGE_LAYOUT_UNDEFINED,

        // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,    // Images used as color attachment
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR             // Images to be presented in the swap chain
        // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,        // Images to be used as destination for a memory copy operation
    };

    constexpr VkAttachmentReference color_attachment_ref {
        0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    // for later:
    // The following other types of attachments can be referenced by a subpass:

    // pInputAttachments: Attachments that are read from a shader
    // pResolveAttachments: Attachments used for multisampling color attachments
    // pDepthStencilAttachment: Attachment for depth and stencil data
    // pPreserveAttachments: Attachments that are not used by this subpass, but for which the data must be preserved

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

        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,

        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
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

    if (vkCreateRenderPass(g_device, &renderpass_info, nullptr, &g_renderpass) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to create render pass!",logger::exc);
        throw std::runtime_error("failed to create render pass");
    }

    logger::log("<Vulkan> Render pass created!",logger::dbg);
}

void konanix::create_framebuffers() {
    g_swapchain_framebuffers.resize(g_swapchain_image_views.size());

    for (size_t i = 0; i < g_swapchain_image_views.size(); i++) {
        const VkImageView attachments[] = {
            g_swapchain_image_views[i]
        };

        const VkFramebufferCreateInfo framebuffer_info {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            VK_NULL_HANDLE,
            0,
            g_renderpass,
            1,
            attachments,
            g_swapchain_extent.width,
            g_swapchain_extent.height,
            1
        };

        if (vkCreateFramebuffer(g_device, &framebuffer_info, nullptr, &g_swapchain_framebuffers[i]) != VK_SUCCESS) {
            logger::log("<Vulkan> Failed to a framebuffer!",logger::exc);
            throw std::runtime_error("failed to create a framebuffer");
        }
    }
}



void konanix::create_commandpool() {
    logger::log("<Vulkan> Creating command pool...",logger::dbg);

    QueueFamilyIndices qfi = find_queue_families(g_physicaldevice, g_surface);

    const VkCommandPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        VK_NULL_HANDLE,

        // VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,               // Hint that command buffers are rerecorded with new commands very often (may change memory allocation behavior)
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,    // Allow command buffers to be rerecorded individually, without this flag they all have to be reset together

        qfi.graphicsFamily.value()
    };

    if (vkCreateCommandPool(g_device,&pool_info,nullptr,&g_commandpool) != VK_SUCCESS) {
            logger::log("<Vulkan> Failed to the command pool!",logger::exc);
            throw std::runtime_error("failed to create command pool");
    }
    logger::log("<Vulkan> Command pool created!",logger::dbg);

}

void konanix::create_commandbuffers() {
    logger::log("<Vulkan> Allocating command buffer...",logger::dbg);
    g_commandbuffers.resize(MAX_FRAMES_IN_FLIGHT);

    const VkCommandBufferAllocateInfo alloc_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        VK_NULL_HANDLE,
        
        g_commandpool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        (uint32_t) g_commandbuffers.size()
    };

    if (vkAllocateCommandBuffers(g_device,&alloc_info,g_commandbuffers.data()) != VK_SUCCESS) {
            logger::log("<Vulkan> Failed to allocate the command buffer!",logger::exc);
            throw std::runtime_error("failed to allocate command buffer");
    }

    logger::log("<Vulkan> Command buffer allocated successfully!",logger::dbg);

}

void konanix::record_command_buffer(VkCommandBuffer commandbuffer, uint32_t image_index) {
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        0,
        nullptr
    };

    if (vkBeginCommandBuffer(commandbuffer, &begin_info) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer");
    }

    VkClearValue clear_color = {{{0.0f, 0.0f, 0.0f, 0.0f}}};

    VkRenderPassBeginInfo renderpass_info{
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        nullptr,
        g_renderpass,
        g_swapchain_framebuffers[image_index],
        {{0, 0}, g_swapchain_extent},
        1,
        &clear_color
    };

    vkCmdBeginRenderPass(commandbuffer, &renderpass_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_graphics_pipeline);

    VkViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(g_swapchain_extent.width),
        static_cast<float>(g_swapchain_extent.height),
        0.0f, 1.0f
    };
    vkCmdSetViewport(commandbuffer, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, g_swapchain_extent};
    vkCmdSetScissor(commandbuffer, 0, 1, &scissor);

    vkCmdBindDescriptorSets(
        commandbuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_pipeline_layout,
        0,
        1,
        &g_descriptor_set,
        0,
        nullptr
    );

    vkCmdDraw(commandbuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandbuffer);

    if (vkEndCommandBuffer(commandbuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to record command buffer");
    }
}

void konanix::create_sync_objects() {
    g_image_available_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
    g_render_finished_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
    g_in_flight_fences.resize(MAX_FRAMES_IN_FLIGHT);

    constexpr VkSemaphoreCreateInfo semaphore_info {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        VK_NULL_HANDLE,
        0
    };

    constexpr VkFenceCreateInfo fence_info {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        VK_NULL_HANDLE,
        VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(g_device,&semaphore_info,nullptr,&g_image_available_semaphores[i]) != VK_SUCCESS) {
            logger::log("<Vulkan> Failed create a semaphore!",logger::exc);
            throw std::runtime_error("failed to create a semaphore");
        }    
        if (vkCreateSemaphore(g_device,&semaphore_info,nullptr,&g_render_finished_semaphores[i]) != VK_SUCCESS) {
            logger::log("<Vulkan> Failed create a semaphore!",logger::exc);
            throw std::runtime_error("failed to create a semaphore");
        }    

        if (vkCreateFence(g_device,&fence_info,nullptr,&g_in_flight_fences[i]) != VK_SUCCESS) {
            logger::log("<Vulkan> Failed create a fance!",logger::exc);
            throw std::runtime_error("failed to create a fence");
        }
    } 
}

// moved to core
// void konanix::draw_frame() {
//     vkWaitForFences(g_device,1,&g_in_flight_fence,VK_TRUE,UINT64_MAX);
//     vkResetFences(g_device,1,&g_in_flight_fence);

//     uint32_t image_index;
//     vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_image_available_semaphore, VK_NULL_HANDLE, &image_index);

//     vkResetCommandBuffer(g_commandbuffer,0);
//     record_command_buffer(g_commandbuffer, image_index);
    
//     const VkSemaphore wait_semaphores[] = {g_image_available_semaphore};
//     const VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
//     const VkSemaphore signal_semaphores[] = {g_render_finished_semaphore};

//     const VkSubmitInfo submit_info {
//         VK_STRUCTURE_TYPE_SUBMIT_INFO,
//         VK_NULL_HANDLE,

//         1,
//         wait_semaphores,
//         wait_stages,

//         1,
//         &g_commandbuffer,

//         1,
//         signal_semaphores
//     };
//     if (vkQueueSubmit(g_graphicsqueue,1,&submit_info,g_in_flight_fence) != VK_SUCCESS) {
//         logger::log("<Vulkan> Failed to submit draw command buffer!",logger::exc);
//         throw std::runtime_error("failed to submit draw command buffer");
//     } 
    

//     const VkSwapchainKHR swapchains[] = {g_swapchain};

//     const VkPresentInfoKHR present_info {
//         VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
//         VK_NULL_HANDLE,

//         1,
//         signal_semaphores,

//         1,
//         swapchains,

//         &image_index,

//         nullptr
//     };
//     vkQueuePresentKHR(g_presentqueue,&present_info);

// };

void konanix::recreate_swap_chain() {
    logger::log("Recreating swap chain...",logger::dbg);
    vkDeviceWaitIdle(g_device);

    for (VkFramebuffer f : g_swapchain_framebuffers)
        vkDestroyFramebuffer(g_device, f, nullptr);

    for (VkImageView iw : g_swapchain_image_views)
        vkDestroyImageView(g_device, iw, nullptr);
    
    vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);

    create_swap_chain();
    create_image_views();
    create_framebuffers();

    logger::log("Swap chain recreated!",logger::dbg);
}
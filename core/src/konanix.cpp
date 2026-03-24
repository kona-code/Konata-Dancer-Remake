#include "core.h"
#include "konanix.h"
#include <algorithm>
#include <cstdint>
#include <optional>
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
            logger::log("<Vulkan> Checking if swapchain extensions are supported on this device...",logger::dbg);{
            SwapChainSupportDetails sc_support = query_swap_chain_support(device, surface);
            if (sc_support.formats.empty() || sc_support.present_modes.empty()) {
                logger::log("<Vulkan> Swapchain extensions are NOT supported by \""+std::string(p_dev.deviceName)+"\"!",logger::dbg);
                return -1;
            }
            logger::log("<Vulkan> Swapchain extensions are supported!",logger::dbg);
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
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score += 10000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 3000; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score += 1000; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score += 100; break;
        default: score += 10; break;
    }

    score = static_cast<int>(p_dev.limits.maxImageDimension2D / 1024);

    if (f_dev.geometryShader) score += 500;
    if (f_dev.samplerAnisotropy) score += 200;

    if (qfi.graphicsFamily.has_value()) {
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device,&queue_family_count,nullptr);
        std::vector<VkQueueFamilyProperties> qfs(queue_family_count);
        if (queue_family_count > 0) vkGetPhysicalDeviceQueueFamilyProperties(device,&queue_family_count,qfs.data());
        
        for (uint32_t i = 0; i < queue_family_count; ++i) {
            if ((qfs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                score += 50; // dedicated transfer
                break;
            }
        }
    }
    logger::log("<Vulkan> Device \""+std::string(p_dev.deviceName)+"\" scored "+std::to_string(score)+" points!",logger::dbg);
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
        logger::log("Device \""+std::string(p_dev.deviceName)+"\" supports Vulkan! Rating device...",logger::dbg);\

        const int score = rate_device(dev,surface,required_extensions,required_features);
        if (score>0) {
            candidates.push_back({dev,score});
            logger::log("Device \""+std::string(p_dev.deviceName)+"\" accepted! Device scored: "+std::to_string(score)+" points!",logger::dbg);
        } else {
            logger::log("Device rejected! Reason: score <= 0",logger::dbg);
        }
    }


    if (candidates.empty()) return VK_NULL_HANDLE;


    std::sort(candidates.begin(), candidates.end(), [](const candidate& a, const candidate& b){
        return a.score > b.score; // descending order
    });


    VkPhysicalDevice best = candidates.front().dev;
    VkPhysicalDeviceProperties bestp_dev{};
    vkGetPhysicalDeviceProperties(best, &bestp_dev);
    logger::log("Selected device: \""+std::string(bestp_dev.deviceName)+"\". Device score: "+std::to_string(candidates.front().score)+" points.", logger::dbg);
    return best;
}






konanix::konanix() {
    if (!glfwInit()) {
        logger::log("GLFW failed to initialize!",logger::exc);
        throw std::runtime_error("failed to initialize glfw");
    }
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    g_window = glfwCreateWindow(640, 480, "Konata Dancer", nullptr,nullptr);
    if (!g_window) {
        logger::log("Failed to create a GLFW window!",logger::exc);
        glfwTerminate();
        throw std::runtime_error("failed to create a glfw window");
    }
}

konanix::~konanix() {
    cleanup();
}

void konanix::initialize() {
    create_instance();
    {
        if (glfwCreateWindowSurface(g_instance, g_window, nullptr, &g_surface) != VK_SUCCESS) {
            logger::log("Failed to create a window surface!",logger::exc);
            throw std::runtime_error("failed to create a window surface");
        }
        logger::log("<Vulkan> Surface created successfully!",logger::dbg);
    }
    create_device();

}

void konanix::create_instance() {
    constexpr VkApplicationInfo appInfo {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        VK_NULL_HANDLE,

        konacore::project,
        VK_MAKE_VERSION(konacore::version[0],konacore::version[1],konacore::version[2]),
        
        "konanix",
        VK_MAKE_VERSION(version[0],version[1],version[2]),
        
        VK_API_VERSION_1_4
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

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
#ifdef __APPLE__
    // copy glfw extensions and add portability enumeration
    std::vector<const char*> requiredExtensions;
    for (uint32_t i = 0; i < extension_count; ++i) requiredExtensions.emplace_back(glfw_extensions[i]);
    requiredExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#ifdef DEBUG
    requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    createInfo.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
    createInfo.ppEnabledExtensionNames = requiredExtensions.data();
#else
#ifdef DEBUG
    std::vector<const char*> extensions(glfw_extensions,glfw_extensions+extension_count);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
#else
    createInfo.ppEnabledExtensionNames = glfw_extensions;
    createInfo.enabledExtensionCount = extension_count;
#endif
#endif

    if (vkCreateInstance(&createInfo,nullptr,&g_instance) != VK_SUCCESS) {
        logger::log("Failed to create a Vulkan instance!",logger::exc);
        throw std::runtime_error("failed to create instance");
    }
    logger::log("<Vulkan> Instance created successfully!",logger::dbg);
}

void konanix::create_device() {
    logger::log("<Vulkan> Setting up a logical device...",logger::dbg);


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

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfoVec;



}

void konanix::cleanup() {
    if (g_device != VK_NULL_HANDLE)
        vkDestroyDevice(g_device, nullptr);
    g_physicaldevice = VK_NULL_HANDLE;
    if (g_instance != VK_NULL_HANDLE)
        vkDestroyInstance(g_instance,nullptr);

    logger::log("<Vulkan> Objects cleaned up!",logger::dbg);

}
#include "core.h"
#include "konanix.h"
#include <cstdint>
#include <stdexcept>
#include <vulkan/vulkan_core.h>
#include <GLFW/glfw3.h>

#include "logger.h"

void konanix::initialize() {
    create_instance();


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
    const char** extensions = glfwGetRequiredInstanceExtensions(&extension_count);
    
    const VkInstanceCreateInfo createInfo {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        &appInfo,
        0,
        0,
        extension_count,
        glfwGetRequiredInstanceExtensions(&extension_count)
    };

    if (vkCreateInstance(&createInfo,nullptr,&g_instance) != VK_SUCCESS) {
        logger::log("Failed to create a Vulkan instance!",logger::exc);
        throw std::runtime_error("failed to create instance");
    }
    logger::log("<Vulkan> Instance created successfully!",logger::dbg);
}

void konanix::create_device() {
    
}

void konanix::cleanup() {
    if (g_device != VK_NULL_HANDLE)
        vkDestroyDevice(g_device, nullptr);
    g_physicaldevice = VK_NULL_HANDLE;
    if (g_instance != VK_NULL_HANDLE)
        vkDestroyInstance(g_instance,nullptr);

    logger::log("<Vulkan> Objects cleaned up!",logger::dbg);

}
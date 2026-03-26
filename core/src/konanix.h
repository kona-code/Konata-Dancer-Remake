#pragma once
#include <vector>
#include <vulkan/vulkan.h>
#include <atomic>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined (__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

class konanix {
public:
    void initialize();
    void render();
    void cleanup();
    std::atomic<bool> running {true};
    konanix();
    ~konanix();
private:
    void create_instance();
    void create_device();
    void create_surface();
    void create_swap_chain();
    void cleanup_swap_chain();
    void create_image_views();
    void create_framebuffers();
    void create_graphics_pipeline();
    void recreate_swap_chain();


    constexpr static short version[3] = {1, 0, 0};

protected:
    GLFWwindow* g_window;
    VkInstance g_instance;
    VkSurfaceKHR g_surface;

    VkPhysicalDevice g_physicaldevice;
    VkDevice g_device;
    VkQueue g_graphicsqueue;

    VkSwapchainKHR g_swapchain;
    std::vector<VkImage> g_swapchain_images;
    VkFormat g_swapchain_image_format;
    VkExtent2D g_swapchain_extent;
    std::vector<VkImageView> g_swapchain_image_views;
    std::vector<VkFramebuffer> g_swapchain_framebuffer;

};
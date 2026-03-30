#pragma once
#include <vector>
#include <vulkan/vulkan.h>
#include <atomic>
#include <vulkan/vulkan_core.h>

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
    GLFWwindow* g_window;
    void initialize();
    void render();
    void cleanup();
    void draw_frame();
    std::atomic<bool> running {true};
    konanix(const uint32_t width, const uint32_t height);
    ~konanix();
private:
    void create_instance();
    void create_device();
    void create_surface();
    void create_swap_chain();
    void cleanup_swap_chain();
    void create_image_views();
    void create_render_pass();
    void create_graphics_pipeline();
    void create_framebuffers();
    void create_commandpool();
    void create_commandbuffer();
    void create_sync_objects();
    void recreate_swap_chain();

    void record_command_buffer(VkCommandBuffer commandbuffer, uint32_t image_index);

    constexpr static short version[3] = {1, 0, 0};

protected:
    VkInstance g_instance;
    VkSurfaceKHR g_surface;

    VkPhysicalDevice g_physicaldevice;
    VkDevice g_device;
    VkQueue g_graphicsqueue;
    VkQueue g_presentqueue;

    VkSwapchainKHR g_swapchain;
    std::vector<VkImage> g_swapchain_images;
    VkFormat g_swapchain_image_format;
    VkExtent2D g_swapchain_extent;
    std::vector<VkImageView> g_swapchain_image_views;
    std::vector<VkFramebuffer> g_swapchain_framebuffers;

    VkRenderPass g_renderpass;
    VkPipelineLayout g_pipeline_layout;
    VkPipeline g_graphics_pipeline;

    VkCommandPool g_commandpool;
    VkCommandBuffer g_commandbuffer;

    VkSemaphore g_image_available_semaphore;
    VkSemaphore g_render_finished_semaphore;
    VkFence g_in_flight_fence;
};
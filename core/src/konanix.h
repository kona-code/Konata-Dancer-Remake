#pragma once
#include <cstdint>
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
    konanix(const uint32_t &width = 1280, const uint32_t &height = 640);
    ~konanix();


    void create_gif_image(uint32_t width, uint32_t height);
    void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer &buffer, VkDeviceMemory &buffer_memory);
    void upload_rgba_frame_to_gif_image(const uint8_t* rgba_pixels, size_t pixel_bytes, uint32_t width, uint32_t height, bool first_upload = false);
    void transition_image_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout);
    void copy_buffer_to_image(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
    VkDevice get_device() const { return g_device; };
    uint32_t width, height;
    VkDeviceSize image_size;
    void* pxdata;
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
    void create_commandbuffers();
    void create_sync_objects();
    void recreate_swap_chain();

    void record_command_buffer(VkCommandBuffer commandbuffer, uint32_t image_index);
    void create_descriptor_set();
    void create_descriptor_pool();
    void create_descriptor_set_layout();

    VkCommandBuffer begin_single_time_commands();
    void end_single_time_commands(VkCommandBuffer command_buffer);
    // void copy_buffer_to_image(VkBuffer  buffer, VkImage image, const uint32_t &width, const uint32_t &height);
    uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void create_image(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                            VkMemoryPropertyFlags properties, VkImage &image, VkDeviceMemory &image_memory);

    constexpr static short version[3] = {1, 0, 0};
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
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
    std::vector<VkCommandBuffer> g_commandbuffers;

    // VkSemaphore g_image_available_semaphore;
    // VkSemaphore g_render_finished_semaphore;
    // VkFence g_in_flight_fence;
    std::vector<VkSemaphore> g_image_available_semaphores;
    std::vector<VkSemaphore> g_render_finished_semaphores;
    std::vector<VkFence> g_in_flight_fences;

    VkDescriptorSet g_descriptor_set;
    VkDescriptorSetLayout g_descriptor_set_layout;
    VkDescriptorPool g_descriptor_pool;

    uint32_t current_frame = 1;
};
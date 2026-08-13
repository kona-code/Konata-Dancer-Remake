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

#pragma once
#include <cstdint>
#include <string>
#include <vector>
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

struct GifFrame {
    std::vector<uint8_t> rgba;
    int delay_ms = 100;
};

struct GifAnimation {
    int width = 0;
    int height = 0;
    std::vector<GifFrame> frames;
};

namespace konanix {
    inline GifAnimation g_raw_anim_data;
    inline bool g_swapchain_rebuild = false;

    void initialize(const uint32_t gif_frame_count, const uint32_t &width = 1280, const uint32_t &height = 640, const bool &debug = false, const bool &resizable = false);
    void render();
    void cleanup();
    void draw_frame(int width, int height);

    void create_gif_image(const unsigned char* pixels, const uint32_t &frame, uint32_t width, uint32_t height);
    void upload_rgba_frame_to_gif_image(const uint8_t* rgba_pixels, size_t pixel_bytes, uint32_t width, uint32_t height, bool first_upload = false);

    struct Overlay {
        int w = 0;
        int h = 0;
        std::vector<uint8_t> storage;
        uint8_t* rgba = nullptr;
    
        void clear() {
            if (!rgba) return;
            std::fill(storage.begin(), storage.end(), 0);
        }
    
        void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
        void rect(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
        void stroke_rect(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
        void draw_char(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale = 2);
        void draw_text(int x, int y, const std::string& s, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale = 2);
    };
    void draw_context_menu(Overlay &overlay);
    VkSampler create_sampler();
    VkImageView create_image_view(VkImage image, VkFormat format);
    inline VkDeviceSize image_size;
    inline void* pxdata;
    
    void recreate_swap_chain();
    void create_descriptor_set();
    void record_command_buffer(VkCommandBuffer commandbuffer, uint32_t image_index);

    VkCommandBuffer begin_single_time_commands();
    void end_single_time_commands(VkCommandBuffer &command_buffer);
    // void copy_buffer_to_image(VkBuffer  buffer, VkImage image, const uint32_t &width, const uint32_t &height);
    void create_image(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                            VkMemoryPropertyFlags properties, VkImage &image, VkDeviceMemory &image_memory);


};

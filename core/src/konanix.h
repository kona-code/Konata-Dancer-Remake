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
#include <vector>

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

    void initialize(const uint32_t &width = 1280, const uint32_t &height = 640, const bool &debug = false, const bool &resizable = false);
    void cleanup();
    void draw_frame();
    void render();

    // void draw_context_menu();
    // inline VkDeviceSize image_size;
    // inline void* pxdata;
    //
    // void recreate_swap_chain();
    // void create_descriptor_set();
    // void record_command_buffer(VkCommandBuffer commandbuffer, uint32_t image_index);
    //
    // VkCommandBuffer begin_single_time_commands();
    // void end_single_time_commands(VkCommandBuffer &command_buffer);
    // void copy_buffer_to_image(VkBuffer  buffer, VkImage image, const uint32_t &width, const uint32_t &height);


};

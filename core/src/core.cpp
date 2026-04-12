#include "core.h"
#include "konanix.h"
#include "logger.h"
#include <GLFW/glfw3.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <stdlib.h>
#include <thread>
#include <vulkan/vulkan_core.h>
#include <fstream>
#include <chrono>
#include <array>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <algorithm>

#include <gif_lib.h>

// #define STB_ONLY_GIF
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "./third_party/stb_image.h"
// #include <ktx.h>

#include <signal.h>

#include "konata.c"

// #include <GLFW/glfw3.h>
// #include <GLFW/glfw3native.h>

struct GifFrame {
    std::vector<uint8_t> rgba;
    int delay_ms = 100;
};

struct GifAnimation {
    int width = 0;
    int height = 0;
    std::vector<GifFrame> frames;
};

struct MemoryGifReader {
    const unsigned char *data;
    size_t size;
    size_t pos;
};

std::filesystem::path path;
konanix *g_konanix;

// giflib helpers
static std::vector<uint8_t> read_binary_file(const std::filesystem::path& p) {
    std::ifstream file(p, std::ios::binary | std::ios::ate);
    if (!file) {
        logger::log("Failed to open \""+p.string()+"\"!",logger::exc);
        throw std::runtime_error("failed to open file: " + p.string());
    }

    std::streamsize size = file.tellg();
    if (size <= 0) {
        logger::log("File \""+p.string()+"\" is empty!",logger::exc);
        throw std::runtime_error("empty file: " + p.string());
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        logger::log("Failed to read file \""+p.string()+"\"!",logger::exc);
        throw std::runtime_error("failed to read file: " + p.string());
    }
    return data;
}

static std::array<uint8_t, 4> gif_color_at(const ColorMapObject* cmap, int index) {
    if (!cmap || index < 0 || index >= cmap->ColorCount) return {0, 0, 0, 0};
    const GifColorType& c = cmap->Colors[index];
    return {c.Red, c.Green, c.Blue, 255};
}

static int gif_gce_disposal(const SavedImage& img) {
    for (int i = 0; i < img.ExtensionBlockCount; ++i) {
        const ExtensionBlock& eb = img.ExtensionBlocks[i];
        if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
            return (eb.Bytes[0] >> 2) & 0x7;
        }
    }
    return 0;
}

static int gif_gce_delay_ms(const SavedImage& img) {
    for (int i = 0; i < img.ExtensionBlockCount; ++i) {
        const ExtensionBlock& eb = img.ExtensionBlocks[i];
        if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
            int hundredths = eb.Bytes[1] | (eb.Bytes[2] << 8);
            return std::max(10, hundredths * 10);
        }
    }
    return 100;
}

static int gif_gce_transparent_index(const SavedImage& img) {
    for (int i = 0; i < img.ExtensionBlockCount; ++i) {
        const ExtensionBlock& eb = img.ExtensionBlocks[i];
        if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
            const bool has_transparency = (eb.Bytes[0] & 0x01) != 0;
            return has_transparency ? static_cast<int>(eb.Bytes[3]) : -1;
        }
    }
    return -1;
}

static void fill_canvas(std::vector<uint8_t>& canvas, const std::array<uint8_t, 4>& rgba) {
    for (size_t i = 0; i + 3 < canvas.size(); i += 4) {
        canvas[i + 0] = rgba[0];
        canvas[i + 1] = rgba[1];
        canvas[i + 2] = rgba[2];
        canvas[i + 3] = rgba[3];
    }
}

static void clear_rect_to_bg(std::vector<uint8_t>& canvas,
                             int canvas_w, int canvas_h,
                             int left, int top, int w, int h,
                             const std::array<uint8_t, 4>& bg) {
    for (int y = 0; y < h; ++y) {
        const int dy = top + y;
        if (dy < 0 || dy >= canvas_h) continue;
        for (int x = 0; x < w; ++x) {
            const int dx = left + x;
            if (dx < 0 || dx >= canvas_w) continue;
            const size_t p = static_cast<size_t>(dy * canvas_w + dx) * 4;
            // canvas[p + 0] = bg[0];
            // canvas[p + 1] = bg[1];
            // canvas[p + 2] = bg[2];
            // canvas[p + 3] = bg[3];
            canvas[p + 0] = 0;
            canvas[p + 1] = 0;
            canvas[p + 2] = 0;
            canvas[p + 3] = 0;

        }
    }
}

static void draw_indexed_frame(std::vector<uint8_t>& canvas,
                               int canvas_w, int canvas_h,
                               const SavedImage& img,
                               const ColorMapObject* cmap,
                               int transparent_index) {
    const int left = img.ImageDesc.Left;
    const int top  = img.ImageDesc.Top;
    const int w    = img.ImageDesc.Width;
    const int h    = img.ImageDesc.Height;

    const GifByteType* src = img.RasterBits;

    for (int y = 0; y < h; ++y) {
        const int dy = top + y;
        if (dy < 0 || dy >= canvas_h) continue;

        for (int x = 0; x < w; ++x) {
            const int dx = left + x;
            if (dx < 0 || dx >= canvas_w) continue;

            const int idx = src[y * w + x];
            if (idx == transparent_index) continue;

            const auto c = gif_color_at(cmap, idx);
            const size_t p = static_cast<size_t>(dy * canvas_w + dx) * 4;
            canvas[p + 0] = c[0];
            canvas[p + 1] = c[1];
            canvas[p + 2] = c[2];
            canvas[p + 3] = 255;
        }
    }
}

static int read_from_memory(GifFileType *gif, GifByteType *dst, int len)
{
    MemoryGifReader *r = (MemoryGifReader *)gif->UserData;
    size_t remaining = r->size - r->pos;

    if (remaining == 0)
        return 0; // eof

    if ((size_t)len > remaining)
        len = (int)remaining;

    memcpy(dst, r->data + r->pos, (size_t)len);
    r->pos += (size_t)len;
    return len;
}

GifFileType *open_gif_from_memory(const unsigned char *gif_bytes,
                                  size_t gif_size,
                                  int *err)
{
    MemoryGifReader *reader = static_cast<MemoryGifReader*>(malloc(sizeof(*reader)));
    if (!reader)
        return NULL;

    reader->data = gif_bytes;
    reader->size = gif_size;
    reader->pos = 0;

    GifFileType *gif = DGifOpen(reader, read_from_memory, err);
    if (!gif) {
        free(reader);
        return NULL;
    }

    gif->UserData = reader;
    return gif;
}

void close_gif_from_memory(GifFileType *gif)
{
    if (!gif) return;
    free(gif->UserData);
    DGifCloseFile(gif, NULL);
}

// main GIF loader
static GifAnimation load_gif_animation(const std::filesystem::path& path) {
    int err = 0;
    GifFileType* gif;
    if (!path.empty())
        gif = DGifOpenFileName(path.string().c_str(), &err);
    else
        gif = open_gif_from_memory(konata, konata_len, &err);
    if (!gif) {
        logger::log("DGifOpenFileName failed for \""+path.string()+"\"! Exception details: "+std::to_string(err),logger::exc);
        throw std::runtime_error("DGifOpenFileName failed for: " + path.string() + " err=" + std::to_string(err));
    }

    if (DGifSlurp(gif) == GIF_ERROR) {
        int close_err = 0;
        DGifCloseFile(gif, &close_err);
        logger::log("DGifSlurp failed for \""+path.string()+"\"!",logger::exc);
        throw std::runtime_error("DGifSlurp failed for: " + path.string());
    }

    GifAnimation anim;
    anim.width = gif->SWidth;
    anim.height = gif->SHeight;
    anim.frames.reserve(std::max(0, gif->ImageCount));

    std::vector<uint8_t> canvas(static_cast<size_t>(anim.width) * anim.height * 4, 0);

    // std::array<uint8_t, 4> bg = {0, 0, 0, 0};
    // if (gif->SColorMap &&
    //     gif->SBackGroundColor >= 0 &&
    //     gif->SBackGroundColor < gif->SColorMap->ColorCount) {
    //     bg = gif_color_at(gif->SColorMap, gif->SBackGroundColor);
    // }
    fill_canvas(canvas, {0,0,0,0});

    for (int i = 0; i < gif->ImageCount; ++i) {
        const SavedImage& img = gif->SavedImages[i];
        const ColorMapObject* cmap = img.ImageDesc.ColorMap ? img.ImageDesc.ColorMap : gif->SColorMap;
        if (!cmap) {
            int close_err = 0;
            DGifCloseFile(gif, &close_err);
            logger::log("GIF frame (i="+std::to_string(i)+") has no color map!",logger::exc);
            throw std::runtime_error("GIF frame has no color map");
        }

        const int disposal = gif_gce_disposal(img);
        const int delay_ms = gif_gce_delay_ms(img);
        const int transparent_index = gif_gce_transparent_index(img);

        std::vector<uint8_t> before = canvas; // for disposal 3
        draw_indexed_frame(canvas, anim.width, anim.height, img, cmap, transparent_index);

        GifFrame frame;
        frame.delay_ms = delay_ms;
        frame.rgba = canvas;
        anim.frames.push_back(std::move(frame));

        if (disposal == 2) {
            clear_rect_to_bg(canvas,
                             anim.width, anim.height,
                             img.ImageDesc.Left, img.ImageDesc.Top,
                             img.ImageDesc.Width, img.ImageDesc.Height,
                             {0,0,0,0});
        } else if (disposal == 3) {
            canvas.swap(before);
        }
    }

    int close_err = 0;
    DGifCloseFile(gif, &close_err);
    return anim;
}

VkImage g_gif_image = VK_NULL_HANDLE;
VkDeviceMemory g_gif_image_memory = VK_NULL_HANDLE;
VkImageView g_gif_image_view = VK_NULL_HANDLE;
VkSampler g_gif_sampler = VK_NULL_HANDLE;

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

void konanix::create_image(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
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

    if (vkCreateImage(g_device,&image_info,nullptr,&image) != VK_SUCCESS) {
        logger::log("Failed to create image!",logger::exc);
        throw std::runtime_error("failed to create image");
    }
    logger::log("Vulkan image created!",logger::dbg);
    VkMemoryRequirements mem_requirements{};
    vkGetImageMemoryRequirements(g_device, image, &mem_requirements);

    const VkMemoryAllocateInfo alloc_info{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        VK_NULL_HANDLE,
        mem_requirements.size,
        find_memory_type(mem_requirements.memoryTypeBits, properties)
    };

    if (vkAllocateMemory(g_device, &alloc_info, nullptr, &image_memory) != VK_SUCCESS) {
        vkDestroyImage(g_device,image,nullptr);
        image = VK_NULL_HANDLE;
        logger::log("Failed to allocate image memory!",logger::exc);
        throw std::runtime_error("failed to allocate image memory");
    }
    if (vkBindImageMemory(g_device, image, image_memory, 0) != VK_SUCCESS) {
        vkFreeMemory(g_device, image_memory, nullptr);
        vkDestroyImage(g_device, image, nullptr);
        image = VK_NULL_HANDLE;
        image_memory = VK_NULL_HANDLE;
        logger::log("Failed to bind image memory!",logger::exc);
        throw std::runtime_error("failed to bind image memory");
    }
    logger::log("Allocated memory for Vulkan image!",logger::dbg);
}

VkImageView konanix::create_image_view(VkImage image, VkFormat format) {
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
    if (vkCreateImageView(g_device,&view_info,nullptr,&image_view) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image view");
    } 

    return image_view;
}

VkSampler konanix::create_sampler() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g_physicaldevice,&properties);

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
    if (vkCreateSampler(g_device,&sampler_info,nullptr,&sampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create sampler");
    }
    return sampler;
}

void konanix::create_descriptor_pool() {
    std::array<VkDescriptorPoolSize, 1> pool_sizes{{
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 }
    }};

    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr,
        0,
        4,
        static_cast<uint32_t>(pool_sizes.size()),
        pool_sizes.data()
    };

    if (vkCreateDescriptorPool(g_device, &pool_info, nullptr, &g_descriptor_pool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor pool");
    }
}

void konanix::create_descriptor_set() {
    VkDescriptorSetLayout layouts[] = { g_descriptor_set_layout };

    VkDescriptorSetAllocateInfo alloc_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr,
        g_descriptor_pool,
        1,
        layouts
    };

    VkResult res = vkAllocateDescriptorSets(g_device, &alloc_info, &g_descriptor_set);
    if (res != VK_SUCCESS) {
        throw std::runtime_error("vkAllocateDescriptorSets failed with code " + std::to_string((int)res));
    }

    VkDescriptorImageInfo image_info{
        g_gif_sampler,
        g_gif_image_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        nullptr,
        g_descriptor_set,
        0,
        0,
        1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        &image_info,
        nullptr,
        nullptr
    };

    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}

void konanix::create_descriptor_set_layout() {
    constexpr VkDescriptorSetLayoutBinding gif_binding {
        0,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        1,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        nullptr
    };

    const VkDescriptorSetLayoutCreateInfo layout_info {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        VK_NULL_HANDLE,
        0,
        1,
        &gif_binding
    };

    if (vkCreateDescriptorSetLayout(g_device,&layout_info,nullptr,&g_descriptor_set_layout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor set");
    }
}

void konanix::create_gif_image(uint32_t width, uint32_t height) {
    constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    create_image(width,height,format,
    VK_IMAGE_TILING_OPTIMAL,
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    g_gif_image,g_gif_image_memory);

    g_gif_image_view = create_image_view(g_gif_image, format);
    g_gif_sampler = create_sampler();
}

static std::vector<uint8_t> read_binary_file(const std::string& path) {
    logger::log("Parsing \""+path+"\"...",logger::dbg);

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        logger::log("Unable to open \""+path+"\"!",logger::exc);
        throw std::runtime_error("failed to open file: " + path);
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        logger::log("File \""+path+"\" is empty!",logger::exc);
        throw std::runtime_error("empty file: " + path);
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        logger::log("Unable to read \""+path+"\"!",logger::exc);
        throw std::runtime_error("failed to read file: " + path);
    }
    logger::log("Read \""+path+"\"!",logger::dbg);
    return data;
}

VkCommandBuffer konanix::begin_single_time_commands() {
    VkCommandBufferAllocateInfo alloc_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr,
        g_commandpool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        1
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_device, &alloc_info, &cmd) != VK_SUCCESS) {
        logger::log("Failed to allocate transient command buffer!",logger::exc);
        throw std::runtime_error("failed to allocate transient command buffer");
    }

    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr
    };

    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_device, g_commandpool, 1, &cmd);
        logger::log("Failed to begin transient command buffer!",logger::exc);
        throw std::runtime_error("failed to begin transient command buffer");
    }

    return cmd;
}

void konanix::end_single_time_commands(VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_device, g_commandpool, 1, &cmd);
        logger::log("Failed to end transient command buffer!",logger::exc);
        throw std::runtime_error("failed to end transient command buffer");
    }

    VkSubmitInfo submit_info{
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        0, nullptr, nullptr,
        1, &cmd,
        0, nullptr
    };

    if (vkQueueSubmit(g_graphicsqueue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_device, g_commandpool, 1, &cmd);
        logger::log("Failed to submit transient command buffer!",logger::exc);
        throw std::runtime_error("failed to submit transient command buffer");
    }

    vkQueueWaitIdle(g_graphicsqueue);
    vkFreeCommandBuffers(g_device, g_commandpool, 1, &cmd);
}

void konanix::transition_image_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout) {
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
        vkFreeCommandBuffers(g_device, g_commandpool, 1, &cmd);
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

void konanix::copy_buffer_to_image(
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

void konanix::upload_rgba_frame_to_gif_image(const uint8_t* rgba_pixels, size_t pixel_bytes, uint32_t width, uint32_t height, bool first_upload) {
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;

    create_buffer(
        static_cast<VkDeviceSize>(pixel_bytes),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging_buffer,
        staging_memory
    );
    // logger::log("Created buffer for GIF frame",logger::dbg);

    void* mapped = nullptr;
    if (vkMapMemory(g_device, staging_memory, 0, pixel_bytes, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(g_device, staging_buffer, nullptr);
        vkFreeMemory(g_device, staging_memory, nullptr);
        logger::log("Failed to map staging memory!",logger::exc);
        throw std::runtime_error("failed to map staging memory");
    }

    memcpy(mapped, rgba_pixels, pixel_bytes);
    vkUnmapMemory(g_device, staging_memory);
    // logger::log("Moved GIF pixel data to \"pixel_bytes\"!",logger::dbg);

    const VkImageLayout from_layout = first_upload
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    transition_image_layout(
        g_gif_image,
        from_layout,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    );

    copy_buffer_to_image(
        staging_buffer,
        g_gif_image,
        width,
        height
    );
    // logger::log("Successfully copied buffer to image!",logger::dbg);

    transition_image_layout(
        g_gif_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    );

    vkDestroyBuffer(g_device, staging_buffer, nullptr);
    vkFreeMemory(g_device, staging_memory, nullptr);
    // logger::log("Freed up unneeded memory!",logger::dbg);
}

void terminate_handler(int s) {
    logger::log("Caught signal "+std::to_string(s)+"! Terminating...");

    if (g_gif_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(g_konanix->get_device(), g_gif_sampler, nullptr);
        g_gif_sampler = VK_NULL_HANDLE;
    }

    if (g_gif_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(g_konanix->get_device(), g_gif_image_view, nullptr);
        g_gif_image_view = VK_NULL_HANDLE;
    }

    if (g_gif_image != VK_NULL_HANDLE) {
        vkDestroyImage(g_konanix->get_device(), g_gif_image, nullptr);
        g_gif_image = VK_NULL_HANDLE;
    }

    if (g_gif_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_konanix->get_device(), g_gif_image_memory, nullptr);
        g_gif_image_memory = VK_NULL_HANDLE;
    }
    g_konanix->cleanup();
    logger::log("Terminated successfully!");
    exit(0);
}

int main(int argc, char *argv[]) {

    printf("[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;7;7;7m [0m[38;2;25;19;16m [0m[38;2;33;23;18m [0m[38;2;42;31;26m.[0m[38;2;28;20;17m [0m[38;2;14;10;9m [0m[38;2;2;2;2m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;4;3;2m [0m[38;2;42;30;24m.[0m[38;2;80;55;41m.[0m[38;2;104;70;51m,[0m[38;2;108;72;51m,[0m[38;2;110;72;52m,[0m[38;2;108;71;51m,[0m[38;2;106;70;51m,[0m[38;2;97;64;46m'[0m[38;2;87;58;42m'[0m[38;2;63;42;31m.[0m[38;2;35;24;18m [0m[38;2;8;6;5m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;3;3;3m [0m[38;2;41;29;23m.[0m[38;2;107;76;58m,[0m[38;2;120;81;59m;[0m[38;2;111;73;52m,[0m[38;2;105;68;49m,[0m[38;2;105;68;49m,[0m[38;2;113;74;55m,[0m[38;2;113;74;55m,[0m[38;2;105;68;49m,[0m[38;2;104;67;48m,[0m[38;2;104;67;48m,[0m[38;2;103;68;49m,[0m[38;2;103;68;49m,[0m[38;2;93;61;44m'[0m[38;2;62;41;30m.[0m[38;2;18;11;9m [0m[38;2;1;1;1m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;4;3;2m [0m[38;2;72;52;39m.[0m[38;2;122;86;64m;[0m[38;2;105;68;48m,[0m[38;2;108;69;50m,[0m[38;2;148;100;79mc[0m[38;2;192;134;112md[0m[38;2;214;150;127mx[0m[38;2;222;157;133mk[0m[38;2;223;158;134mk[0m[38;2;220;155;132mk[0m[38;2;194;135;114md[0m[38;2;157;107;85mc[0m[38;2;124;82;63m;[0m[38;2;103;67;48m'[0m[38;2;104;67;49m,[0m[38;2;103;68;49m,[0m[38;2;99;65;47m'[0m[38;2;62;41;30m.[0m[38;2;7;5;4m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;7;4;4m [0m[38;2;91;66;50m'[0m[38;2;128;91;66m:[0m[38;2;96;62;42m'[0m[38;2;104;66;46m'[0m[38;2;170;116;93ml[0m[38;2;222;156;132mk[0m[38;2;224;158;134mk[0m[38;2;224;158;134mk[0m[38;2;224;158;134mk[0m[38;2;223;157;133mk[0m[38;2;223;157;133mk[0m[38;2;217;152;129mk[0m[38;2;200;139;115md[0m[38;2;196;136;112md[0m[38;2;166;114;92ml[0m[38;2;109;71;53m,[0m[38;2;104;67;49m,[0m[38;2;103;68;49m,[0m[38;2;103;68;49m,[0m[38;2;90;60;44m'[0m[38;2;18;12;10m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;2;2;2m [0m[38;2;85;61;47m'[0m[38;2;142;106;81mc[0m[38;2;129;92;67m:[0m[38;2;94;60;41m'[0m[38;2;122;80;60m;[0m[38;2;214;149;124mx[0m[38;2;224;158;134mk[0m[38;2;224;158;134mk[0m[38;2;224;158;134mk[0m[38;2;224;158;134mk[0m[38;2;222;156;132mk[0m[38;2;199;139;116md[0m[38;2;196;136;113md[0m[38;2;196;136;112md[0m[38;2;196;136;112md[0m[38;2;196;136;112md[0m[38;2;182;125;104mo[0m[38;2;107;70;52m,[0m[38;2;103;68;49m,[0m[38;2;102;67;48m,[0m[38;2;106;71;51m,[0m[38;2;68;44;32m.[0m[38;2;20;16;14m [0m[38;2;16;12;9m [0m[38;2;24;17;13m [0m[38;2;29;21;16m [0m[38;2;34;25;19m [0m[38;2;44;33;27m.[0m[38;2;44;33;27m.[0m[38;2;53;40;33m.[0m[38;2;49;37;31m.[0m[38;2;39;29;24m.[0m[38;2;18;13;11m [0m[38;2;5;4;4m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;31;22;17m [0m[38;2;146;108;84mc[0m[38;2;138;101;76m:[0m[38;2;133;96;71m:[0m[38;2;97;62;43m'[0m[38;2;135;89;68m:[0m[38;2;206;143;118mx[0m[38;2;217;152;127mk[0m[38;2;217;152;128mk[0m[38;2;215;151;126mx[0m[38;2;211;147;123mx[0m[38;2;187;129;106mo[0m[38;2;176;121;99mo[0m[38;2;196;136;112md[0m[38;2;196;136;112md[0m[38;2;175;121;99mo[0m[38;2;151;103;84mc[0m[38;2;137;92;75m:[0m[38;2;107;71;54m,[0m[38;2;86;56;41m.[0m[38;2;81;70;45m'[0m[38;2;89;99;57m;[0m[38;2;125;134;98ml[0m[38;2;148;144;121mo[0m[38;2;104;96;62m;[0m[38;2;117;83;65m;[0m[38;2;120;87;68m;[0m[38;2;109;82;66m;[0m[38;2;93;70;57m,[0m[38;2;102;77;64m,[0m[38;2;117;90;75m;[0m[38;2;117;91;75m;[0m[38;2;125;97;81m:[0m[38;2;123;94;79m:[0m[38;2;104;78;65m,[0m[38;2;77;55;44m.[0m[38;2;51;36;29m.[0m[38;2;24;17;14m [0m[38;2;5;4;3m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;7;10;14m [0m[38;2;36;54;81m.[0m[38;2;45;69;106m'[0m[38;2;45;69;105m'[0m[38;2;46;70;106m'[0m[38;2;42;63;96m.[0m[38;2;35;53;80m.[0m[38;2;27;40;61m.[0m[38;2;15;22;35m [0m[38;2;2;2;3m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;5;5;5m [0m[38;2;98;70;54m,[0m[38;2;139;102;77mc[0m[38;2;135;99;74m:[0m[38;2;135;99;74m:[0m[38;2;117;80;57m;[0m[38;2;109;70;52m,[0m[38;2;200;138;113md[0m[38;2;211;147;123mx[0m[38;2;210;146;122mx[0m[38;2;200;138;113md[0m[38;2;195;134;109md[0m[38;2;176;121;99mo[0m[38;2;146;99;81mc[0m[38;2;121;80;63m;[0m[38;2;98;63;47m'[0m[38;2;99;65;46m'[0m[38;2;108;72;51m,[0m[38;2;97;83;53m,[0m[38;2;106;129;72mc[0m[38;2;75;101;53m;[0m[38;2;113;166;88mo[0m[38;2;134;201;106mk[0m[38;2;149;207;125mO[0m[38;2;209;231;200mX[0m[38;2;94;132;71mc[0m[38;2;97;118;63m:[0m[38;2;108;89;67m;[0m[38;2;123;95;77m:[0m[38;2;114;87;72m;[0m[38;2;114;87;73m;[0m[38;2;73;55;45m.[0m[38;2;118;92;76m;[0m[38;2;123;95;80m:[0m[38;2;125;96;82m:[0m[38;2;124;96;82m:[0m[38;2;121;93;79m:[0m[38;2;116;86;71m;[0m[38;2;113;83;68m;[0m[38;2;97;69;56m,[0m[38;2;72;51;42m.[0m[38;2;41;30;24m.[0m[38;2;13;10;9m [0m[38;2;2;2;2m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;4;4;4m [0m[38;2;39;53;77m.[0m[38;2;71;108;165m:[0m[38;2;76;116;177mc[0m[38;2;76;116;177mc[0m[38;2;75;114;173m:[0m[38;2;70;106;161m:[0m[38;2;66;100;152m;[0m[38;2;65;98;149m;[0m[38;2;65;98;149m;[0m[38;2;67;101;152m;[0m[38;2;56;84;128m,[0m[38;2;54;91;150m,[0m[38;2;14;22;37m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;23;23;23m [0m[38;2;118;86;66m;[0m[38;2;135;99;75m:[0m[38;2;135;99;75m:[0m[38;2;135;99;75m:[0m[38;2;133;97;73m:[0m[38;2;101;67;47m'[0m[38;2;170;116;94ml[0m[38;2;216;151;127mx[0m[38;2;213;149;124mx[0m[38;2;169;115;94ml[0m[38;2;119;78;61m;[0m[38;2;92;59;42m'[0m[38;2;107;69;49m,[0m[38;2;112;74;53m,[0m[38;2;116;78;55m,[0m[38;2;93;78;50m,[0m[38;2;96;128;69mc[0m[38;2;99;142;75mc[0m[38;2;85;120;64m:[0m[38;2;61;81;43m'[0m[38;2;101;146;77ml[0m[38;2;76;104;55m;[0m[38;2;143;185;124mx[0m[38;2;111;136;95ml[0m[38;2;113;156;80mo[0m[38;2;104;140;69mc[0m[38;2;75;83;48m,[0m[38;2;126;97;80m:[0m[38;2;126;97;81m:[0m[38;2;122;93;78m:[0m[38;2;85;65;54m'[0m[38;2;100;77;64m,[0m[38;2;82;62;52m'[0m[38;2;118;91;78m;[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;121;94;80m:[0m[38;2;112;86;73m;[0m[38;2;111;85;73m;[0m[38;2;105;78;65m,[0m[38;2;105;79;67m,[0m[38;2;101;77;65m,[0m[38;2;82;65;56m'[0m[38;2;41;34;31m.[0m[38;2;37;31;29m.[0m[38;2;34;30;28m.[0m[38;2;50;58;75m.[0m[38;2;68;102;154m;[0m[38;2;73;112;171m:[0m[38;2;69;104;157m:[0m[38;2;68;102;154m;[0m[38;2;67;99;149m;[0m[38;2;37;55;82m.[0m[38;2;84;84;84m [0m[38;2;78;78;78m [0m[38;2;80;80;80m [0m[38;2;82;82;82m [0m[38;2;42;62;94m.[0m[38;2;88;144;237mo[0m[38;2;84;138;229ml[0m[38;2;24;37;60m.[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;22;22;22m [0m[38;2;102;74;57m,[0m[38;2;135;99;75m:[0m[38;2;135;99;75m:[0m[38;2;134;100;75m:[0m[38;2;134;100;75m:[0m[38;2;128;91;67m:[0m[38;2;122;82;61m;[0m[38;2;135;91;73m:[0m[38;2;105;68;53m,[0m[38;2;96;61;43m'[0m[38;2;111;72;52m,[0m[38;2;114;76;54m,[0m[38;2;117;79;56m;[0m[38;2;117;79;56m;[0m[38;2;84;70;45m'[0m[38;2;128;188;99mx[0m[38;2;131;194;103mx[0m[38;2;101;143;76ml[0m[38;2;77;107;57m;[0m[38;2;136;201;109mk[0m[38;2;150;203;128mk[0m[38;2;129;163;110md[0m[38;2;102;130;77mc[0m[38;2;108;146;73ml[0m[38;2;104;139;69mc[0m[38;2;82;102;53m;[0m[38;2;103;80;65m,[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;98;81m:[0m[38;2;125;98;81m:[0m[38;2;112;86;73m;[0m[38;2;106;81;69m;[0m[38;2;74;56;47m.[0m[38;2;127;101;87m:[0m[38;2;146;123;109ml[0m[38;2;151;130;116mo[0m[38;2;165;142;128md[0m[38;2;173;149;135md[0m[38;2;175;152;139mx[0m[38;2;175;152;139mx[0m[38;2;172;148;135md[0m[38;2;174;152;138mx[0m[38;2;172;150;136md[0m[38;2;176;156;142mx[0m[38;2;177;157;143mx[0m[38;2;177;157;143mx[0m[38;2;169;149;135md[0m[38;2;142;123;113ml[0m[38;2;111;92;81m;[0m[38;2;89;76;70m,[0m[38;2;58;52;52m.[0m[38;2;37;31;30m.[0m[38;2;10;9;9m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;3;3;3m [0m[38;2;60;92;148m;[0m[38;2;88;144;237mo[0m[38;2;89;145;236mo[0m[38;2;73;119;195mc[0m[38;2;12;12;12m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;6;6;6m [0m[38;2;71;51;39m.[0m[38;2;136;101;76m:[0m[38;2;134;100;75m:[0m[38;2;134;100;75m:[0m[38;2;134;100;75m:[0m[38;2;133;99;76m:[0m[38;2;126;89;66m;[0m[38;2;94;61;43m'[0m[38;2;96;61;44m'[0m[38;2;107;70;50m,[0m[38;2;116;78;55m,[0m[38;2;117;79;56m;[0m[38;2;119;82;60m;[0m[38;2;122;86;64m;[0m[38;2;100;91;63m;[0m[38;2;162;198;143mO[0m[38;2;160;207;138mO[0m[38;2;122;176;98md[0m[38;2;125;171;102md[0m[38;2;112;152;86ml[0m[38;2;102;137;72mc[0m[38;2;107;143;72ml[0m[38;2;103;138;68mc[0m[38;2;93;123;61m:[0m[38;2;85;87;54m,[0m[38;2;113;87;72m;[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;98;81m:[0m[38;2;125;98;81m:[0m[38;2;125;96;82m:[0m[38;2;124;96;82m:[0m[38;2;118;91;78m;[0m[38;2;93;71;61m,[0m[38;2;108;84;70m;[0m[38;2;139;112;96mc[0m[38;2;159;134;118mo[0m[38;2;169;144;130md[0m[38;2;170;144;131md[0m[38;2;170;144;131md[0m[38;2;170;144;131md[0m[38;2;169;142;129md[0m[38;2;164;137;123mo[0m[38;2;162;136;120mo[0m[38;2;164;138;124mo[0m[38;2;165;140;126md[0m[38;2;165;140;126md[0m[38;2;164;138;124mo[0m[38;2;165;139;125md[0m[38;2;162;134;120mo[0m[38;2;152;123;107ml[0m[38;2;122;95;80m:[0m[38;2;60;44;36m.[0m[38;2;45;32;25m.[0m[38;2;5;4;5m [0m[38;2;0;0;0m [0m[38;2;32;46;72m.[0m[38;2;87;143;231mo[0m[38;2;90;146;236mo[0m[38;2;90;146;236mo[0m[38;2;70;113;182m:[0m[38;2;21;21;21m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;39;28;22m.[0m[38;2;144;109;84mc[0m[38;2;133;99;76m:[0m[38;2;133;98;76m:[0m[38;2;133;98;76m:[0m[38;2;133;98;76m:[0m[38;2;127;90;68m:[0m[38;2;117;79;56m;[0m[38;2;103;68;48m,[0m[38;2;95;62;44m'[0m[38;2;115;78;56m,[0m[38;2;118;82;60m;[0m[38;2;125;90;69m;[0m[38;2;130;97;77m:[0m[38;2;129;96;76m:[0m[38;2;108;94;63m;[0m[38;2;108;136;74mc[0m[38;2;126;181;95md[0m[38;2;121;167;85mo[0m[38;2;114;154;77ml[0m[38;2;95;123;63m:[0m[38;2;92;104;59m;[0m[38;2;97;91;61m;[0m[38;2;110;85;69m;[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;98;81m:[0m[38;2;125;97;82m:[0m[38;2;125;96;82m:[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;120;92;77m:[0m[38;2;93;69;57m'[0m[38;2;56;43;38m.[0m[38;2;50;47;52m.[0m[38;2;75;70;75m'[0m[38;2;85;78;81m,[0m[38;2;97;85;85m;[0m[38;2;113;96;89m:[0m[38;2;128;102;89m:[0m[38;2;131;101;85m:[0m[38;2;129;100;84m:[0m[38;2;133;103;86mc[0m[38;2;136;106;88mc[0m[38;2;136;106;89mc[0m[38;2;135;104;88mc[0m[38;2;134;104;87mc[0m[38;2;131;101;85m:[0m[38;2;129;100;83m:[0m[38;2;125;97;80m:[0m[38;2;104;79;65m,[0m[38;2;79;59;48m.[0m[38;2;9;9;10m [0m[38;2;37;51;77m.[0m[38;2;86;139;224ml[0m[38;2;91;146;236mo[0m[38;2;91;146;236mo[0m[38;2;87;136;215ml[0m[38;2;6;9;14m [0m[38;2;1;1;1m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;8;6;5m [0m[38;2;143;108;85mc[0m[38;2;145;110;86mc[0m[38;2;133;98;76m:[0m[38;2;133;98;76m:[0m[38;2;133;98;77m:[0m[38;2;126;90;69m:[0m[38;2;117;79;57m;[0m[38;2;113;76;55m,[0m[38;2;93;61;44m'[0m[38;2;88;59;43m'[0m[38;2;118;82;60m;[0m[38;2;129;96;76m:[0m[38;2;130;97;78m:[0m[38;2;129;97;78m:[0m[38;2;129;97;78m:[0m[38;2;117;87;70m;[0m[38;2;104;83;64m;[0m[38;2;114;94;72m;[0m[38;2;113;87;70m;[0m[38;2;124;96;78m:[0m[38;2;126;97;80m:[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;97;81m:[0m[38;2;125;98;81m:[0m[38;2;125;97;82m:[0m[38;2;125;96;82m:[0m[38;2;124;96;82m:[0m[38;2;124;95;81m:[0m[38;2;119;90;76m;[0m[38;2;101;81;75m,[0m[38;2;85;85;102m,[0m[38;2;69;89;129m;[0m[38;2;67;103;160m:[0m[38;2;79;122;191mc[0m[38;2;81;126;198mc[0m[38;2;81;126;198mc[0m[38;2;78;119;182mc[0m[38;2;75;113;170m:[0m[38;2;63;93;138m;[0m[38;2;58;83;124m,[0m[38;2;64;77;104m,[0m[38;2;90;83;88m,[0m[38;2;100;89;92m;[0m[38;2;96;87;90m;[0m[38;2;99;87;88m;[0m[38;2;104;87;81m;[0m[38;2;112;87;74m;[0m[38;2;118;90;74m;[0m[38;2;124;95;78m:[0m[38;2;100;75;62m,[0m[38;2;129;98;80m:[0m[38;2;125;95;78m:[0m[38;2;72;87;121m,[0m[38;2;89;143;230mo[0m[38;2;91;145;235mo[0m[38;2;89;140;222mo[0m[38;2;57;87;135m,[0m[38;2;54;54;54m [0m[38;2;4;4;4m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;24;17;14m [0m[38;2;100;72;56m,[0m[38;2;148;112;88mc[0m[38;2;139;104;82mc[0m[38;2;132;99;78m:[0m[38;2;126;92;71m:[0m[38;2;118;81;60m;[0m[38;2;116;79;58m;[0m[38;2;115;79;57m;[0m[38;2;97;65;47m'[0m[38;2;70;48;37m.[0m[38;2;126;94;76m:[0m[38;2;129;97;78m:[0m[38;2;129;97;78m:[0m[38;2;128;97;79m:[0m[38;2;128;97;79m:[0m[38;2;113;86;70m;[0m[38;2;116;89;72m;[0m[38;2;126;97;79m:[0m[38;2;126;97;80m:[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;97;81m:[0m[38;2;125;97;81m:[0m[38;2;125;96;82m:[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;121;92;78m:[0m[38;2;101;82;78m;[0m[38;2;87;92;117m;[0m[38;2;78;110;164m:[0m[38;2;89;135;208ml[0m[38;2;91;146;233mo[0m[38;2;88;142;231mo[0m[38;2;79;125;197mc[0m[38;2;82;130;204ml[0m[38;2;88;144;235mo[0m[38;2;88;144;237mo[0m[38;2;88;144;237mo[0m[38;2;87;143;233mo[0m[38;2;80;124;191mc[0m[38;2;79;122;186mc[0m[38;2;78;119;180mc[0m[38;2;79;120;183mc[0m[38;2;81;125;194mc[0m[38;2;82;129;202ml[0m[38;2;84;131;206ml[0m[38;2;84;131;205ml[0m[38;2;77;115;173mc[0m[38;2;76;84;106m,[0m[38;2;129;98;80m:[0m[38;2;104;79;64m,[0m[38;2;109;93;91m;[0m[38;2;82;106;151m:[0m[38;2;83;124;190mc[0m[38;2;82;121;182mc[0m[38;2;75;100;143m;[0m[38;2;5;6;8m [0m[38;2;12;12;12m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;1;1;1m [0m[38;2;36;25;20m.[0m[38;2;98;66;48m'[0m[38;2;83;57;42m.[0m[38;2;148;112;90mc[0m[38;2;150;115;93ml[0m[38;2;149;113;93mc[0m[38;2;134;101;81m:[0m[38;2;120;84;64m;[0m[38;2;114;78;57m,[0m[38;2;109;74;55m,[0m[38;2;75;50;37m.[0m[38;2;102;76;61m,[0m[38;2;128;97;79m:[0m[38;2;128;97;79m:[0m[38;2;127;97;79m:[0m[38;2;126;97;79m:[0m[38;2;126;97;79m:[0m[38;2;126;97;80m:[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;98;81m:[0m[38;2;125;97;81m:[0m[38;2;125;97;82m:[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;122;93;79m:[0m[38;2;105;82;74m;[0m[38;2;88;90;108m;[0m[38;2;76;103;149m:[0m[38;2;81;125;195mc[0m[38;2;102;147;218mo[0m[38;2;129;171;239mx[0m[38;2;138;179;243mk[0m[38;2;101;139;203mo[0m[38;2;96;76;82m,[0m[38;2;79;92;128m;[0m[38;2;87;141;229mo[0m[38;2;88;144;237mo[0m[38;2;88;144;237mo[0m[38;2;88;144;237mo[0m[38;2;86;140;227mo[0m[38;2;83;132;208ml[0m[38;2;84;133;211ml[0m[38;2;86;139;224ml[0m[38;2;88;144;235mo[0m[38;2;88;144;237mo[0m[38;2;89;145;236mo[0m[38;2;89;146;236mo[0m[38;2;88;142;228mo[0m[38;2;80;120;181mc[0m[38;2;75;83;106m,[0m[38;2;103;83;75m;[0m[38;2;88;82;90m,[0m[38;2;89;98;124m;[0m[38;2;96;97;115m:[0m[38;2;107;96;100m:[0m[38;2;75;55;48m.[0m[38;2;116;87;70m;[0m[38;2;100;77;64m,[0m[38;2;59;45;38m.[0m[38;2;10;7;7m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;41;29;23m.[0m[38;2;113;81;61m;[0m[38;2;116;80;58m;[0m[38;2;104;71;51m,[0m[38;2;101;70;53m,[0m[38;2;136;100;80m:[0m[38;2;136;101;81m:[0m[38;2;125;90;70m;[0m[38;2;114;77;58m,[0m[38;2;112;76;57m,[0m[38;2;84;56;42m.[0m[38;2;86;63;50m'[0m[38;2;127;97;79m:[0m[38;2;126;97;79m:[0m[38;2;126;97;80m:[0m[38;2;126;97;80m:[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;97;81m:[0m[38;2;125;97;81m:[0m[38;2;125;96;81m:[0m[38;2;119;89;74m;[0m[38;2;121;92;77m:[0m[38;2;124;95;81m:[0m[38;2;114;88;76m;[0m[38;2;91;83;89m,[0m[38;2;77;95;130m;[0m[38;2;77;115;173m:[0m[38;2;81;126;194mc[0m[38;2;71;109;167m:[0m[38;2;85;134;212ml[0m[38;2;104;155;239md[0m[38;2;110;160;239md[0m[38;2;93;131;201ml[0m[38;2;178;116;98ml[0m[38;2;209;126;97md[0m[38;2;102;111;150mc[0m[38;2;101;153;237md[0m[38;2;92;147;237mo[0m[38;2;78;126;204mc[0m[38;2;88;144;237mo[0m[38;2;87;143;233mo[0m[38;2;87;143;234mo[0m[38;2;88;144;237mo[0m[38;2;88;144;237mo[0m[38;2;88;144;237mo[0m[38;2;89;145;237mo[0m[38;2;89;146;236mo[0m[38;2;89;146;236mo[0m[38;2;87;139;222ml[0m[38;2;78;116;174mc[0m[38;2;62;87;128m,[0m[38;2;64;88;128m,[0m[38;2;75;80;98m,[0m[38;2;110;89;82m;[0m[38;2;132;99;79m:[0m[38;2;122;92;73m:[0m[38;2;75;50;40m.[0m[38;2;88;59;44m'[0m[38;2;101;71;55m,[0m[38;2;107;76;60m,[0m[38;2;104;75;59m,[0m[38;2;56;41;33m.[0m[38;2;2;1;1m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;38;28;22m.[0m[38;2;121;90;72m;[0m[38;2;120;85;65m;[0m[38;2;115;79;59m;[0m[38;2;115;78;59m;[0m[38;2;120;86;67m;[0m[38;2;99;68;53m,[0m[38;2;81;56;43m.[0m[38;2;96;67;52m'[0m[38;2;96;67;52m'[0m[38;2;103;75;59m,[0m[38;2;76;56;45m.[0m[38;2;113;87;71m;[0m[38;2;126;97;80m:[0m[38;2;126;97;81m:[0m[38;2;126;97;81m:[0m[38;2;125;97;81m:[0m[38;2;125;97;81m:[0m[38;2;125;97;82m:[0m[38;2;124;95;81m:[0m[38;2;120;90;76m;[0m[38;2;113;82;67m;[0m[38;2;105;74;60m,[0m[38;2;91;77;77m,[0m[38;2;80;91;117m;[0m[38;2;67;96;142m;[0m[38;2;77;117;176mc[0m[38;2;78;119;180mc[0m[38;2;79;123;189mc[0m[38;2;64;95;142m;[0m[38;2;58;82;119m,[0m[38;2;82;130;205ml[0m[38;2;88;144;237mo[0m[38;2;85;137;225ml[0m[38;2;173;124;111mo[0m[38;2;228;150;118mk[0m[38;2;218;136;104mx[0m[38;2;106;114;148mc[0m[38;2;137;177;241mk[0m[38;2;115;158;224md[0m[38;2;73;116;188mc[0m[38;2;89;144;237mo[0m[38;2;95;149;238mo[0m[38;2;104;155;239md[0m[38;2;88;144;237mo[0m[38;2;88;144;237mo[0m[38;2;88;144;237mo[0m[38;2;89;145;236mo[0m[38;2;89;146;236mo[0m[38;2;89;146;236mo[0m[38;2;86;135;212ml[0m[38;2;68;99;147m;[0m[38;2;74;106;157m:[0m[38;2;76;110;163m:[0m[38;2;63;88;125m,[0m[38;2;65;90;129m,[0m[38;2;84;88;104m;[0m[38;2;106;76;67m,[0m[38;2;193;135;115md[0m[38;2;196;137;118md[0m[38;2;191;133;116md[0m[38;2;158;109;92ml[0m[38;2;109;73;55m,[0m[38;2;100;66;48m'[0m[38;2;62;42;32m.[0m[38;2;5;5;5m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;34;25;20m.[0m[38;2;119;89;71m;[0m[38;2;128;96;78m:[0m[38;2;121;87;68m;[0m[38;2;116;79;60m;[0m[38;2;119;84;66m;[0m[38;2;127;97;79m:[0m[38;2;132;101;83m:[0m[38;2;121;93;76m:[0m[38;2;125;96;78m:[0m[38;2;112;86;71m;[0m[38;2;73;55;45m.[0m[38;2;110;85;71m;[0m[38;2;102;78;65m,[0m[38;2;120;93;78m:[0m[38;2;125;97;81m:[0m[38;2;125;96;82m:[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;117;87;73m;[0m[38;2;109;78;63m,[0m[38;2;91;70;65m,[0m[38;2;78;82;101m,[0m[38;2;72;102;151m;[0m[38;2;83;131;208ml[0m[38;2;70;107;166m:[0m[38;2;69;103;154m;[0m[38;2;77;117;176mc[0m[38;2;74;112;170m:[0m[38;2;92;92;111m;[0m[38;2;146;99;78mc[0m[38;2;83;96;129m;[0m[38;2;78;121;188mc[0m[38;2;87;144;237mo[0m[38;2;129;126;153ml[0m[38;2;250;186;155m0[0m[38;2;250;185;155m0[0m[38;2;220;145;112mx[0m[38;2;104;106;133m:[0m[38;2;96;137;201ml[0m[38;2;73;109;166m:[0m[38;2;88;143;234mo[0m[38;2;114;163;239mx[0m[38;2;125;170;241mx[0m[38;2;105;156;239md[0m[38;2;110;159;240md[0m[38;2;132;175;242mk[0m[38;2;120;166;240mx[0m[38;2;108;157;238md[0m[38;2;90;147;236mo[0m[38;2;97;151;236mo[0m[38;2;87;132;204ml[0m[38;2;65;92;135m;[0m[38;2;79;114;170m:[0m[38;2;82;121;180mc[0m[38;2;77;110;161m:[0m[38;2;66;90;129m;[0m[38;2;68;92;133m;[0m[38;2;68;90;128m;[0m[38;2;101;105;133m:[0m[38;2;151;130;144mo[0m[38;2;205;147;133mx[0m[38;2;223;157;137mk[0m[38;2;213;150;129mx[0m[38;2;126;84;66m;[0m[38;2;102;65;47m'[0m[38;2;15;12;11m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;31;23;19m [0m[38;2;127;98;80m:[0m[38;2;130;99;81m:[0m[38;2;127;97;79m:[0m[38;2;120;87;69m;[0m[38;2;117;84;66m;[0m[38;2;125;96;78m:[0m[38;2;135;105;87mc[0m[38;2;146;114;96mc[0m[38;2;144;113;95mc[0m[38;2;127;98;82m:[0m[38;2;118;91;76m;[0m[38;2;109;84;71m;[0m[38;2;80;60;50m'[0m[38;2;97;74;63m,[0m[38;2;105;81;69m,[0m[38;2;124;96;82m:[0m[38;2;123;95;81m:[0m[38;2;118;89;74m;[0m[38;2;100;75;65m,[0m[38;2;79;76;88m,[0m[38;2;68;90;127m;[0m[38;2;75;114;172m:[0m[38;2;82;131;206ml[0m[38;2;87;142;233mo[0m[38;2;76;119;189mc[0m[38;2;62;92;138m;[0m[38;2;76;116;176mc[0m[38;2;81;101;140m:[0m[38;2;153;114;102ml[0m[38;2;213;148;114mx[0m[38;2;234;173;139mO[0m[38;2;88;97;126m;[0m[38;2;74;112;171m:[0m[38;2;75;118;186mc[0m[38;2;200;152;127mx[0m[38;2;230;176;147mO[0m[38;2;231;176;147mO[0m[38;2;225;156;123mk[0m[38;2;100;98;118m:[0m[38;2;71;108;164m:[0m[38;2;74;120;192mc[0m[38;2;84;138;221ml[0m[38;2;86;140;223ml[0m[38;2;94;149;238mo[0m[38;2;107;157;239md[0m[38;2;126;170;241mx[0m[38;2;123;168;241mx[0m[38;2;118;165;240mx[0m[38;2;128;171;241mx[0m[38;2;139;178;243mk[0m[38;2;142;180;243mk[0m[38;2;112;150;211md[0m[38;2;65;91;133m;[0m[38;2;82;120;181mc[0m[38;2;85;127;193ml[0m[38;2;83;118;173mc[0m[38;2;69;95;136m;[0m[38;2;76;104;150m:[0m[38;2;80;109;158m:[0m[38;2;71;94;132m;[0m[38;2;81;107;153m:[0m[38;2;105;143;209mo[0m[38;2;129;144;194md[0m[38;2;158;142;165md[0m[38;2;165;126;123mo[0m[38;2;97;62;44m'[0m[38;2;35;25;21m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;1;1;1m [0m[38;2;6;6;6m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;22;17;14m [0m[38;2;128;98;81m:[0m[38;2;130;100;82m:[0m[38;2;125;96;79m:[0m[38;2;122;92;74m:[0m[38;2;126;96;79m:[0m[38;2;130;101;85m:[0m[38;2;140;109;92mc[0m[38;2;130;101;85m:[0m[38;2;137;107;91mc[0m[38;2;134;104;89mc[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;98;75;64m,[0m[38;2;113;87;74m;[0m[38;2;96;74;63m,[0m[38;2;76;57;48m.[0m[38;2;111;83;69m;[0m[38;2;89;74;73m,[0m[38;2;70;81;105m,[0m[38;2;67;100;149m;[0m[38;2;75;114;171m:[0m[38;2;81;128;201ml[0m[38;2;87;142;232mo[0m[38;2;87;143;235mo[0m[38;2;79;127;203mc[0m[38;2;59;88;132m,[0m[38;2;71;102;152m:[0m[38;2;98;90;101m;[0m[38;2;158;110;86mc[0m[38;2;207;153;124mx[0m[38;2;235;182;152m0[0m[38;2;247;192;161mK[0m[38;2;139;125;131ml[0m[38;2;73;113;174m:[0m[38;2;102;112;143mc[0m[38;2;251;194;163mK[0m[38;2;252;195;164mK[0m[38;2;244;189;159m0[0m[38;2;194;134;104md[0m[38;2;89;83;95m,[0m[38;2;62;94;143m;[0m[38;2;79;126;196mc[0m[38;2;79;124;194mc[0m[38;2;86;143;232mo[0m[38;2;88;145;237mo[0m[38;2;90;146;237mo[0m[38;2;91;146;237mo[0m[38;2;90;146;236mo[0m[38;2;97;151;237mo[0m[38;2;115;162;239mx[0m[38;2;125;168;239mx[0m[38;2;109;155;228md[0m[38;2;76;107;156m:[0m[38;2;72;102;149m;[0m[38;2;91;130;191ml[0m[38;2;122;162;231mx[0m[38;2;111;147;208mo[0m[38;2;94;121;169mc[0m[38;2;77;104;151m:[0m[38;2;122;158;221md[0m[38;2;96;126;179ml[0m[38;2;73;96;135m;[0m[38;2;73;94;132m;[0m[38;2;88;122;180mc[0m[38;2;96;138;213mo[0m[38;2;99;144;229mo[0m[38;2;96;136;213ml[0m[38;2;84;112;171mc[0m[38;2;65;88;136m,[0m[38;2;54;73;111m'[0m[38;2;16;21;30m [0m[38;2;26;26;26m [0m[38;2;3;3;3m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;14;11;9m [0m[38;2;121;94;79m:[0m[38;2;131;101;84m:[0m[38;2;126;97;81m:[0m[38;2;113;83;66m;[0m[38;2;125;93;77m:[0m[38;2;144;113;96mc[0m[38;2;130;101;86m:[0m[38;2;133;103;89mc[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;123;95;81m:[0m[38;2;123;95;81m:[0m[38;2;123;95;81m:[0m[38;2;112;87;75m;[0m[38;2;101;78;66m,[0m[38;2;71;52;42m.[0m[38;2;80;73;80m,[0m[38;2;65;84;118m,[0m[38;2;69;103;155m:[0m[38;2;74;112;170m:[0m[38;2;78;121;186mc[0m[38;2;84;139;224ml[0m[38;2;86;145;237mo[0m[38;2;86;145;237mo[0m[38;2;83;137;222ml[0m[38;2;52;76;113m'[0m[38;2;48;46;53m.[0m[38;2;36;30;28m.[0m[38;2;38;28;23m.[0m[38;2;36;29;25m.[0m[38;2;52;44;39m.[0m[38;2;54;47;45m.[0m[38;2;64;63;72m'[0m[38;2;71;74;90m'[0m[38;2;47;63;94m.[0m[38;2;62;66;86m'[0m[38;2;181;144;128md[0m[38;2;239;185;156m0[0m[38;2;243;183;151m0[0m[38;2;212;145;112mx[0m[38;2;103;93;103m;[0m[38;2;63;95;143m;[0m[38;2;73;114;175m:[0m[38;2;83;136;219ml[0m[38;2;86;145;236mo[0m[38;2;86;142;232mo[0m[38;2;87;142;232mo[0m[38;2;88;144;236mo[0m[38;2;88;145;235mo[0m[38;2;88;144;234mo[0m[38;2;87;140;223mo[0m[38;2;82;126;193mc[0m[38;2;83;130;201ml[0m[38;2;62;86;125m,[0m[38;2;78;113;167m:[0m[38;2;93;133;196ml[0m[38;2;140;177;242mk[0m[38;2;144;179;241mk[0m[38;2;121;150;203md[0m[38;2;80;108;156m:[0m[38;2;110;150;219md[0m[38;2;136;173;240mk[0m[38;2;83;112;163m:[0m[38;2;78;101;143m:[0m[38;2;82;97;130m;[0m[38;2;92;94;115m;[0m[38;2;84;90;114m;[0m[38;2;63;71;94m'[0m[38;2;74;74;74m [0m[38;2;61;61;61m [0m[38;2;34;34;34m [0m[38;2;5;5;5m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;11;8;7m [0m[38;2;112;87;73m;[0m[38;2;137;107;90mc[0m[38;2;125;97;82m:[0m[38;2;115;87;74m;[0m[38;2;109;81;67m;[0m[38;2;142;111;96mc[0m[38;2;136;106;92mc[0m[38;2;124;96;82m:[0m[38;2;123;95;81m:[0m[38;2;123;95;82m:[0m[38;2;123;95;82m:[0m[38;2;122;96;82m:[0m[38;2;122;96;83m:[0m[38;2;122;96;83m:[0m[38;2;112;85;72m;[0m[38;2;72;58;56m.[0m[38;2;59;79;113m,[0m[38;2;66;99;148m;[0m[38;2;69;104;156m:[0m[38;2;76;116;176mc[0m[38;2;79;125;196mc[0m[38;2;85;143;234mo[0m[38;2;86;145;237mo[0m[38;2;86;145;237mo[0m[38;2;85;145;236mo[0m[38;2;74;111;172m:[0m[38;2;151;113;108ml[0m[38;2;210;141;115mx[0m[38;2;57;45;40m.[0m[38;2;168;157;145mx[0m[38;2;88;102;71m;[0m[38;2;48;76;36m.[0m[38;2;41;66;29m.[0m[38;2;83;99;73m;[0m[38;2;183;181;171mO[0m[38;2;150;135;122mo[0m[38;2;136;121;111ml[0m[38;2;99;87;82m;[0m[38;2;96;74;63m,[0m[38;2;200;141;111md[0m[38;2;161;117;102ml[0m[38;2;74;110;165m:[0m[38;2;75;117;179mc[0m[38;2;80;129;204ml[0m[38;2;82;135;217ml[0m[38;2;81;131;208ml[0m[38;2;84;139;223ml[0m[38;2;83;135;217ml[0m[38;2;85;139;223ml[0m[38;2;85;137;218ml[0m[38;2;82;128;199ml[0m[38;2;78;118;177mc[0m[38;2;81;124;190mc[0m[38;2;72;105;156m:[0m[38;2;62;85;124m,[0m[38;2;81;117;173mc[0m[38;2;87;133;206ml[0m[38;2;99;150;237mo[0m[38;2;108;155;237md[0m[38;2;95;130;192ml[0m[38;2;84;117;169mc[0m[38;2;97;140;214mo[0m[38;2;104;151;235md[0m[38;2;111;154;233md[0m[38;2;76;101;146m:[0m[38;2;81;106;148m:[0m[38;2;84;74;80m,[0m[38;2;40;26;19m [0m[38;2;15;15;15m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;8;6;5m [0m[38;2;103;80;68m,[0m[38;2;143;111;96mc[0m[38;2;122;93;78m:[0m[38;2;106;79;66m,[0m[38;2;121;94;80m:[0m[38;2;139;110;95mc[0m[38;2;129;102;87m:[0m[38;2;123;96;82m:[0m[38;2;122;96;83m:[0m[38;2;122;96;83m:[0m[38;2;122;96;83m:[0m[38;2;122;96;83m:[0m[38;2;121;96;83m:[0m[38;2;120;95;83m:[0m[38;2;99;74;63m,[0m[38;2;53;60;77m.[0m[38;2;62;93;141m;[0m[38;2;65;98;149m;[0m[38;2;66;100;152m;[0m[38;2;74;114;175m:[0m[38;2;78;127;200mc[0m[38;2;85;145;236mo[0m[38;2;85;145;236mo[0m[38;2;84;145;236mo[0m[38;2;84;145;236mo[0m[38;2;82;132;210ml[0m[38;2;154;150;177md[0m[38;2;249;178;150m0[0m[38;2;235;162;137mO[0m[38;2;212;145;123mx[0m[38;2;147;132;113mo[0m[38;2;111;170;102mo[0m[38;2;84;120;79m:[0m[38;2;46;79;41m'[0m[38;2;130;168;125md[0m[38;2;153;171;150mx[0m[38;2;116;122;106mc[0m[38;2;221;195;181m0[0m[38;2;208;183;169mO[0m[38;2;136;111;99mc[0m[38;2;105;79;70m,[0m[38;2;75;108;159m:[0m[38;2;74;116;177m:[0m[38;2;74;115;177m:[0m[38;2;75;117;180mc[0m[38;2;76;118;183mc[0m[38;2;77;123;192mc[0m[38;2;78;124;193mc[0m[38;2;69;106;164m:[0m[38;2;75;117;181mc[0m[38;2;78;119;181mc[0m[38;2;77;117;176mc[0m[38;2;78;117;176mc[0m[38;2;76;113;170m:[0m[38;2;58;81;118m,[0m[38;2;68;96;141m;[0m[38;2;81;117;173mc[0m[38;2;89;139;221mo[0m[38;2;92;146;236mo[0m[38;2;95;146;236mo[0m[38;2;81;118;183mc[0m[38;2;85;118;171mc[0m[38;2;92;135;208ml[0m[38;2;97;147;234mo[0m[38;2;97;143;225mo[0m[38;2;94;135;213ml[0m[38;2;76;96;134m;[0m[38;2;87;113;159mc[0m[38;2;54;69;102m'[0m[38;2;1;1;1m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;6;4;4m [0m[38;2;86;63;52m'[0m[38;2;126;95;80m:[0m[38;2;103;75;62m,[0m[38;2;103;77;65m,[0m[38;2;131;103;90mc[0m[38;2;131;104;90mc[0m[38;2;123;97;83m:[0m[38;2;122;96;83m:[0m[38;2;122;96;83m:[0m[38;2;121;96;83m:[0m[38;2;121;96;83m:[0m[38;2;120;96;83m:[0m[38;2;120;95;83m:[0m[38;2;118;93;81m:[0m[38;2;89;69;61m'[0m[38;2;54;73;106m'[0m[38;2;65;98;149m;[0m[38;2;64;98;150m;[0m[38;2;64;98;150m;[0m[38;2;71;110;169m:[0m[38;2;76;123;192mc[0m[38;2;83;142;230mo[0m[38;2;77;129;203ml[0m[38;2;83;142;231mo[0m[38;2;84;145;236mo[0m[38;2;84;145;236mo[0m[38;2;158;147;159md[0m[38;2;182;172;194mk[0m[38;2;251;186;158m0[0m[38;2;227;161;138mk[0m[38;2;236;159;136mk[0m[38;2;237;153;131mk[0m[38;2;236;151;128mk[0m[38;2;199;139;111md[0m[38;2;160;138;100mo[0m[38;2;138;180;114mx[0m[38;2;127;144;123mo[0m[38;2;196;200;194m0[0m[38;2;250;246;244mW[0m[38;2;226;202;188mK[0m[38;2;153;128;125mo[0m[38;2;69;104;155m:[0m[38;2;72;116;177m:[0m[38;2;72;116;177m:[0m[38;2;73;116;177m:[0m[38;2;74;115;177m:[0m[38;2;75;116;178mc[0m[38;2;76;119;183mc[0m[38;2;77;119;183mc[0m[38;2;76;116;178mc[0m[38;2;70;103;156m:[0m[38;2;69;102;152m;[0m[38;2;77;117;175mc[0m[38;2;74;111;166m:[0m[38;2;62;89;131m,[0m[38;2;58;81;118m,[0m[38;2;75;108;161m:[0m[38;2;81;119;179mc[0m[38;2;91;145;234mo[0m[38;2;92;145;235mo[0m[38;2;94;145;234mo[0m[38;2;72;100;150m;[0m[38;2;85;116;170mc[0m[38;2;91;132;205ml[0m[38;2;97;145;234mo[0m[38;2;95;136;212ml[0m[38;2;93;131;199ml[0m[38;2;79;105;154m:[0m[38;2;82;104;144m:[0m[38;2;95;129;192ml[0m[38;2;71;99;154m;[0m[38;2;7;8;11m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;4;3;3m [0m[38;2;70;50;41m.[0m[38;2;96;67;55m'[0m[38;2;97;71;60m,[0m[38;2;121;95;82m:[0m[38;2;131;104;91mc[0m[38;2;124;98;85m:[0m[38;2;121;96;83m:[0m[38;2;120;96;83m:[0m[38;2;120;96;83m:[0m[38;2;119;95;83m:[0m[38;2;119;95;83m:[0m[38;2;119;95;84m:[0m[38;2;119;95;84m:[0m[38;2;116;91;80m;[0m[38;2;74;56;48m.[0m[38;2;44;59;86m.[0m[38;2;67;104;160m:[0m[38;2;62;98;150m;[0m[38;2;62;98;150m;[0m[38;2;64;102;156m;[0m[38;2;72;115;176m:[0m[38;2;79;134;214ml[0m[38;2;73;120;185mc[0m[38;2;75;123;192mc[0m[38;2;82;143;233mo[0m[38;2;82;145;237mo[0m[38;2;84;127;195mc[0m[38;2;243;191;162mK[0m[38;2;182;175;196mk[0m[38;2;252;194;165mK[0m[38;2;252;189;161mK[0m[38;2;250;181;155m0[0m[38;2;247;172;148m0[0m[38;2;239;161;138mO[0m[38;2;237;156;135mk[0m[38;2;238;156;134mk[0m[38;2;240;159;136mO[0m[38;2;239;173;152mO[0m[38;2;247;207;193mX[0m[38;2;221;190;181m0[0m[38;2;123;129;155ml[0m[38;2;70;114;175m:[0m[38;2;71;117;178mc[0m[38;2;72;116;177m:[0m[38;2;72;116;177m:[0m[38;2;72;116;177m:[0m[38;2;72;114;175m:[0m[38;2;71;111;170m:[0m[38;2;71;110;168m:[0m[38;2;65;98;148m;[0m[38;2;62;87;129m,[0m[38;2;64;90;132m,[0m[38;2;69;102;153m;[0m[38;2;66;97;145m;[0m[38;2;58;83;122m,[0m[38;2;59;82;120m,[0m[38;2;62;86;126m,[0m[38;2;77;111;167m:[0m[38;2;85;130;203ml[0m[38;2;90;143;230mo[0m[38;2;90;135;214ml[0m[38;2;88;130;202ml[0m[38;2;62;81;116m,[0m[38;2;84;115;169mc[0m[38;2;90;128;197ml[0m[38;2;96;143;229mo[0m[38;2;91;127;191ml[0m[38;2;87;118;169mc[0m[38;2;89;118;173mc[0m[38;2;75;92;126m;[0m[38;2;86;109;152m:[0m[38;2;100;141;221mo[0m[38;2;91;128;201ml[0m[38;2;38;48;73m.[0m[38;2;3;3;3m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;6;5;4m [0m[38;2;56;41;34m.[0m[38;2;99;74;62m,[0m[38;2;120;94;82m:[0m[38;2;130;104;91mc[0m[38;2;123;98;86m:[0m[38;2;120;95;83m:[0m[38;2;119;95;83m:[0m[38;2;119;95;83m:[0m[38;2;119;95;84m:[0m[38;2;119;95;84m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;117;92;82m;[0m[38;2;104;78;68m,[0m[38;2;70;71;85m'[0m[38;2;53;82;125m,[0m[38;2;57;89;135m,[0m[38;2;70;113;173m:[0m[38;2;62;98;151m;[0m[38;2;62;97;151m;[0m[38;2;66;106;163m:[0m[38;2;67;109;167m:[0m[38;2;72;118;182mc[0m[38;2;71;117;179mc[0m[38;2;74;124;196mc[0m[38;2;81;144;236mo[0m[38;2;81;144;238mo[0m[38;2;93;110;146m:[0m[38;2;253;198;168mK[0m[38;2;187;177;195mO[0m[38;2;253;198;168mK[0m[38;2;253;197;167mK[0m[38;2;253;195;166mK[0m[38;2;252;191;164mK[0m[38;2;252;188;161mK[0m[38;2;251;186;159m0[0m[38;2;248;182;156m0[0m[38;2;250;183;157m0[0m[38;2;239;176;152m0[0m[38;2;159;130;129mo[0m[38;2;82;98;133m;[0m[38;2;65;96;143m;[0m[38;2;61;98;152m;[0m[38;2;63;102;157m;[0m[38;2;62;99;153m;[0m[38;2;65;96;145m;[0m[38;2;76;96;136m;[0m[38;2;92;98;127m:[0m[38;2;95;94;113m;[0m[38;2;104;96;105m:[0m[38;2;117;92;86m:[0m[38;2;132;88;69m:[0m[38;2;55;70;98m'[0m[38;2;64;94;141m;[0m[38;2;74;91;128m;[0m[38;2;96;74;72m,[0m[38;2;62;88;130m,[0m[38;2;68;96;143m;[0m[38;2;79;117;178mc[0m[38;2;84;126;198mc[0m[38;2;82;119;180mc[0m[38;2;81;115;170mc[0m[38;2;80;116;179mc[0m[38;2;68;91;134m;[0m[38;2;82;112;165m:[0m[38;2;86;118;176mc[0m[38;2;88;122;183mc[0m[38;2;86;116;168mc[0m[38;2;87;115;165mc[0m[38;2;88;116;166mc[0m[38;2;70;86;120m,[0m[38;2;77;77;77m [0m[38;2;12;15;23m [0m[38;2;85;119;186mc[0m[38;2;102;143;229mo[0m[38;2;82;111;174m:[0m[38;2;44;59;92m.[0m[38;2;14;16;25m [0m[38;2;1;1;1m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;5;4;4m [0m[38;2;50;39;34m.[0m[38;2;96;75;66m,[0m[38;2;123;97;85m:[0m[38;2;130;103;91m:[0m[38;2;122;97;86m:[0m[38;2;119;95;83m:[0m[38;2;119;95;83m:[0m[38;2;119;95;84m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;118;96;85m:[0m[38;2;118;96;85m:[0m[38;2;115;92;81m;[0m[38;2;89;70;64m'[0m[38;2;58;75;108m'[0m[38;2;62;97;151m;[0m[38;2;51;78;120m'[0m[38;2;70;115;175m:[0m[38;2;67;109;167m:[0m[38;2;61;98;151m;[0m[38;2;61;98;151m;[0m[38;2;62;99;153m;[0m[38;2;61;98;151m;[0m[38;2;70;114;177m:[0m[38;2;70;115;179m:[0m[38;2;74;124;199mc[0m[38;2;80;142;235mo[0m[38;2;74;129;214ml[0m[38;2;84;120;182mc[0m[38;2;252;197;167mK[0m[38;2;194;180;192mO[0m[38;2;253;198;168mK[0m[38;2;253;198;168mK[0m[38;2;253;198;168mK[0m[38;2;253;198;168mK[0m[38;2;253;197;168mK[0m[38;2;253;197;167mK[0m[38;2;253;197;167mK[0m[38;2;252;196;167mK[0m[38;2;252;196;166mK[0m[38;2;252;195;166mK[0m[38;2;249;192;163mK[0m[38;2;242;182;153m0[0m[38;2;226;166;139mO[0m[38;2;215;154;128mk[0m[38;2;216;148;121mx[0m[38;2;213;138;109mx[0m[38;2;213;134;105md[0m[38;2;139;96;71m:[0m[38;2;59;66;41m.[0m[38;2;50;67;40m.[0m[38;2;124;129;112ml[0m[38;2;110;101;89m:[0m[38;2;99;88;83m;[0m[38;2;36;48;73m.[0m[38;2;120;84;74m;[0m[38;2;145;98;80mc[0m[38;2;61;87;129m,[0m[38;2;73;105;158m:[0m[38;2;79;116;179mc[0m[38;2;78;113;171m:[0m[38;2;79;112;169m:[0m[38;2;77;108;161m:[0m[38;2;73;101;153m;[0m[38;2;72;98;146m;[0m[38;2;73;96;141m;[0m[38;2;83;113;167m:[0m[38;2;82;109;160m:[0m[38;2;77;100;144m;[0m[38;2;86;114;164mc[0m[38;2;87;114;164mc[0m[38;2;82;106;151m:[0m[38;2;7;7;10m [0m[38;2;4;4;4m [0m[38;2;11;11;11m [0m[38;2;18;18;18m [0m[38;2;29;29;29m [0m[38;2;19;19;19m [0m[38;2;12;12;12m [0m[38;2;2;2;2m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;7;6;5m [0m[38;2;65;52;46m.[0m[38;2;120;94;84m:[0m[38;2;135;107;95mc[0m[38;2;133;105;94mc[0m[38;2;119;95;83m:[0m[38;2;119;95;83m:[0m[38;2;119;95;84m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;118;96;85m:[0m[38;2;118;96;85m:[0m[38;2;118;95;86m:[0m[38;2;118;95;86m:[0m[38;2;109;84;75m;[0m[38;2;74;70;80m'[0m[38;2;58;89;135m,[0m[38;2;55;86;133m,[0m[38;2;58;94;144m;[0m[38;2;56;89;137m,[0m[38;2;65;106;163m:[0m[38;2;60;97;150m;[0m[38;2;53;84;129m,[0m[38;2;55;87;136m,[0m[38;2;60;97;152m;[0m[38;2;60;97;152m;[0m[38;2;66;108;169m:[0m[38;2;70;115;180m:[0m[38;2;74;125;201mc[0m[38;2;78;139;227ml[0m[38;2;67;109;177m:[0m[38;2;74;128;207mc[0m[38;2;162;156;175mx[0m[38;2;213;186;184m0[0m[38;2;253;198;169mK[0m[38;2;253;198;169mK[0m[38;2;253;197;169mK[0m[38;2;253;198;169mK[0m[38;2;253;198;168mK[0m[38;2;253;198;168mK[0m[38;2;253;198;168mK[0m[38;2;253;198;168mK[0m[38;2;252;197;167mK[0m[38;2;252;197;167mK[0m[38;2;252;195;166mK[0m[38;2;252;193;165mK[0m[38;2;252;191;163mK[0m[38;2;251;186;160m0[0m[38;2;251;180;154m0[0m[38;2;241;162;136mO[0m[38;2;213;126;100md[0m[38;2;138;120;95ml[0m[38;2;55;86;45m'[0m[38;2;93;126;78mc[0m[38;2;108;118;99mc[0m[38;2;106;100;88m:[0m[38;2;176;153;141mx[0m[38;2;105;92;89m;[0m[38;2;43;53;79m.[0m[38;2;82;78;96m,[0m[38;2;68;98;150m;[0m[38;2;76;111;169m:[0m[38;2;76;111;168m:[0m[38;2;77;110;168m:[0m[38;2;76;107;163m:[0m[38;2;66;90;133m;[0m[38;2;76;107;164m:[0m[38;2;71;95;142m;[0m[38;2;63;82;120m,[0m[38;2;79;105;157m:[0m[38;2;77;99;147m;[0m[38;2;74;94;137m;[0m[38;2;80;103;150m:[0m[38;2;85;111;162m:[0m[38;2;86;111;162m:[0m[38;2;37;44;62m.[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;1;1;1m [0m[38;2;51;40;36m.[0m[38;2;122;96;86m:[0m[38;2;140;112;100mc[0m[38;2;137;110;98mc[0m[38;2;121;97;85m:[0m[38;2;120;96;84m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;118;95;85m:[0m[38;2;113;89;78m;[0m[38;2;115;91;81m;[0m[38;2;116;92;83m;[0m[38;2;110;85;76m;[0m[38;2;109;84;75m;[0m[38;2;86;72;70m,[0m[38;2;60;78;110m,[0m[38;2;61;97;150m;[0m[38;2;61;98;151m;[0m[38;2;50;79;122m'[0m[38;2;46;73;112m'[0m[38;2;55;89;139m,[0m[38;2;54;86;135m,[0m[38;2;53;85;133m,[0m[38;2;59;96;150m;[0m[38;2;51;82;127m,[0m[38;2;57;92;144m;[0m[38;2;60;97;152m;[0m[38;2;61;99;155m;[0m[38;2;69;114;179m:[0m[38;2;70;116;181m:[0m[38;2;76;134;219ml[0m[38;2;89;109;158m:[0m[38;2;113;108;134mc[0m[38;2;151;140;159md[0m[38;2;186;157;157mx[0m[38;2;220;171;151mO[0m[38;2;241;187;161m0[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;169mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;185;126;109mo[0m[38;2;207;139;120mx[0m[38;2;244;190;162mK[0m[38;2;253;196;167mK[0m[38;2;251;194;165mK[0m[38;2;252;192;164mK[0m[38;2;251;187;160mK[0m[38;2;248;177;151m0[0m[38;2;232;153;129mk[0m[38;2;221;141;120mx[0m[38;2;240;148;127mk[0m[38;2;216;142;116mx[0m[38;2;130;128;84ml[0m[38;2;140;140;129mo[0m[38;2;214;191;178m0[0m[38;2;168;146;134md[0m[38;2;71;58;54m.[0m[38;2;26;20;19m [0m[38;2;44;54;77m.[0m[38;2;65;94;145m;[0m[38;2;65;92;142m;[0m[38;2;63;89;136m,[0m[38;2;73;103;160m:[0m[38;2;64;87;133m,[0m[38;2;67;91;138m;[0m[38;2;75;106;166m:[0m[38;2;69;91;136m;[0m[38;2;62;80;118m,[0m[38;2;71;93;140m;[0m[38;2;71;91;134m;[0m[38;2;73;91;134m;[0m[38;2;73;92;135m;[0m[38;2;81;105;155m:[0m[38;2;84;108;160m:[0m[38;2;70;87;126m,[0m[38;2;9;9;9m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;2;1;1m [0m[38;2;125;97;86m:[0m[38;2;139;111;99mc[0m[38;2;139;111;99mc[0m[38;2;129;103;92m:[0m[38;2;119;95;85m:[0m[38;2;119;95;85m:[0m[38;2;117;94;83m:[0m[38;2;114;90;79m;[0m[38;2;112;88;77m;[0m[38;2;106;80;70m;[0m[38;2;103;76;65m,[0m[38;2;104;77;67m,[0m[38;2;96;72;63m,[0m[38;2;81;78;90m,[0m[38;2;68;91;128m;[0m[38;2;55;88;136m,[0m[38;2;59;95;147m;[0m[38;2;58;94;146m;[0m[38;2;56;90;142m,[0m[38;2;49;77;119m'[0m[38;2;51;81;127m,[0m[38;2;60;97;152m;[0m[38;2;60;97;152m;[0m[38;2;60;97;152m;[0m[38;2;66;108;169m:[0m[38;2;66;108;169m:[0m[38;2;54;87;136m,[0m[38;2;55;90;141m,[0m[38;2;59;97;152m;[0m[38;2;62;104;163m;[0m[38;2;69;115;180m:[0m[38;2;71;118;188mc[0m[38;2;133;116;133ml[0m[38;2;192;134;110md[0m[38;2;191;131;109md[0m[38;2;234;174;148mO[0m[38;2;253;195;167mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;194;98;92ml[0m[38;2;230;116;116md[0m[38;2;188;87;84mc[0m[38;2;141;52;47m,[0m[38;2;159;84;74m:[0m[38;2;220;169;144mO[0m[38;2;239;181;155m0[0m[38;2;249;184;158m0[0m[38;2;249;177;152m0[0m[38;2;247;167;144mO[0m[38;2;235;150;130mk[0m[38;2;224;136;117mx[0m[38;2;229;136;117mx[0m[38;2;201;129;115md[0m[38;2;190;120;104mo[0m[38;2;103;70;74m,[0m[38;2;61;80;121m,[0m[38;2;59;85;134m,[0m[38;2;55;76;118m'[0m[38;2;51;69;106m'[0m[38;2;60;84;130m,[0m[38;2;67;95;149m;[0m[38;2;64;89;138m;[0m[38;2;70;96;151m;[0m[38;2;68;92;144m;[0m[38;2;74;105;164m:[0m[38;2;66;88;135m,[0m[38;2;60;77;116m,[0m[38;2;67;86;131m,[0m[38;2;66;83;126m,[0m[38;2;70;88;132m;[0m[38;2;71;89;132m;[0m[38;2;73;91;134m;[0m[38;2;82;105;157m:[0m[38;2;79;101;150m:[0m[38;2;4;4;5m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;6;5;4m [0m[38;2;65;49;42m.[0m[38;2;131;103;91m:[0m[38;2;137;109;98mc[0m[38;2;121;97;86m:[0m[38;2;117;94;84m:[0m[38;2;107;81;70m;[0m[38;2;103;76;65m,[0m[38;2;103;76;65m,[0m[38;2;100;76;67m,[0m[38;2;111;94;88m;[0m[38;2;129;117;116mc[0m[38;2;149;144;148md[0m[38;2;167;165;172mx[0m[38;2;104;112;131mc[0m[38;2;62;101;158m;[0m[38;2;68;112;176m:[0m[38;2;57;91;143m,[0m[38;2;56;90;142m,[0m[38;2;60;97;152m;[0m[38;2;54;86;135m,[0m[38;2;52;84;131m,[0m[38;2;60;97;152m;[0m[38;2;59;97;152m;[0m[38;2;60;98;154m;[0m[38;2;67;112;175m:[0m[38;2;68;115;179m:[0m[38;2;68;115;179m:[0m[38;2;58;98;153m;[0m[38;2;51;85;134m,[0m[38;2;58;97;153m;[0m[38;2;62;105;165m:[0m[38;2;69;115;180m:[0m[38;2;117;107;127mc[0m[38;2;190;131;107md[0m[38;2;195;132;109md[0m[38;2;199;132;109md[0m[38;2;213;148;124mx[0m[38;2;251;191;163mK[0m[38;2;253;197;169mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;196;169mK[0m[38;2;217;155;135mk[0m[38;2;226;134;123mx[0m[38;2;215;137;124mx[0m[38;2;237;179;153m0[0m[38;2;251;193;164mK[0m[38;2;251;194;165mK[0m[38;2;245;189;161m0[0m[38;2;250;191;162mK[0m[38;2;249;185;159m0[0m[38;2;247;179;154m0[0m[38;2;244;170;147mO[0m[38;2;239;159;138mO[0m[38;2;224;143;124mx[0m[38;2;200;124;111md[0m[38;2;91;88;118m;[0m[38;2;60;90;144m;[0m[38;2;60;89;142m,[0m[38;2;61;88;141m,[0m[38;2;61;87;140m,[0m[38;2;52;70;110m'[0m[38;2;61;85;135m,[0m[38;2;67;95;151m;[0m[38;2;68;95;152m;[0m[38;2;68;95;151m;[0m[38;2;69;98;156m;[0m[38;2;68;93;149m;[0m[38;2;62;81;126m,[0m[38;2;61;78;120m,[0m[38;2;65;82;128m,[0m[38;2;61;75;115m'[0m[38;2;67;84;130m,[0m[38;2;68;85;130m,[0m[38;2;66;81;122m,[0m[38;2;74;92;140m;[0m[38;2;80;101;153m:[0m[38;2;6;6;8m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;2;2;2m [0m[38;2;46;34;30m.[0m[38;2;79;59;50m.[0m[38;2;75;55;46m.[0m[38;2;78;56;47m.[0m[38;2;83;61;52m'[0m[38;2;118;94;84m:[0m[38;2;118;96;85m:[0m[38;2;107;83;72m;[0m[38;2;84;65;57m'[0m[38;2;125;118;122mc[0m[38;2;173;175;187mk[0m[38;2;198;208;228mK[0m[38;2;193;199;214m0[0m[38;2;159;158;165mx[0m[38;2;182;181;189mO[0m[38;2;169;167;175mk[0m[38;2;149;146;153md[0m[38;2;75;91;120m;[0m[38;2;54;87;136m,[0m[38;2;57;92;143m;[0m[38;2;55;88;138m,[0m[38;2;54;86;135m,[0m[38;2;57;92;144m;[0m[38;2;46;73;114m'[0m[38;2;58;97;153m;[0m[38;2;58;97;154m;[0m[38;2;61;102;161m;[0m[38;2;67;116;181m:[0m[38;2;69;118;187mc[0m[38;2;71;121;193mc[0m[38;2;72;126;201mc[0m[38;2;58;97;151m;[0m[38;2;53;87;138m,[0m[38;2;53;85;134m,[0m[38;2;62;104;163m;[0m[38;2;75;106;158m:[0m[38;2;167;116;99ml[0m[38;2;188;128;104mo[0m[38;2;191;126;103mo[0m[38;2;192;122;100mo[0m[38;2;187;120;99mo[0m[38;2;232;171;145mO[0m[38;2;253;196;169mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;248;192;165mK[0m[38;2;252;192;165mK[0m[38;2;252;192;164mK[0m[38;2;252;193;165mK[0m[38;2;251;194;166mK[0m[38;2;251;195;168mK[0m[38;2;251;194;166mK[0m[38;2;250;193;166mK[0m[38;2;249;191;164mK[0m[38;2;247;187;162m0[0m[38;2;245;183;159m0[0m[38;2;237;172;149mO[0m[38;2;168;126;125mo[0m[38;2;69;86;130m,[0m[38;2;59;90;144m,[0m[38;2;59;89;142m,[0m[38;2;59;87;141m,[0m[38;2;59;86;140m,[0m[38;2;54;75;121m'[0m[38;2;65;97;163m;[0m[38;2;76;116;201mc[0m[38;2;69;99;161m;[0m[38;2;68;95;155m;[0m[38;2;70;103;167m:[0m[38;2;68;97;158m;[0m[38;2;60;79;128m,[0m[38;2;74;76;103m,[0m[38;2;62;78;123m,[0m[38;2;63;79;126m,[0m[38;2;22;25;37m [0m[38;2;46;56;88m.[0m[38;2;67;82;128m,[0m[38;2;68;82;128m,[0m[38;2;66;81;124m,[0m[38;2;74;92;142m;[0m[38;2;3;4;4m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;1;1;1m [0m[38;2;25;19;17m [0m[38;2;76;55;46m.[0m[38;2;97;71;59m,[0m[38;2;96;69;58m,[0m[38;2;87;64;54m'[0m[38;2;84;61;51m'[0m[38;2;85;62;54m'[0m[38;2;94;71;61m,[0m[38;2;98;97;102m;[0m[38;2;195;209;231mK[0m[38;2;215;229;251mN[0m[38;2;229;238;253mN[0m[38;2;192;197;210m0[0m[38;2;184;184;193mO[0m[38;2;199;203;213m0[0m[38;2;192;195;206m0[0m[38;2;173;174;184mk[0m[38;2;152;150;158md[0m[38;2;152;149;156md[0m[38;2;89;98;116m;[0m[38;2;51;80;125m,[0m[38;2;61;101;158m;[0m[38;2;57;94;148m;[0m[38;2;56;92;145m;[0m[38;2;49;76;119m'[0m[38;2;55;89;141m,[0m[38;2;56;92;146m;[0m[38;2;63;107;168m:[0m[38;2;68;118;185mc[0m[38;2;71;125;199mc[0m[38;2;77;138;226ml[0m[38;2;79;144;238mo[0m[38;2;65;114;183m:[0m[38;2;63;106;166m:[0m[38;2;91;90;110m;[0m[38;2;136;105;100mc[0m[38;2;114;113;136mc[0m[38;2;89;100;136m:[0m[38;2;128;100;104m:[0m[38;2;172;115;96ml[0m[38;2;156;99;86mc[0m[38;2;106;77;91m;[0m[38;2;80;78;110m,[0m[38;2;171;135;131md[0m[38;2;249;193;166mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;252;196;169mK[0m[38;2;251;195;168mK[0m[38;2;251;195;168mK[0m[38;2;247;191;165mK[0m[38;2;237;182;156m0[0m[38;2;219;170;151mO[0m[38;2;190;151;143mx[0m[38;2;148;128;139mo[0m[38;2;84;88;120m;[0m[38;2;59;91;145m;[0m[38;2;59;90;144m,[0m[38;2;58;88;143m,[0m[38;2;58;87;141m,[0m[38;2;58;86;141m,[0m[38;2;58;84;139m,[0m[38;2;54;77;127m,[0m[38;2;77;123;216mc[0m[38;2;76;119;200mc[0m[38;2;74;113;186m:[0m[38;2;73;111;183m:[0m[38;2;62;86;142m,[0m[38;2;60;77;124m,[0m[38;2;106;99;122m:[0m[38;2;82;83;111m,[0m[38;2;110;103;126m:[0m[38;2;42;39;48m.[0m[38;2;45;45;45m [0m[38;2;11;11;15m [0m[38;2;25;28;44m [0m[38;2;65;79;126m,[0m[38;2;62;75;118m,[0m[38;2;61;73;114m'[0m[38;2;2;2;2m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;9;7;6m [0m[38;2;54;42;37m.[0m[38;2;98;77;68m,[0m[38;2;112;86;74m;[0m[38;2;105;78;66m,[0m[38;2;104;76;64m,[0m[38;2;104;76;64m,[0m[38;2;102;75;63m,[0m[38;2;86;63;54m'[0m[38;2;85;62;52m'[0m[38;2;82;60;52m'[0m[38;2;120;118;124mc[0m[38;2;210;226;251mX[0m[38;2;216;230;251mN[0m[38;2;225;235;251mN[0m[38;2;182;182;190mO[0m[38;2;202;210;225mK[0m[38;2;230;240;253mW[0m[38;2;233;241;254mW[0m[38;2;233;241;254mW[0m[38;2;225;235;250mN[0m[38;2;178;183;197mO[0m[38;2;150;148;155md[0m[38;2;78;101;139m:[0m[38;2;63;101;158m;[0m[38;2;65;111;173m:[0m[38;2;62;105;165m:[0m[38;2;56;86;135m,[0m[38;2;57;92;146m;[0m[38;2;57;91;145m;[0m[38;2;63;107;169m:[0m[38;2;67;115;179m:[0m[38;2;75;133;215ml[0m[38;2;75;135;220ml[0m[38;2;79;144;238mo[0m[38;2;79;144;238mo[0m[38;2;72;128;207mc[0m[38;2;75;98;141m;[0m[38;2;176;121;97mo[0m[38;2;215;150;120mx[0m[38;2;210;146;117mx[0m[38;2;185;126;102mo[0m[38;2;100;78;91m,[0m[38;2;62;81;126m,[0m[38;2;61;86;135m,[0m[38;2;60;90;141m,[0m[38;2;59;96;152m;[0m[38;2;84;101;140m:[0m[38;2;132;126;145ml[0m[38;2;135;127;144ml[0m[38;2;125;122;142ml[0m[38;2;107;111;138mc[0m[38;2;87;91;119m;[0m[38;2;79;98;137m;[0m[38;2;72;97;141m;[0m[38;2;67;93;140m;[0m[38;2;61;90;140m,[0m[38;2;59;91;145m;[0m[38;2;58;88;140m,[0m[38;2;60;92;146m;[0m[38;2;57;86;137m,[0m[38;2;62;93;149m;[0m[38;2;58;90;145m;[0m[38;2;59;92;148m;[0m[38;2;57;87;142m,[0m[38;2;57;86;141m,[0m[38;2;57;85;140m,[0m[38;2;51;74;122m'[0m[38;2;59;88;151m,[0m[38;2;69;104;180m:[0m[38;2;65;94;159m;[0m[38;2;64;91;154m;[0m[38;2;58;80;136m,[0m[38;2;65;78;122m,[0m[38;2;118;106;126mc[0m[38;2;115;103;123m:[0m[38;2;103;98;123m:[0m[38;2;157;139;158md[0m[38;2;17;15;17m [0m[38;2;0;0;0m [0m[38;2;8;8;8m [0m[38;2;10;10;10m [0m[38;2;46;46;46m [0m[38;2;43;51;81m.[0m[38;2;60;72;114m'[0m[38;2;15;16;24m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;1;1;1m [0m[38;2;23;18;15m [0m[38;2;74;58;51m.[0m[38;2;121;95;84m:[0m[38;2;136;108;96mc[0m[38;2;129;102;91m:[0m[38;2;110;83;71m;[0m[38;2;104;76;64m,[0m[38;2;96;69;59m,[0m[38;2;99;72;61m,[0m[38;2;104;76;65m,[0m[38;2;97;72;61m,[0m[38;2;86;63;54m'[0m[38;2;87;64;54m'[0m[38;2;72;53;46m.[0m[38;2;154;160;174mx[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;201;211;230mK[0m[38;2;209;224;248mX[0m[38;2;218;232;252mN[0m[38;2;231;240;253mW[0m[38;2;233;241;254mW[0m[38;2;232;241;254mW[0m[38;2;182;200;228m0[0m[38;2;102;142;202mo[0m[38;2;69;118;186mc[0m[38;2;66;106;163m:[0m[38;2;66;113;176m:[0m[38;2;67;115;179m:[0m[38;2;61;92;144m;[0m[38;2;58;79;124m,[0m[38;2;59;86;135m,[0m[38;2;61;103;163m;[0m[38;2;67;115;179m:[0m[38;2;79;133;208ml[0m[38;2;80;141;228ml[0m[38;2;79;144;238mo[0m[38;2;79;144;238mo[0m[38;2;77;139;229ml[0m[38;2;65;109;175m:[0m[38;2;158;136;146mo[0m[38;2;185;126;101mo[0m[38;2;215;149;119mx[0m[38;2;200;135;108md[0m[38;2;128;91;91m:[0m[38;2;82;96;134m;[0m[38;2;58;96;153m;[0m[38;2;58;97;154m;[0m[38;2;58;96;152m;[0m[38;2;58;97;154m;[0m[38;2;58;97;154m;[0m[38;2;58;97;154m;[0m[38;2;58;97;154m;[0m[38;2;59;98;155m;[0m[38;2;67;97;144m;[0m[38;2;79;104;146m:[0m[38;2;57;96;151m;[0m[38;2;58;95;150m;[0m[38;2;57;95;150m;[0m[38;2;58;92;147m;[0m[38;2;61;93;148m;[0m[38;2;61;99;159m;[0m[38;2;57;90;146m,[0m[38;2;69;110;179m:[0m[38;2;61;95;154m;[0m[38;2;61;95;154m;[0m[38;2;54;82;134m,[0m[38;2;56;84;139m,[0m[38;2;57;86;140m,[0m[38;2;52;77;127m'[0m[38;2;63;94;158m;[0m[38;2;64;93;158m;[0m[38;2;60;86;147m,[0m[38;2;57;77;130m,[0m[38;2;86;88;123m;[0m[38;2;129;114;132mc[0m[38;2;125;109;126mc[0m[38;2;117;105;125mc[0m[38;2;155;137;159md[0m[38;2;153;135;154mo[0m[38;2;6;5;5m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;1;1;1m [0m[38;2;33;38;58m.[0m[38;2;49;49;49m [0m[38;2;60;60;60m [0m[38;2;8;8;10m [0m[38;2;2;2;2m [0m[38;2;3;3;3m [0m[38;2;11;11;11m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m [0m[38;2;1;1;1m [0m[38;2;24;18;15m [0m[38;2;80;63;54m'[0m[38;2;118;93;80m:[0m[38;2;123;97;84m:[0m[38;2;134;106;94mc[0m[38;2;135;107;95mc[0m[38;2;127;101;89m:[0m[38;2;112;86;74m;[0m[38;2;95;69;58m,[0m[38;2;84;60;51m'[0m[38;2;88;64;54m'[0m[38;2;99;73;62m,[0m[38;2;103;76;65m,[0m[38;2;85;62;53m'[0m[38;2;94;69;59m'[0m[38;2;92;68;58m'[0m[38;2;63;49;45m.[0m[38;2;173;184;202mO[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;204;219;244mX[0m[38;2;203;219;243mX[0m[38;2;174;197;232m0[0m[38;2;118;159;218md[0m[38;2;90;143;219mo[0m[38;2;106;138;188mo[0m[38;2;140;151;174md[0m[38;2;156;160;175mx[0m[38;2;73;128;206mc[0m[38;2;67;115;180m:[0m[38;2;64;100;156m;[0m[38;2;59;80;126m,[0m[38;2;136;95;90m:[0m[38;2;77;95;136m;[0m[38;2;66;113;177m:[0m[38;2;70;118;183mc[0m[38;2;99;154;234md[0m[38;2;85;147;238mo[0m[38;2;79;144;238mo[0m[38;2;79;144;238mo[0m[38;2;76;136;224ml[0m[38;2;127;112;127mc[0m[38;2;151;135;152mo[0m[38;2;188;132;111md[0m[38;2;203;138;111md[0m[38;2;168;107;86ml[0m[38;2;195;132;106md[0m[38;2;164;114;98ml[0m[38;2;107;81;77m;[0m[38;2;59;69;94m'[0m[38;2;51;74;110m'[0m[38;2;50;72;105m'[0m[38;2;55;88;137m,[0m[38;2;59;99;157m;[0m[38;2;65;111;174m:[0m[38;2;78;104;145m:[0m[38;2;133;136;149mo[0m[38;2;64;101;157m;[0m[38;2;58;95;150m;[0m[38;2;57;94;148m;[0m[38;2;56;90;143m,[0m[38;2;61;100;159m;[0m[38;2;55;88;140m,[0m[38;2;61;100;165m;[0m[38;2;67;107;173m:[0m[38;2;67;106;171m:[0m[38;2;63;100;163m;[0m[38;2;54;84;137m,[0m[38;2;52;79;129m,[0m[38;2;50;75;123m'[0m[38;2;57;85;142m,[0m[38;2;60;85;139m,[0m[38;2;55;77;128m,[0m[38;2;57;73;118m'[0m[38;2;122;112;132mc[0m[38;2;134;120;138ml[0m[38;2;133;118;137ml[0m[38;2;143;128;148mo[0m[38;2;156;138;161md[0m[38;2;155;137;159md[0m[38;2;146;129;149mo[0m[38;2;5;4;5m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;4;4;5m [0m[38;2;29;29;29m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;10;10;10m [0m[38;2;22;22;22m [0m[38;2;19;19;19m [0m[38;2;3;3;3m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;14;11;10m [0m[38;2;82;64;55m'[0m[38;2;120;93;80m:[0m[38;2;122;96;83m:[0m[38;2;122;96;83m:[0m[38;2;122;96;83m:[0m[38;2;132;104;92mc[0m[38;2;134;107;95mc[0m[38;2;125;99;87m:[0m[38;2;109;82;70m;[0m[38;2;83;60;51m'[0m[38;2;90;64;54m'[0m[38;2;88;64;54m'[0m[38;2;100;73;63m,[0m[38;2;103;76;65m,[0m[38;2;95;70;59m,[0m[38;2;77;56;48m.[0m[38;2;102;75;64m,[0m[38;2;98;72;63m,[0m[38;2;74;62;60m'[0m[38;2;193;207;230mK[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;208;224;249mX[0m[38;2;196;212;237mK[0m[38;2;193;212;240mK[0m[38;2;201;216;239mK[0m[38;2;190;194;207m0[0m[38;2;193;198;211m0[0m[38;2;185;185;194mO[0m[38;2;181;180;188mO[0m[38;2;106;137;190mo[0m[38;2;73;130;210ml[0m[38;2;67;115;179m:[0m[38;2;62;89;138m,[0m[38;2;150;96;80m:[0m[38;2;160;110;95ml[0m[38;2;118;107;124mc[0m[38;2;69;110;168m:[0m[38;2;80;128;194mc[0m[38;2;119;169;241mx[0m[38;2;87;149;239mo[0m[38;2;79;144;238mo[0m[38;2;79;144;238mo[0m[38;2;83;134;215ml[0m[38;2;158;117;109ml[0m[38;2;146;128;145mo[0m[38;2;182;125;105mo[0m[38;2;185;125;101mo[0m[38;2;185;124;101mo[0m[38;2;185;125;101mo[0m[38;2;185;126;102mo[0m[38;2;179;122;99mo[0m[38;2;136;106;105mc[0m[38;2;101;91;106m;[0m[38;2;67;67;76m'[0m[38;2;122;134;161mo[0m[38;2;123;134;161mo[0m[38;2;139;139;150mo[0m[38;2;164;163;172mx[0m[38;2;119;125;145ml[0m[38;2;62;98;153m;[0m[38;2;54;86;136m,[0m[38;2;55;91;143m,[0m[38;2;49;79;125m,[0m[38;2;48;75;120m'[0m[38;2;70;118;193mc[0m[38;2;67;108;174m:[0m[38;2;66;107;171m:[0m[38;2;64;101;165m;[0m[38;2;55;85;140m,[0m[38;2;54;83;137m,[0m[38;2;54;83;137m,[0m[38;2;58;78;119m,[0m[38;2;139;129;145mo[0m[38;2;83;90;122m;[0m[38;2;55;74;120m'[0m[38;2;136;123;141ml[0m[38;2;144;130;150mo[0m[38;2;156;142;164md[0m[38;2;156;141;164md[0m[38;2;155;139;163md[0m[38;2;155;138;162md[0m[38;2;138;121;141ml[0m[38;2;2;2;2m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;3;2;3m [0m[38;2;5;4;5m [0m[38;2;2;2;2m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;120;93;80m:[0m[38;2;126;98;84m:[0m[38;2;123;95;81m:[0m[38;2;123;96;83m:[0m[38;2;122;96;83m:[0m[38;2;125;98;85m:[0m[38;2;133;105;92mc[0m[38;2;129;102;90m:[0m[38;2;120;96;84m:[0m[38;2;110;83;71m;[0m[38;2;83;61;52m'[0m[38;2;92;66;56m'[0m[38;2;101;73;62m,[0m[38;2;104;76;65m,[0m[38;2;103;76;65m,[0m[38;2;97;72;61m,[0m[38;2;87;64;54m'[0m[38;2;86;63;54m'[0m[38;2;93;68;59m'[0m[38;2;88;65;57m'[0m[38;2;87;71;68m,[0m[38;2;168;177;195mk[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;204;216;237mX[0m[38;2;209;223;247mX[0m[38;2;227;238;253mN[0m[38;2;227;234;246mN[0m[38;2;193;195;204m0[0m[38;2;148;152;169md[0m[38;2;78;141;233ml[0m[38;2;68;116;181m:[0m[38;2;68;83;126m,[0m[38;2;180;122;99mo[0m[38;2;161;114;100ml[0m[38;2;118;98;109m:[0m[38;2;75;91;130m;[0m[38;2;63;106;167m:[0m[38;2;82;130;196ml[0m[38;2;122;170;239mx[0m[38;2;93;153;240md[0m[38;2;79;144;238mo[0m[38;2;79;144;238mo[0m[38;2;88;132;205ml[0m[38;2;192;143;127md[0m[38;2;159;134;144mo[0m[38;2;190;127;104mo[0m[38;2;219;157;132mk[0m[38;2;209;146;119mx[0m[38;2;202;138;111md[0m[38;2;200;137;110md[0m[38;2;197;135;110md[0m[38;2;196;135;110md[0m[38;2;137;96;80m:[0m[38;2;139;94;80m:[0m[38;2;159;108;94mc[0m[38;2;143;98;87m:[0m[38;2;138;105;102mc[0m[38;2;142;130;136mo[0m[38;2;116;121;144ml[0m[38;2;68;108;170m:[0m[38;2;64;106;167m:[0m[38;2;54;88;139m,[0m[38;2;68;119;197mc[0m[38;2;67;112;178m:[0m[38;2;67;110;175m:[0m[38;2;67;108;173m:[0m[38;2;61;97;157m;[0m[38;2;53;81;133m,[0m[38;2;53;82;135m,[0m[38;2;56;87;144m,[0m[38;2;79;91;126m;[0m[38;2;140;132;147mo[0m[38;2;131;124;139ml[0m[38;2;62;78;119m,[0m[38;2;124;114;131mc[0m[38;2;156;144;164md[0m[38;2;158;145;166md[0m[38;2;157;144;165md[0m[38;2;157;142;165md[0m[38;2;156;141;163md[0m[38;2;128;114;133mc[0m[38;2;1;1;1m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;4;4;4m [0m[38;2;3;3;3m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;123;95;81m:[0m[38;2;123;95;81m:[0m[38;2;122;96;83m:[0m[38;2;131;103;91m:[0m[38;2;134;106;93mc[0m[38;2;134;106;94mc[0m[38;2;127;101;89m:[0m[38;2;106;78;66m,[0m[38;2;89;67;58m'[0m[38;2;97;69;59m,[0m[38;2;104;76;64m,[0m[38;2;104;76;65m,[0m[38;2;103;76;65m,[0m[38;2;99;73;62m,[0m[38;2;88;64;54m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;82;62;56m'[0m[38;2;133;134;144mo[0m[38;2;206;221;246mX[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;210;226;251mX[0m[38;2;211;226;251mX[0m[38;2;231;240;254mW[0m[38;2;233;241;254mW[0m[38;2;225;232;244mN[0m[38;2;135;145;168mo[0m[38;2;72;124;203mc[0m[38;2;68;116;181m:[0m[38;2;98;109;141m:[0m[38;2;157;115;105ml[0m[38;2;208;145;116mx[0m[38;2;203;141;114mx[0m[38;2;175;126;109mo[0m[38;2;107;104;125m:[0m[38;2;61;98;152m;[0m[38;2;69;115;178m:[0m[38;2;107;155;224md[0m[38;2;109;163;241md[0m[38;2;81;145;238mo[0m[38;2;79;143;236mo[0m[38;2;87;133;208ml[0m[38;2;150;143;167md[0m[38;2;222;157;127mk[0m[38;2;222;156;126mk[0m[38;2;223;156;126mk[0m[38;2;222;157;126mk[0m[38;2;222;157;126mk[0m[38;2;222;157;126mk[0m[38;2;222;156;126mk[0m[38;2;220;155;126mk[0m[38;2;117;87;74m;[0m[38;2;222;157;127mk[0m[38;2;218;154;125mk[0m[38;2;208;146;119mx[0m[38;2;186;128;107mo[0m[38;2;162;122;113ml[0m[38;2;114;108;125mc[0m[38;2;66;98;154m;[0m[38;2;71;124;205mc[0m[38;2;76;137;227ml[0m[38;2;69;120;197mc[0m[38;2;61;101;163m;[0m[38;2;56;91;147m;[0m[38;2;55;87;141m,[0m[38;2;55;88;143m,[0m[38;2;48;74;120m'[0m[38;2;56;85;137m,[0m[38;2;123;119;137ml[0m[38;2;142;134;149mo[0m[38;2;141;132;147mo[0m[38;2;131;123;139ml[0m[38;2;98;93;110m;[0m[38;2;140;129;146mo[0m[38;2;159;147;167md[0m[38;2;159;146;167md[0m[38;2;158;145;166md[0m[38;2;157;144;165md[0m[38;2;107;97;111m;[0m[38;2;16;16;16m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;123;95;81m:[0m[38;2;123;95;81m:[0m[38;2;123;96;83m:[0m[38;2;128;101;88m:[0m[38;2;133;105;93mc[0m[38;2;132;104;91mc[0m[38;2;124;99;87m:[0m[38;2;114;88;76m;[0m[38;2;101;74;63m,[0m[38;2;79;56;48m.[0m[38;2;99;71;60m,[0m[38;2;104;76;64m,[0m[38;2;104;76;65m,[0m[38;2;100;74;63m,[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;86;63;57m'[0m[38;2;94;80;80m,[0m[38;2;155;159;174mx[0m[38;2;205;221;245mX[0m[38;2;210;226;251mX[0m[38;2;211;227;251mX[0m[38;2;233;241;254mW[0m[38;2;233;241;254mW[0m[38;2;202;206;218mK[0m[38;2;113;152;212md[0m[38;2;71;122;200mc[0m[38;2;67;114;178m:[0m[38;2;142;142;157mo[0m[38;2;178;175;184mk[0m[38;2;195;143;125mx[0m[38;2;227;166;136mO[0m[38;2;234;174;145mO[0m[38;2;234;174;146mO[0m[38;2;159;123;108ml[0m[38;2;79;84;106m,[0m[38;2;65;98;149m;[0m[38;2;78;124;188mc[0m[38;2;111;160;230md[0m[38;2;98;156;240md[0m[38;2;80;144;238mo[0m[38;2;78;126;200mc[0m[38;2;155;130;135mo[0m[38;2;221;156;127mk[0m[38;2;210;147;119mx[0m[38;2;222;157;127mk[0m[38;2;222;157;127mk[0m[38;2;222;157;127mk[0m[38;2;222;157;127mk[0m[38;2;222;157;127mk[0m[38;2;171;123;101mo[0m[38;2;164;119;98ml[0m[38;2;221;156;126mk[0m[38;2;222;155;127mk[0m[38;2;210;145;119mx[0m[38;2;184;122;100mo[0m[38;2;135;76;66m;[0m[38;2;124;119;138ml[0m[38;2;75;97;143m;[0m[38;2;60;86;135m,[0m[38;2;57;94;149m;[0m[38;2;57;93;151m;[0m[38;2;56;92;150m;[0m[38;2;60;99;160m;[0m[38;2;52;82;133m,[0m[38;2;45;63;98m.[0m[38;2;111;114;136mc[0m[38;2;149;142;157md[0m[38;2;149;142;157md[0m[38;2;149;140;157mo[0m[38;2;148;139;157mo[0m[38;2;148;138;155mo[0m[38;2;134;125;141ml[0m[38;2;143;133;150mo[0m[38;2;153;142;161md[0m[38;2;155;143;163md[0m[38;2;158;146;166md[0m[38;2;46;41;47m.[0m[38;2;3;3;3m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;125;96;82m:[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;124;96;82m:[0m[38;2;129;101;87m:[0m[38;2;132;104;91mc[0m[38;2;132;104;91mc[0m[38;2;132;104;91mc[0m[38;2;123;98;86m:[0m[38;2;109;81;69m;[0m[38;2;104;76;64m,[0m[38;2;91;66;56m'[0m[38;2;86;60;52m'[0m[38;2;91;66;56m'[0m[38;2;94;69;58m'[0m[38;2;92;67;57m'[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;88;64;55m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;86;63;57m'[0m[38;2;75;55;50m.[0m[38;2;79;58;52m.[0m[38;2;91;72;68m,[0m[38;2;137;131;138mo[0m[38;2;189;199;216m0[0m[38;2;216;223;237mX[0m[38;2;192;193;203m0[0m[38;2;125;148;187md[0m[38;2;92;152;240mo[0m[38;2;67;113;181m:[0m[38;2;78;106;152m:[0m[38;2;170;169;177mk[0m[38;2;185;187;197mO[0m[38;2;194;166;162mk[0m[38;2;247;196;170mK[0m[38;2;254;213;193mX[0m[38;2;254;209;186mX[0m[38;2;253;198;171mK[0m[38;2;172;136;119mo[0m[38;2;135;105;93mc[0m[38;2;135;122;132ml[0m[38;2;66;97;146m;[0m[38;2;75;118;180mc[0m[38;2;87;136;209ml[0m[38;2;79;142;232mo[0m[38;2;74;130;209ml[0m[38;2;100;120;163mc[0m[38;2;158;131;132mo[0m[38;2;207;150;126mx[0m[38;2;221;160;132mk[0m[38;2;213;154;130mk[0m[38;2;207;147;124mx[0m[38;2;222;154;127mk[0m[38;2;224;153;126mk[0m[38;2;140;99;83m:[0m[38;2;197;137;112md[0m[38;2;221;155;126mk[0m[38;2;215;149;122mx[0m[38;2;197;138;114md[0m[38;2;187;127;105mo[0m[38;2;153;136;142mo[0m[38;2;156;153;162md[0m[38;2;92;102;136m:[0m[38;2;61;101;160m;[0m[38;2;57;93;151m;[0m[38;2;57;94;152m;[0m[38;2;60;100;161m;[0m[38;2;51;67;108m'[0m[38;2;105;110;135mc[0m[38;2;163;159;173mx[0m[38;2;164;158;173mx[0m[38;2;164;157;173mx[0m[38;2;164;155;173mx[0m[38;2;163;154;173mx[0m[38;2;163;154;173mx[0m[38;2;162;153;172mx[0m[38;2;162;152;171mx[0m[38;2;162;150;171md[0m[38;2;160;148;169md[0m[38;2;48;44;50m.[0m[38;2;14;14;14m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;125;97;81m:[0m[38;2;125;96;82m:[0m[38;2;128;100;85m:[0m[38;2;133;104;90mc[0m[38;2;135;106;92mc[0m[38;2;134;105;91mc[0m[38;2;131;103;90mc[0m[38;2;123;96;83m:[0m[38;2;112;84;70m;[0m[38;2;106;77;63m,[0m[38;2;105;76;64m,[0m[38;2;104;76;64m,[0m[38;2;89;65;54m'[0m[38;2;86;61;52m'[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;83;63;56m'[0m[38;2;79;60;55m'[0m[38;2;87;64;56m'[0m[38;2;87;64;56m'[0m[38;2;66;48;43m.[0m[38;2;86;63;56m'[0m[38;2;102;75;66m,[0m[38;2;101;75;66m,[0m[38;2;95;72;64m,[0m[38;2;113;107;113m:[0m[38;2;131;159;204md[0m[38;2;82;144;232mo[0m[38;2;80;145;239mo[0m[38;2;63;104;164m:[0m[38;2;127;132;149ml[0m[38;2;180;179;187mO[0m[38;2;195;202;218m0[0m[38;2;201;176;174mO[0m[38;2;253;197;170mK[0m[38;2;253;201;175mK[0m[38;2;254;209;186mX[0m[38;2;253;198;172mK[0m[38;2;252;194;166mK[0m[38;2;105;86;79m;[0m[38;2;109;89;81m;[0m[38;2;199;148;127mx[0m[38;2;120;111;126mc[0m[38;2;60;93;145m;[0m[38;2;58;96;151m;[0m[38;2;62;106;171m:[0m[38;2;68;114;182m:[0m[38;2;85;119;176mc[0m[38;2;107;127;169ml[0m[38;2;141;139;161mo[0m[38;2;186;159;158mx[0m[38;2;239;186;161m0[0m[38;2;246;191;162mK[0m[38;2;241;183;154m0[0m[38;2;234;173;144mO[0m[38;2;158;117;99ml[0m[38;2;207;149;123mx[0m[38;2;179;119;96mo[0m[38;2;184;127;104mo[0m[38;2;169;114;94ml[0m[38;2;166;161;169mx[0m[38;2;165;164;172mx[0m[38;2;120;113;130mc[0m[38;2;67;104;165m:[0m[38;2;62;102;163m;[0m[38;2;60;100;160m;[0m[38;2;58;87;139m,[0m[38;2;58;85;138m,[0m[38;2;65;100;160m;[0m[38;2;116;121;147ml[0m[38;2;160;155;170mx[0m[38;2;167;160;177mx[0m[38;2;167;159;177mx[0m[38;2;166;157;176mx[0m[38;2;164;155;174mx[0m[38;2;163;154;173mx[0m[38;2;161;152;171mx[0m[38;2;134;125;142ml[0m[38;2;62;62;62m [0m[38;2;7;7;7m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "[0m[38;2;128;97;80m:[0m[38;2;128;98;82m:[0m[38;2;127;96;81m:[0m[38;2;125;93;78m:[0m[38;2;116;84;69m;[0m[38;2;110;79;64m,[0m[38;2;109;77;62m,[0m[38;2;105;75;62m,[0m[38;2;103;75;61m,[0m[38;2;104;75;62m,[0m[38;2;106;77;63m,[0m[38;2;105;76;64m,[0m[38;2;104;76;64m,[0m[38;2;95;69;58m,[0m[38;2;83;60;51m'[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;88;64;54m'[0m[38;2;85;62;53m'[0m[38;2;65;90;130m;[0m[38;2;74;61;62m'[0m[38;2;87;64;56m'[0m[38;2;70;51;45m.[0m[38;2;74;54;48m.[0m[38;2;102;75;66m,[0m[38;2;102;75;66m,[0m[38;2;100;79;77m,[0m[38;2;89;95;120m;[0m[38;2;76;95;133m;[0m[38;2;83;141;225mo[0m[38;2;80;145;239mo[0m[38;2;71;123;201mc[0m[38;2;93;110;143m:[0m[38;2;173;172;180mk[0m[38;2;182;181;190mO[0m[38;2;207;221;244mX[0m[38;2;206;173;165mO[0m[38;2;253;197;170mK[0m[38;2;252;196;169mK[0m[38;2;251;195;167mK[0m[38;2;251;195;167mK[0m[38;2;251;199;173mK[0m[38;2;208;163;141mk[0m[38;2;63;57;57m.[0m[38;2;71;59;55m.[0m[38;2;182;124;103mo[0m[38;2;175;122;108mo[0m[38;2;96;107;143m:[0m[38;2;69;115;181m:[0m[38;2;73;113;172m:[0m[38;2;196;164;158mk[0m[38;2;253;197;170mK[0m[38;2;253;197;170mK[0m[38;2;252;196;169mK[0m[38;2;250;194;167mK[0m[38;2;248;192;164mK[0m[38;2;243;186;157m0[0m[38;2;236;178;149m0[0m[38;2;230;170;141mO[0m[38;2;169;126;106mo[0m[38;2;150;106;89mc[0m[38;2;187;128;104mo[0m[38;2;152;115;106ml[0m[38;2;177;175;185mk[0m[38;2;164;162;172mx[0m[38;2;111;125;163ml[0m[38;2;63;90;143m;[0m[38;2;66;110;175m:[0m[38;2;64;106;170m:[0m[38;2;73;83;121m,[0m[38;2;95;107;141m:[0m[38;2;75;96;140m;[0m[38;2;62;92;146m;[0m[38;2;74;96;140m;[0m[38;2;112;116;141mc[0m[38;2;147;141;158mo[0m[38;2;160;152;171mx[0m[38;2;160;152;171mx[0m[38;2;161;152;171mx[0m[38;2;31;29;32m [0m[38;2;22;22;22m [0m[38;2;1;1;1m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m[38;2;0;0;0m [0m\n"
            "\n\033[34m\033[1mCopyright (C) konacode | \033[0m\033[34mhttps://konacode.com/\033[0m\n");


    if (argc>0) {
        bool log_to_file = false, debug = false;
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "-debug" || arg == "--debug" || arg == "-d") debug = true;
            else if (arg == "-log" || arg == "--log" || arg == "-l") log_to_file = true;
            else if (arg == "-file" || arg == "--file" || arg == "-f") {
                try {
                    path = std::string(argv[++i]);
                } catch (std::exception &e) {
                    fprintf(stderr,"\033[31;1m[ERR]\033[0;31m Unable to store \"%s\" into a std::string! Error details: %s\033[0m\n",argv[i+1],e.what());
                } catch (...) {
                    fprintf(stderr,"\033[31;1m[ERR]\033[0;31m Unable to store \"%s\" into a std::string!\033[0m\n",argv[i+1]);
                }
            }
            else {
                printf("\033[33;1m[WRN]\033[0;33m Your command argument \"%s\" is invalid! Please use the following:\033[0m\n", argv[i]);
                printf("\033[33;1m[WRN]\033[0;33m \"--debug\" or \"-debug\" or \"-d\" to enable the debug logger output.\033[0m\n");
                printf("\033[33;1m[WRN]\033[0;33m \"--log\"   or \"-log\"   or \"-l\" to logging to file.\033[0m\n\n");
                printf("\033[33;1m[WRN]\033[0;33m \"--file\" + path   or \"-file\" + path   or \"-f\" + path to select a custom file to load.\033[0m\n\n");
            }
        }

        logger::initialize(log_to_file,debug);

    } else {
        logger::initialize();
    }

    logger::log("Starting...\n\n"
        "                         .'.                                                                                                                \n"
        "                   ..      ,l.                                                                                                              \n"
        "                 ..,loc;cl,.'                                                                                                               \n"
        "              .,ldxxxddlddxxdl.           .XNNNx   :0NNNO;  .;dOXNWWXOo.     dNNN0.    :NNNNl      oNNNNd   'XNNNNNNNNNNNNN;   oNNNNd       \n"
        "               .'colodxkdoodxdd.          kWWWW,.cXWWWk,  :OWWWWK0KNWWWWx   'WWWWWX'   KWWWN.    .OWWWWWX   o000KWWWWN0000x  .OWWWWWX       \n"
        "                :cccooxKdxdcloo.         ,WWWWXxWWWXo.  .0WWWNc.    kWWWWo  OWWWWWWWc lWWWWo    ,NWWKNWWW'      cWWWWl      ,NWWKNWWW'      \n"
        "          .:,d .. 'c000K0kdc:c;          OWWWWWWWW0     OWWWW;      cWWWWd 'WWWWWWWWWdXWWWN.   :NWWO 0WWWl      XWWWN.     :NWWO 0WWWl      \n"
        "          ,cOOo.  .loxxxk0xocc'         ,WWWWWNWWWWd   .WWWWW.     .KWWWX. xWWWW:xWWWWWWWWl   xWWWN:'KWWWO     lWWWWd     xWWWN:'KWWWO      \n"
        "           ;ddll;,ol:cdooolocl.         kWWWW: xWWWWO.  kWWWWO;',:xNWWWx. .NWWWk  cWWWWWWX. .KWWWWWWWWWWWW.   .XWWWX.   .KWWWWWWWWWWWW.     \n"
        "           ldx0NOdkOlldoolokkxo,       'WWWWx   kWWWWK.  lNWWWWWWWWWKo.   xWWWW.   ,XWWWWo ;NWWWd    dWWWW,   lWWWWl   ;NWWWd    dWWWW,     \n"
        "            :KWWxkkxxdKWKk0XXXNKx'     ,oooo.    loooo;    'coddoc,.      looo:     .looo..looo:     'oooo'   :oool   .looo:     'oooo'     \n"
        "             .:dOKXN0kKWXOKWWWWWWkc.                                                                                                        \n"
        "             'cdxllodoxkdlkKNN0xlc;',                                                                                                       \n"
        "          .;oolcclllllcld:cccccllcc; '                                                                                                      \n"
        "         ::,ccclolllllllo,.cl:.,cllc...                                                                                                     \n"
        "        :. :l:cclc:lclllll,.cc.  'clc:.    :ccccccc:'.         .cccc'      .ccc:      :ccc:      .':cllc;.    .ccccccccccc;  ,cccccccc:.    \n"
        "       .. ,l;'kkkc...dkkl.  ,c.   .cc.    cWWWWWWWWWWWO'      'XWWWW0      oWWWW0.   ;WWWWx   .oKWWWWWWWWWK:  oWWWWWWWWWWWc .WWWWWWWWWWWK'  \n"
        "          ::.OXKc    ,k0x   .;     ';    .XWWWN:;:xWWWWW;    cWWWWWWW.    .XWWWWWX'  OWWWN. .OWWWNxc;;c0W0o, .XWWW0;;;;;;;  oWWWWo;;xWWWWK  \n"
        "          . dXX:      dXK.  .      .     oWWWWo    xWWWWd   xWWWlKWWW:    oWWWWWWWN;,WWWWx 'NWWW0.           oWWWWKxxxxx.  .NWWWK...xWWWWd  \n"
        "           ,0Xo       .0Xx              .NWWWX.    OWWWW: .KWWWc 0WWWx   .NWWWKKWWWWNWWWW' OWWWW,           .NWWWWWWWWWK   oWWWWWWWWWWWK:   \n"
        "          .ddl         ,kx'             cWWWWl   .kWWWWd ;NWWWNOONWWWK   oWWWW: kWWWWWWWO  kWWWWo     c:.   oWWWWc        .NWWWXONWWWWx     \n"
        "         .od:          .dd,             KWWWW0kOKWWWWK; cWWWWkxxxKWWWW' .NWWWX   lWWWWWW'  .KWWWWKkk0WWWWd .NWWWWOkkkkkk. dWWWW; ;WWWWN.    \n"
        "        .do'            od.            cWWWWWWWWX0xc. .OWWWX,    oWWWWl xWWWW:    ;NWWWx     ckXWWWWNKkl.  xWWWWWWWWWWWx .WWWWK   lWWWWK.   \n"
        "       ,lo:.           .lo.                                                                      ..                                         \n"
        "     .:c:..           ,:::.                                                                                                                 \n\n"
        " Version "+std::to_string(konacore::version[0])+"."+std::to_string(konacore::version[1])+"."+std::to_string(konacore::version[2])+" by konacode | https://konacode.com/\n");
    
    logger::log("Initializing...");
    if (!path.empty()) {
        logger::log("Konata Dancer will be loading \""+path.string()+"\".");
    } 
    // else { path = "./konata.gif"; }

    logger::log("Getting pixel data from STB...",logger::dbg);
    int iw,ih;
    if (!path.empty())
        stbi_load(path.c_str(),&iw,&ih,nullptr,STBI_rgb_alpha);
    else {
        iw = 640; ih = 480;
    }
    logger::log("Creating window object...",logger::dbg);

    konanix w(iw,ih);
    g_konanix = &w;
    w.image_size = iw*ih*4;
    try {
        w.initialize();
    } catch (std::exception &e) {
        logger::log("Could not initialize Vulkan! Exception details: "+std::string(e.what()),logger::err);
        exit(1);
    }

    logger::log("Loading animated GIF...", logger::dbg);

    const GifAnimation anim = load_gif_animation(path);
    w.create_gif_image(anim.width, anim.height);
    w.create_descriptor_set();
    struct sigaction sigIntHandler {
        terminate_handler,
        {static_cast<unsigned long>(sigemptyset(&sigIntHandler.sa_mask))},
        0
    };

    sigaction(SIGINT, &sigIntHandler, NULL);
    size_t frame_index = 0;
    // auto next_frame_time = std::chrono::steady_clock::now() +
    // std::chrono::milliseconds(std::max(1, anim.frames[0].delay_ms));
    konanix::Overlay ctx; // context menu renderer
    logger::log("Initialized!");
    logger::log("Started rendering loop!");
    while (!glfwWindowShouldClose(w.g_window)) {
        // const auto now = std::chrono::steady_clock::now();
        // if (now >= next_frame_time) {
        std::this_thread::sleep_for(std::chrono::milliseconds(anim.frames[frame_index].delay_ms));
            w.upload_rgba_frame_to_gif_image(
                anim.frames[frame_index].rgba.data(),
                anim.frames[frame_index].rgba.size(),
                anim.width,
                anim.height,
                frame_index == 0
            );
            frame_index = (frame_index + 1) % anim.frames.size();
            // next_frame_time = now + std::chrono::milliseconds(std::max(1, anim.frames[frame_index].delay_ms));
        // }
        // int wi,he;
        // glfwGetWindowSize(w.g_window,&wi,&he);
        // logger::log("GLFW window size: "+std::to_string(wi)+"x"+std::to_string(he));
        w.draw_frame();
        glfwPollEvents();
    }

    if (g_gif_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(w.get_device(), g_gif_sampler, nullptr);
        g_gif_sampler = VK_NULL_HANDLE;
    }

    if (g_gif_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(w.get_device(), g_gif_image_view, nullptr);
        g_gif_image_view = VK_NULL_HANDLE;
    }

    if (g_gif_image != VK_NULL_HANDLE) {
        vkDestroyImage(w.get_device(), g_gif_image, nullptr);
        g_gif_image = VK_NULL_HANDLE;
    }

    if (g_gif_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(w.get_device(), g_gif_image_memory, nullptr);
        g_gif_image_memory = VK_NULL_HANDLE;
    }
    return 0;
}

void konanix::draw_frame() {
    // glfwSetWindowSize(g_window,width,height);
    vkWaitForFences(g_device,1,&g_in_flight_fences[current_frame],VK_TRUE,UINT64_MAX);
    // vkResetFences(g_device,1,&g_in_flight_fences[current_frame]);

    uint32_t image_index;
    // vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_image_available_semaphores[current_frame], VK_NULL_HANDLE, &image_index);
    VkResult result = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_image_available_semaphores[current_frame], VK_NULL_HANDLE, &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swap_chain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        logger::log("<Vulkan> Failed to acquire swap chain image!",logger::exc);
        throw std::runtime_error("failed to acquire swap chain image");
    }    
    
    vkResetFences(g_device,1,&g_in_flight_fences[current_frame]);

    vkResetCommandBuffer(g_commandbuffers[current_frame],0);
    record_command_buffer(g_commandbuffers[current_frame], image_index);
    
    const VkSemaphore wait_semaphores[] = {g_image_available_semaphores[current_frame]};
    const VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signal_semaphores[] = {g_render_finished_semaphores[current_frame]};

    const VkSubmitInfo submit_info {
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        VK_NULL_HANDLE,

        1,
        wait_semaphores,
        wait_stages,

        1,
        &g_commandbuffers[current_frame],

        1,
        signal_semaphores
    };
    if (vkQueueSubmit(g_graphicsqueue,1,&submit_info,g_in_flight_fences[current_frame]) != VK_SUCCESS) {
        logger::log("<Vulkan> Failed to submit draw command buffer!",logger::exc);
        throw std::runtime_error("failed to submit draw command buffer");
    } 
    

    const VkSwapchainKHR swapchains[] = {g_swapchain};

    const VkPresentInfoKHR present_info {
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        VK_NULL_HANDLE,

        1,
        signal_semaphores,

        1,
        swapchains,

        &image_index,

        nullptr
    };
    vkQueuePresentKHR(g_presentqueue,&present_info);
    current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
};
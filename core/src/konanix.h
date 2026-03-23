#pragma once
#include <vulkan/vulkan.h>
#include <atomic>

class konanix {
public:
    void initialize();
    void render();
    std::atomic<bool> running {true};
private:
    void create_instance();
    void create_device();
    void create_surface();
    constexpr static short version[3] = {1, 0, 0};


protected:
    VkInstance g_instance;
    VkPhysicalDevice g_physicaldevice;
    VkDevice g_device;


};
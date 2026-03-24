#pragma once
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


    constexpr static short version[3] = {1, 0, 0};

protected:
    GLFWwindow* g_window;
    VkInstance g_instance;
    VkSurfaceKHR g_surface;
    VkPhysicalDevice g_physicaldevice;
    VkDevice g_device;
    VkQueue g_graphicsqueue;


};
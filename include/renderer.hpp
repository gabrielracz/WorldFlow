#ifndef RENDERER_HPP_
#define RENDERER_HPP_

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <SDL2/SDL.h>
#include <SDL_vulkan.h>

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include "renderer_structs.hpp"
#include "camera.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "defines.hpp"
#include "vk_descriptors.h"

#include <variant>

namespace Constants
{
    constexpr bool IsValidationLayersEnabled = true;
    constexpr bool VSYNCEnabled = true;

    constexpr uint32_t FPSMeasurePeriod = 60;
    constexpr uint32_t FrameOverlap = 2;
    constexpr uint64_t TimeoutNs = 100000000;
    constexpr uint32_t MaxDescriptorSets = 10;

    constexpr glm::vec3 CameraPosition = glm::vec3(0.0, 0.0, -3.0);
    constexpr VkExtent3D DrawImageResolution {2560, 1440, 1};
}

class Renderer
{
public:
    Renderer(const std::string &name, uint32_t width, uint32_t height);
    ~Renderer();

    bool Init();
    bool ShouldClose() const;
    void Close();

    void RegisterUpdateCallback(std::function<void(VkCommandBuffer,float)>&& callback);

    void Update(float dt);
    bool ImmediateSubmit(std::function<void(VkCommandBuffer)>&& immediateFunction);

    void CreateImage(Image &newImg, VkExtent3D extent, VkImageUsageFlags usageFlags, VkImageAspectFlags aspectFlags, VkFormat format, bool autoCleanup = true);
    void CreateBuffer(Buffer &newBuffer, uint64_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, bool autoCleanup = true);
    bool CreateComputePipeline(ComputePipeline& newPipeline, const std::string& shaderFilename, std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors,
                               VkShaderStageFlags descriptorShaderStages, uint32_t pushConstantsSize, bool autoCleanup = true);
    bool CreateGraphicsPipeline(GraphicsPipeline &pipeline, const std::string &vertexShaderFile, const std::string &fragmentShaderFile, const std::string &geometryShaderFile, 
                                std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors, VkShaderStageFlags descriptorShaderStages, uint32_t pushConstantsSize, GraphicsPipelineOptions options = {}, bool autoCleanup = true);

    VkExtent3D GetWorkgroupCounts(uint32_t localGroupSize = 16);
private:
    bool render(float dt);

    bool initWindow();
    bool initVulkan();
    bool initSwapchain();
    bool initCommands();
    bool initSyncStructures();
    bool initResources();
    bool initDescriptorPool();
    bool initCamera();
    void destroySwapchain();
    void destroyVulkan();

    template<typename PipelineType>
    void createPipelineDescriptors(PipelineType& pipeline, VkShaderStageFlags shaderStages, std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors);

    bool createSwapchain(uint32_t width, uint32_t height);
    void resizeSwapchain();


    FrameData& getCurrentFrame();
    void pollEvents();
    void updatePerformanceCounters(float dt);

private:
    /* ENGINE */
    std::string _name {};
    SDL_Window* _window {};
    VkExtent3D _windowExtent {}; 
    VkExtent2D _drawExtent;
    DeletionQueue _deletionQueue;
    std::atomic<bool> _shouldClose {false};
    bool _isInitialized {};
    bool _resizeRequested {false};
    double _elapsed {};
    double _lastFpsMeasurementTime {};
    Mouse _mouse;
    MouseMap _mouseButtons;
    KeyMap _keyMap;

    std::function<void(VkCommandBuffer, float)> _userUpdate;

    /* RENDER DATA */
    Camera _camera;
    Transform _origin;
    Image _drawImage;

    /* VULKAN */
    VkInstance _instance {};
    VkDebugUtilsMessengerEXT _debugMessenger {};
    VkPhysicalDevice _gpu {};
    VkDevice _device {};
    VkSurfaceKHR _surface {};
    VkQueue _graphicsQueue {};
    uint32_t _graphicsQueueFamily {};

    VmaAllocator _allocator {};

    // swapchain
    VkSwapchainKHR _swapchain;
    VkFormat _swapchainImageFormat;
    std::vector<Image> _swapchainImages;
    // std::vector<VkImage> _swapchainImages;
    // std::vector<VkImageView> _swapchainImageViews;
    VkExtent2D _swapchainExtent;

    //immediate submit structures
    VkFence _immFence;
    VkCommandBuffer _immCommandBuffer;
    VkCommandPool _immCommandPool;

    DescriptorWriter _descriptorWriter {};
    DescriptorPool _descriptorPool;
    
    // frame storage
    FrameData _frames[Constants::FrameOverlap] {};
    uint32_t _frameNumber = 0;
};

#endif
#ifndef RENDERER_HPP_
#define RENDERER_HPP_

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>

#include <SDL2/SDL.h>
#include <SDL_vulkan.h>

// #include "vk_mem_alloc.h"
#include "vma.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include "renderer_structs.hpp"
#include "camera.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "vk_descriptors.h"

#include <variant>
#include <atomic>

namespace Constants
{
    constexpr uint32_t FrameOverlap = 2;
}

class Renderer
{
public:
    Renderer(const std::string &name, uint32_t width, uint32_t height);
    ~Renderer();

    bool Init();
    bool ShouldClose() const;
    void Close();

    void RegisterPreFrameCallback(std::function<void()>&& callback);
    void RegisterUpdateCallback(std::function<void(VkCommandBuffer,float)>&& callback);
    void RegisterUICallback(std::function<void()>&& callback);

    void Update(float dt);
    bool ImmediateSubmit(std::function<void(VkCommandBuffer)>&& immediateFunction);

    void CreateImage(Image &newImg, VkExtent3D extent, VkImageUsageFlags usageFlags, VkImageAspectFlags aspectFlags, VkFormat format, VkImageLayout layout, bool autoCleanup = true);
    void CreateBuffer(Buffer &newBuffer, uint64_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, bool autoCleanup = true);
    bool CreateComputePipeline(ComputePipeline& newPipeline, const std::string& shaderFilename, std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors,
                               uint32_t pushConstantsSize = 0, bool autoCleanup = true);
    bool CreateGraphicsPipeline(GraphicsPipeline &pipeline, const std::string &vertexShaderFile, const std::string &fragmentShaderFile, const std::string &geometryShaderFile = "", 
                                std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors = {}, VkShaderStageFlags descriptorShaderStages = VK_SHADER_STAGE_ALL_GRAPHICS, uint32_t pushConstantsSize = 0, GraphicsPipelineOptions options = {}, bool autoCleanup = true);
	void UploadMesh(GPUMesh& mesh, std::span<Vertex> vertices, std::span<uint32_t> indices);

    Camera& GetCamera();
    Image& GetDrawImage();
    VkViewport GetWindowViewport();
    VkRect2D GetWindowScissor();
    VkExtent2D GetWindowExtent2D();
    float GetElapsedTime();
    uint32_t GetFrameNumber();

	VkDevice GetDevice();
    VkExtent3D GetWorkgroupCounts(uint32_t localGroupSize = 16);
    KeyMap& GetKeyMap();
    MouseMap& GetMouseMap();
    Mouse& GetMouse();

public:
    static void FillBuffers(const std::vector<Buffer>& buffers, uint32_t data = 0, uint32_t offset = 0);

private:
    bool render(float dt);
    void drawUI(VkCommandBuffer cmd, VkImageView targetImageView);

    bool initWindow();
    bool initVulkan();
    bool initSwapchain();
    bool initCommands();
    bool initSyncStructures();
    bool initUI();
    bool initResources();
    bool initDescriptorPool();
    bool initCamera();
    bool initControls();


    void destroySwapchain();
    void destroyVulkan();

    template<typename PipelineType>
    void createPipelineDescriptors(PipelineType& pipeline, VkShaderStageFlags shaderStages, std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors);

    bool createSwapchain(uint32_t width, uint32_t height);
    void resizeSwapchain();


    FrameData& getCurrentFrame();
    void pollEvents();
	static int eventCallback(void* userdata, SDL_Event* event);
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
    MouseMap _mouseMap;
    KeyMap _keyMap;

    std::function<void()> _userPreFrame;
    std::function<void(VkCommandBuffer, float)> _userUpdate;
    std::function<void()> _userUI;

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
#include <SDL2/SDL.h>
#include "SDL_vulkan.h"
#include <cstddef>
#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <glm/glm.hpp>

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <vulkan/vulkan_core.h>
#include <cstddef>

#include "utils.hpp"
#include "shared/vk_images.h"
#include "shared/vk_initializers.h"


namespace Constants
{
#ifdef NDEBUG
constexpr bool IsValidationLayersEnabled = false;
#else
constexpr bool IsValidationLayersEnabled = true;
#endif
constexpr uint32_t FrameOverlap = 2;
constexpr uint64_t TimeoutNs = 1000000000;
}

struct FrameData
{
    VkCommandPool commandPool {};
    VkCommandBuffer commandBuffer {};
    VkSemaphore swapchainSemaphore {};
    VkSemaphore renderSemaphore {};
    VkFence renderFence {};
};

class Renderer
{
public:
    Renderer(const std::string& name, uint32_t width, uint32_t height)
    :   _name(name), _windowExtent{.width = width, .height = height} {}

    ~Renderer()
    {
        if(!this->_isInitialized) {
            return;
        }

        this->_running = false;
        this->_renderThread.join();

        vkDeviceWaitIdle(this->_device);
        for(FrameData &frame : this->_frames) {
            vkDestroyCommandPool(this->_device, frame.commandPool, nullptr);
            vkDestroyFence(this->_device, frame.renderFence, nullptr);
            vkDestroySemaphore(this->_device, frame.swapchainSemaphore, nullptr);
            vkDestroySemaphore(this->_device, frame.renderSemaphore, nullptr);
        }

        this->destroySwapchain();
        this->destroyVulkan();
        SDL_DestroyWindow(this->_window);
        SDL_Quit();
    }

    bool Init()
    {
        this->_isInitialized =  this->initWindow() &&
                                this->initVulkan() &&
                                this->initSwapchain() &&
                                this->initCommands() &&
                                this->initSyncStructures();

        return this->_isInitialized;
    }

    void Run()
    {
        this->_running = true;
        this->_renderThread = std::thread(std::bind(&Renderer::renderLoop, this));
    }
    
    bool IsRunning() const { return this->_running; }

private:
    void pollEvents()
    {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_QUIT) {
                this->_running = false;
            }
        }
    }

    bool draw()
    {
        // wait for gpu to be done with last frame
        VK_ASSERT(vkWaitForFences(this->_device, 1, &this->getCurrentFrame().renderFence, true, Constants::TimeoutNs));
        VK_ASSERT(vkResetFences(this->_device, 1, &this->getCurrentFrame().renderFence));

        // register the semaphore to be signalled once the next frame (result of this call) is ready. does not block
        uint32_t swapchainImageIndex;
        VK_CHECK(vkAcquireNextImageKHR(this->_device, this->_swapchain, Constants::TimeoutNs, this->getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex));
        VkImage frameImage = this->_swapchainImages[swapchainImageIndex];

        VkCommandBuffer cmd = this->getCurrentFrame().commandBuffer;
        VK_ASSERT(vkResetCommandBuffer(cmd, 0)); // we can safely reset as we waited on the fence
        
        VkCommandBufferBeginInfo cmdBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };

        // begin recording commands
        VK_ASSERT(vkBeginCommandBuffer(cmd, &cmdBeginInfo));


        // make next swapchain image writeable
        vkutil::transition_image(cmd, frameImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue clearValue {
           (1 + std::sin(this->_frameNumber / 120.0f)) / 2.0f,
           (1 + std::cos(this->_frameNumber / 120.0f)) / 2.0f,
           (1 + std::sin(this->_frameNumber * 2.0f / 120.0f)) / 2.0f,
           1.0f
        };
        VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cmd, frameImage, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
        vkutil::transition_image(cmd, frameImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        VK_ASSERT(vkEndCommandBuffer(cmd)); //commands recorded

        // prepare queue submission
        VkCommandBufferSubmitInfo cmdInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = cmd
        };
        VkSemaphoreSubmitInfo waitSwapchainInfo { // use pre-registered swapchain here wait semaphore to wait until swapchain is ready
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = this->getCurrentFrame().swapchainSemaphore,
            .value = 1,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR
        };
        VkSemaphoreSubmitInfo signalRenderedInfo {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = this->getCurrentFrame().renderSemaphore,
            .value = 1,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT
        };
        VkSubmitInfo2 queueSubmitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &waitSwapchainInfo,
            
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmdInfo,

            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signalRenderedInfo
        };

        // renderFence would now block until rendering is done (frame.renderSemaphore will also signal at this time)
        VK_ASSERT(vkQueueSubmit2(this->_graphicsQueue, 1, &queueSubmitInfo, this->getCurrentFrame().renderFence));

        // present rendered image:
        VkPresentInfoKHR presentInfo {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &this->getCurrentFrame().renderSemaphore, //wait for render complete signal

            .swapchainCount = 1,
            .pSwapchains = &this->_swapchain,
            .pImageIndices = &swapchainImageIndex,
        };
        VK_ASSERT(vkQueuePresentKHR(this->_graphicsQueue, &presentInfo));
        this->_frameNumber++;
        return true;
    }

    void renderLoop()
    {
        while(this->_running) {
            this->pollEvents();
            this->draw();
            std::this_thread::sleep_for(std::chrono::duration(std::chrono::milliseconds(16)));
        }
    }

    bool initWindow()
    {
        SDL_Init(SDL_INIT_VIDEO);
        SDL_WindowFlags windowFlags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);
        this->_window = SDL_CreateWindow(
            this->_name.c_str(),
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            this->_windowExtent.width,
            this->_windowExtent.height,
            windowFlags
        );
        return this->_window != nullptr;
    }

    bool initVulkan()
    {
        vkb::InstanceBuilder builder;
        vkb::Result<vkb::Instance> instanceRet = builder
            .set_app_name(this->_name.c_str())
            .request_validation_layers(Constants::IsValidationLayersEnabled)
            .use_default_debug_messenger()
            .require_api_version(1, 3, 0)
            .build();
        
        if(!instanceRet.has_value()) {
            std::cerr << "[ERROR] Failed to build the vulkan instance" << std::endl;
            return false;
        }

        vkb::Instance vkbInstance =  instanceRet.value();
        this->_instance = vkbInstance.instance;
        this->_debugMessenger = vkbInstance.debug_messenger;


        SDL_Vulkan_CreateSurface(this->_window, this->_instance, &this->_surface);
        VkPhysicalDeviceVulkan13Features features {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .synchronization2 = true,
            .dynamicRendering = true
        };

        VkPhysicalDeviceVulkan12Features features12 {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .descriptorIndexing = true,
            .bufferDeviceAddress = true
        };

        vkb::PhysicalDeviceSelector selector {vkbInstance};
        vkb::PhysicalDevice physDevice = selector
            .set_minimum_version(1, 3)
            .set_required_features_13(features)
            .set_required_features_12(features12)
            .set_surface(this->_surface)
            .select()
            .value();
        
        vkb::DeviceBuilder deviceBuilder {physDevice};
        vkb::Device vkbDevice = deviceBuilder.build().value();
        this->_device = vkbDevice.device;
        this->_gpu = physDevice.physical_device;
        std::cout << physDevice.properties.deviceName << std::endl;

        this->_graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
        this->_graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
        return true;
    }

    bool createSwapchain(uint32_t width, uint32_t height)
    {
        vkb::SwapchainBuilder swapchainBuilder {this->_gpu, this->_device, this->_surface};
        this->_swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        vkb::Swapchain vkbSwapchain = swapchainBuilder
            .set_desired_format(VkSurfaceFormatKHR{.format = this->_swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_RELAXED_KHR)
            .set_desired_extent(width, height)
            .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            .build()
            .value();

        this->_swapchainExtent     = vkbSwapchain.extent;
        this->_swapchainImages     = vkbSwapchain.get_images().value();
        this->_swapchainImageViews = vkbSwapchain.get_image_views().value();
        this->_swapchain           = vkbSwapchain.swapchain;
        return true;
    }

    bool initSwapchain()
    {
        return createSwapchain(this->_windowExtent.width, this->_windowExtent.height);
    }

    bool initCommands()
    {
        const VkCommandPoolCreateInfo cmdPoolInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = this->_graphicsQueueFamily
        };

        for(FrameData &frame : this->_frames) {
            VK_ASSERT(vkCreateCommandPool(this->_device, &cmdPoolInfo, nullptr, &frame.commandPool));

            const VkCommandBufferAllocateInfo cmdAllocInfo {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = frame.commandPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VK_ASSERT(vkAllocateCommandBuffers(this->_device, &cmdAllocInfo, &frame.commandBuffer));
        }

        return true;

    }
    bool initSyncStructures()
    {
        const VkFenceCreateInfo fenceInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT // don't block on first wait
        };

        const VkSemaphoreCreateInfo semaphoreInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };

        for(FrameData &frame : this->_frames) {
            VK_ASSERT(vkCreateFence(this->_device, &fenceInfo, nullptr, &frame.renderFence));
            VK_ASSERT(vkCreateSemaphore(this->_device, &semaphoreInfo, nullptr, &frame.swapchainSemaphore));
            VK_ASSERT(vkCreateSemaphore(this->_device, &semaphoreInfo, nullptr, &frame.renderSemaphore));
        }

        return true;
    }

    void destroySwapchain()
    {
        vkDestroySwapchainKHR(this->_device, this->_swapchain, nullptr);
        for(const VkImageView& view : this->_swapchainImageViews)
        {
            vkDestroyImageView(this->_device, view, nullptr);
        }
    }

    void destroyVulkan()
    {
        vkDestroySurfaceKHR(this->_instance, this->_surface, nullptr);
        vkDestroyDevice(this->_device, nullptr);
        vkb::destroy_debug_utils_messenger(this->_instance, this->_debugMessenger);
        vkDestroyInstance(this->_instance, nullptr);
    }

    FrameData& getCurrentFrame()
    { 
        return this->_frames[this->_frameNumber % Constants::FrameOverlap];
    }


private:
    SDL_Window* _window {};
    VkExtent2D _windowExtent {}; 
    bool _isInitialized {false};
    std::thread _renderThread;

    VkInstance _instance {};
    VkDebugUtilsMessengerEXT _debugMessenger {};
    VkPhysicalDevice _gpu {};
    VkDevice _device {};
    VkSurfaceKHR _surface {};

    VkSwapchainKHR _swapchain;
    VkFormat _swapchainImageFormat;
    std::vector<VkImage> _swapchainImages;
    std::vector<VkImageView> _swapchainImageViews;
    VkExtent2D _swapchainExtent;

    FrameData _frames[Constants::FrameOverlap] {};
    uint32_t _frameNumber {};

    VkQueue _graphicsQueue {};
    uint32_t _graphicsQueueFamily {};

    std::string _name {};
    std::atomic<bool> _running {false};
};

int main(int argc, char* argv[])
{
    // std::cout << "hello" << std::endl;
    SDL_Log("hello");
    Renderer renderer("VulkanFlow", 600, 600);
    if(!renderer.Init()) {
        std::cerr << "[ERROR] Failed to initialize renderer" << std::endl;
        return EXIT_FAILURE;
    }

    renderer.Run();
    while(renderer.IsRunning()) {}

    return EXIT_SUCCESS;
}
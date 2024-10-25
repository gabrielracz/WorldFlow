#include <SDL2/SDL.h>
#include "SDL_vulkan.h"

#include <chrono>
#include <system_error>
#include <vulkan/vulkan.h>
#include <VkBootstrap.h>

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"


#include <stb_image.h>
#include <glm/glm.hpp>

#include "path_config.hpp"
#include "utils.hpp"
#include "shared/vk_images.h"
#include "shared/vk_initializers.h"
#include "shared/vk_descriptors.h"
#include "shared/vk_pipelines.h"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <vulkan/vulkan_core.h>
#include <cstddef>
#include <deque>
#include <csignal>



namespace Constants
{
#ifdef NDEBUG
constexpr bool IsValidationLayersEnabled = false;
#else
constexpr bool IsValidationLayersEnabled = true;
#endif
constexpr uint32_t FrameOverlap = 2;
constexpr uint64_t TimeoutNs = 1000000000;
constexpr uint32_t MaxDescriptorSets = 10;
constexpr uint32_t DiffusionIterations = 5;
}

static_assert(Constants::DiffusionIterations % 2 == 1); //should be odd to ensure consistency of final result index

// Actually a LIFO stack
struct DeletionQueue
{
    void push(std::function<void()>&& function) { deletors.push_front(function); }
    void flush()
    {
        for(auto func : deletors) {
            func();
        }
        deletors.clear();
    }
    std::deque<std::function<void()>> deletors;
};

struct FrameData
{
    VkCommandPool commandPool {};
    VkCommandBuffer commandBuffer {};
    VkSemaphore swapchainSemaphore {};
    VkSemaphore renderSemaphore {};
    VkFence renderFence {};
    DeletionQueue deletionQueue;
};

struct ComputePipeline
{
    VkPipeline pipeline;
    VkPipelineLayout layout;
};

struct ComputePushConstants
{
    float time {};
    float dt {};
};

static inline void
printDeviceProperties(vkb::PhysicalDevice& dev)
{
    const uint32_t* wgs = dev.properties.limits.maxComputeWorkGroupCount;
    std::cout << dev.properties.deviceName << "\n" <<
    "WorkGroups:      " << wgs[0] << " x " << wgs[1] << " x " << wgs[2] << ") \n" << 
    "PushConstants:   " << dev.properties.limits.maxPushConstantsSize << "\n" <<
    "Uniform Buffers: " << dev.properties.limits.maxUniformBufferRange << std::endl;
}


//TODO change all return bool members that VK_ASSERT to return Result code
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

        vkDeviceWaitIdle(this->_device);
        for(FrameData &frame : this->_frames) {
            vkDestroyCommandPool(this->_device, frame.commandPool, nullptr);
            vkDestroyFence(this->_device, frame.renderFence, nullptr);
            vkDestroySemaphore(this->_device, frame.swapchainSemaphore, nullptr);
            vkDestroySemaphore(this->_device, frame.renderSemaphore, nullptr);
            frame.deletionQueue.flush();
        }
        this->_deletionQueue.flush();
        destroySwapchain();
        destroyVulkan();
        SDL_DestroyWindow(this->_window);
        SDL_Quit();
    }

    bool Init()
    {
        this->_isInitialized =  initWindow() &&
                                initVulkan() &&
                                initSwapchain() &&
                                initCommands() &&
                                initSyncStructures() &&
                                initAssets() &&
                                initDescriptors() &&
                                initBuffers() &&
                                initPipelines()
                                ;

        return this->_isInitialized;
    }

    void Render(double dt)
    {
        this->_elapsed += dt;
        pollEvents();
        draw(dt);
    }
    
    bool ShouldClose() const { return this->_shouldClose; }
    void Close() { this->_shouldClose = true; }

private:
    void pollEvents()
    {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_QUIT) {
                this->_shouldClose = true;
            }

        }
    }

    // NOT SAFE TO RUN AFTER RENDER LOOP STARTED:
    // TODO: update to use different queue so concurrent immediate operations are safe
    bool immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& immediateFunction)
    {
        VK_ASSERT(vkResetFences(this->_device, 1, &this->_immFence));
        VK_ASSERT(vkResetCommandBuffer(this->_immCommandBuffer, 0));

        // record the command buffer using user function
        VkCommandBuffer cmd = this->_immCommandBuffer;
        VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        VK_ASSERT(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

        immediateFunction(cmd);

        VK_ASSERT(vkEndCommandBuffer(cmd));

        // submit gpu commands
        VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);
        VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, nullptr, nullptr);
        VK_ASSERT(vkQueueSubmit2(this->_graphicsQueue, 1, &submit, this->_immFence));

        VK_ASSERT(vkWaitForFences(this->_device, 1, &this->_immFence, true, 9999999999));
        return true;
    }

    void drawBackground(VkCommandBuffer cmd, double dt)
    {
        // VkClearColorValue clearValue {
        //    (1 + std::sin(this->_frameNumber / 120.0f)) / 2.0f,
        //    (1 + std::cos(this->_frameNumber / 120.0f)) / 2.0f,
        //    (1 + std::sin(this->_frameNumber * 2.0f / 120.0f)) / 2.0f,
        //    1.0f
        // };
        // VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        // vkCmdClearColorImage(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computePipeline);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computePipelineLayout, 0, 1, &this->_drawImageDescriptorSet, 0, nullptr);

        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed),
            .dt   = static_cast<float>(dt)
        };
        vkCmdPushConstants(cmd, this->_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);

        vkCmdDispatch(cmd, std::ceil(this->_drawImage.imageExtent.width/16.0), std::ceil(this->_drawImage.imageExtent.height/16.0), 1);
    }

    bool addDensity(VkCommandBuffer cmd, double dt)
    {
        
        return true;
    }

    void diffuseDensity(VkCommandBuffer cmd, double dt)
    {
        for(AllocatedImage& img : this->_densityImages) {
            vkutil::transition_image(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        }
        VkImageMemoryBarrier imageBarrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, // Waiting for shader writes to complete
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT, // Next dispatch can only read
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL, // Expected layout before the barrier
            .newLayout = VK_IMAGE_LAYOUT_GENERAL, // Expected layout after the barrier
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = VK_NULL_HANDLE, // UPDATE
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, // Assuming a color image
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computePipeline);
        for(int i = 0; i < Constants::DiffusionIterations; i++) {
            int pingPongDescriptorIndex = i % 2;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computePipelineLayout, 0, 1, &this->_diffusionDescriptorSets[pingPongDescriptorIndex], 0, nullptr);

            ComputePushConstants constants = {
                .time = static_cast<float>(this->_elapsed),
                .dt   = static_cast<float>(dt)
            };
            vkCmdPushConstants(cmd, this->_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);

            vkCmdDispatch(cmd, std::ceil(this->_drawImage.imageExtent.width/16.0), std::ceil(this->_drawImage.imageExtent.height/16.0), 1);
            
            // wait for all image writes to be complete
            imageBarrier.image = this->_densityImages[pingPongDescriptorIndex].image;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &imageBarrier);
        }
    }

    bool draw(double dt)
    {
        // wait for gpu to be done with last frame and clean
        VK_ASSERT(vkWaitForFences(this->_device, 1, &getCurrentFrame().renderFence, true, Constants::TimeoutNs));
        VK_ASSERT(vkResetFences(this->_device, 1, &getCurrentFrame().renderFence));
        getCurrentFrame().deletionQueue.flush(); // delete per-frame objects

        // register the semaphore to be signalled once the next frame (result of this call) is ready. does not block
        uint32_t swapchainImageIndex;
        VK_CHECK(vkAcquireNextImageKHR(this->_device, this->_swapchain, Constants::TimeoutNs, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex));
        VkImage frameImage = this->_swapchainImages[swapchainImageIndex];

        VkCommandBuffer cmd = getCurrentFrame().commandBuffer;
        VK_ASSERT(vkResetCommandBuffer(cmd, 0)); // we can safely reset as we waited on the fence


        // begin recording commands
        VkCommandBufferBeginInfo cmdBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };
        VK_ASSERT(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
        
        addDensity(cmd, dt);
        diffuseDensity(cmd, dt);
        // advectDensity(cmd, dt);

        // addVelocity
        // diffuseVelocity
        // projectIncompressible
        // advectVelocity
        // projectIncompressible

        
        // prepare draw image -> swapchain image copy
        AllocatedImage& drawImage = this->_densityImages[0];
        VkImage& swapchainImage = this->_swapchainImages[swapchainImageIndex];

        vkutil::transition_image(cmd, drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkutil::copy_image_to_image(cmd, drawImage.image, swapchainImage, drawImage.imageExtent, VkExtent3D(this->_swapchainExtent.width, this->_swapchainExtent.height, 1));

        // transition to present format
        vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        VK_ASSERT(vkEndCommandBuffer(cmd)); //commands recorded

        // prepare queue submission
        VkCommandBufferSubmitInfo cmdInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = cmd
        };
        VkSemaphoreSubmitInfo waitSwapchainInfo { // use pre-registered swapchain here wait semaphore to wait until swapchain is ready
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = getCurrentFrame().swapchainSemaphore,
            .value = 1,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR
        };
        VkSemaphoreSubmitInfo signalRenderedInfo {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = getCurrentFrame().renderSemaphore,
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
        VK_ASSERT(vkQueueSubmit2(this->_graphicsQueue, 1, &queueSubmitInfo, getCurrentFrame().renderFence));

        // present rendered image:
        VkPresentInfoKHR presentInfo {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &getCurrentFrame().renderSemaphore, //wait for render complete signal

            .swapchainCount = 1,
            .pSwapchains = &this->_swapchain,
            .pImageIndices = &swapchainImageIndex,
        };
        VK_ASSERT(vkQueuePresentKHR(this->_graphicsQueue, &presentInfo));
        this->_frameNumber++;
        return true;
    }

    bool initWindow()
    {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
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
            std::cout << "[ERROR] Failed to build the vulkan instance" << std::endl;
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
        // print gpu properties
        printDeviceProperties(physDevice);

        this->_graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
        this->_graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        // initialize allocator
        VmaAllocatorCreateInfo allocatorInfo = {
            .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
            .physicalDevice = this->_gpu,
            .device = this->_device,
            .instance = this->_instance, 
        };
        vmaCreateAllocator(&allocatorInfo, &this->_allocator);
        this->_deletionQueue.push([&](){vmaDestroyAllocator(this->_allocator);});

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

    bool createDrawImage()
    {
        // Hardcord image format to float16 and extent to current window size
        // this->_drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        this->_drawImage.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        this->_drawImage.imageExtent = VkExtent3D {
            this->_windowExtent.width,
            this->_windowExtent.height,
            1
        };

        VkImageUsageFlags drawImageUses {};
        drawImageUses |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // allow copying from
        drawImageUses |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; // allow copying to
        drawImageUses |= VK_IMAGE_USAGE_STORAGE_BIT; // read-write access in compute shaders
        drawImageUses |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // able to be rendered to with graphics pipeline

        VkImageCreateInfo imgInfo = vkinit::image_create_info(this->_drawImage.imageFormat, drawImageUses, this->_drawImage.imageExtent);
        VmaAllocationCreateInfo imgAllocInfo = {
            .usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        };
        VK_ASSERT(vmaCreateImage(this->_allocator, &imgInfo, &imgAllocInfo, &this->_drawImage.image, &this->_drawImage.allocation, nullptr));

        // create simple 1-1 view
        VkImageViewCreateInfo imgViewInfo = vkinit::imageview_create_info(this->_drawImage.imageFormat, this->_drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
        VK_ASSERT(vkCreateImageView(this->_device, &imgViewInfo, nullptr, &this->_drawImage.imageView));

        this->_deletionQueue.push([&]() {
            vkDestroyImageView(this->_device, this->_drawImage.imageView, nullptr);
            vmaDestroyImage(this->_allocator, this->_drawImage.image, this->_drawImage.allocation);
        });
        return true;
    }

    bool initSwapchain()
    {
        if(!createSwapchain(this->_windowExtent.width, this->_windowExtent.height)) {
            std::cout << "[ERROR] Failed to create swapchain" << std::endl;
            return false;
        }
        if(!createDrawImage()) {
            std::cout << "[ERROR] Failed to create draw image" << std::endl;
            return false;
        }
        return true;
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

        // immediate mode
        VK_ASSERT(vkCreateCommandPool(this->_device, &cmdPoolInfo, nullptr, &this->_immCommandPool));
        VkCommandBufferAllocateInfo immCmdAllocInfo = vkinit::command_buffer_allocate_info(this->_immCommandPool, 1);
        VK_ASSERT(vkAllocateCommandBuffers(this->_device, &immCmdAllocInfo, &this->_immCommandBuffer));
        this->_deletionQueue.push([&]() {
            vkDestroyCommandPool(this->_device, this->_immCommandPool, nullptr);
        });

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

        VK_ASSERT(vkCreateFence(this->_device, &fenceInfo, nullptr, &this->_immFence));
        this->_deletionQueue.push([&](){ vkDestroyFence(this->_device, this->_immFence, nullptr); });
        return true;
    }

    AllocatedBuffer createBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, bool autoCleanup = true)
    {
        // allocate buffer
        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = allocSize,
            .usage = usage
        };
        VmaAllocationCreateInfo vmaallocInfo = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = memoryUsage,
        };

        AllocatedBuffer newBuffer;
        VK_CHECK(vmaCreateBuffer(this->_allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info));
        if(autoCleanup) {
            this->_deletionQueue.push([this, newBuffer]() {
                vmaDestroyBuffer(this->_allocator, newBuffer.buffer, newBuffer.allocation);
            });
        }
        return newBuffer;
    }

    bool initBuffers()
    {
        return true;
    }

    AllocatedImage createImage(VkExtent3D extent, VkImageUsageFlags usageFlags, VkImageAspectFlags aspectFlags, VkFormat format, bool autoCleanup = true)
    {
        AllocatedImage newImg {};
        newImg.imageExtent = extent;
        newImg.imageFormat = format;
        VkImageCreateInfo imgCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = newImg.imageFormat,
            .extent = newImg.imageExtent,
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, //MSAA samples, 1 = disabled
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usageFlags
        };
        VmaAllocationCreateInfo imgAllocCreateInfo = {
            .usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        };
        VK_CHECK(vmaCreateImage(this->_allocator, &imgCreateInfo, &imgAllocCreateInfo, &newImg.image, &newImg.allocation, &newImg.info));

        VkImageViewCreateInfo imgViewCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = newImg.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = newImg.imageFormat,
            .subresourceRange = {
                .aspectMask = aspectFlags,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            }
        };
        VK_CHECK(vkCreateImageView(this->_device, &imgViewCreateInfo, nullptr, &newImg.imageView));

        if(autoCleanup) {
            this->_deletionQueue.push([this, newImg]() {
                vmaDestroyImage(this->_allocator, newImg.image, newImg.allocation);
                vkDestroyImageView(this->_device, newImg.imageView, nullptr);
            });
        }
        return newImg;
    }

    bool copyBufferToImage(AllocatedBuffer buffer, AllocatedImage image, VkImageLayout finalLayout = VK_IMAGE_LAYOUT_GENERAL)
    {
        immediateSubmit([buffer, image, finalLayout](VkCommandBuffer cmd) {
            vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copyRegion = {};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;

            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageExtent = image.imageExtent;

            vkCmdCopyBufferToImage(cmd, buffer.buffer, image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

            vkutil::transition_image(cmd, image.image, 
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, finalLayout);
        });
        return true;
    }

    bool initAssets()
    {
        int w, h, nrChannel;
        unsigned char* data = stbi_load(ASSETS_DIRECTORY"/monet-alpha-square.png", &w, &h, &nrChannel, STBI_rgb_alpha);
        VkExtent3D dataExtent {.width = (unsigned int)w, .height = (unsigned int)h, .depth = 1};
        this->_stagingBuffer = createBuffer( // allows for CPU -> GPU copies
            w*h*nrChannel*2,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        std::memcpy(this->_stagingBuffer.info.pMappedData, data, w*h*nrChannel);

        for(AllocatedImage& img : this->_densityImages) {
            img = createImage(
                dataExtent,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_FORMAT_R8G8B8A8_UNORM
            );
        }
        copyBufferToImage(this->_stagingBuffer, this->_densityImages[0]);

        for(AllocatedImage& img : this->_velocityImages) {
            img = createImage(
                dataExtent,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_FORMAT_R16G16B16A16_SFLOAT
            );
        }

        immediateSubmit([&](VkCommandBuffer cmd) {
            for(AllocatedImage& img : this->_densityImages)
                vkutil::transition_image(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        });

        return true;
    }

    bool initDescriptors()
    {
        std::vector<DescriptorPool::DescriptorQuantity> sizes = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}
        };
        this->_descriptorPool.init(this->_device, 10, sizes);

        // Define descriptor layout
        {
            DescriptorLayoutBuilder builder;
            builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            this->_drawImageDescriptorLayout = builder.build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        }
        this->_drawImageDescriptorSet = this->_descriptorPool.allocateSet(this->_device, this->_drawImageDescriptorLayout);

        this->_diffusionDescriptorSets[0] = this->_descriptorPool.allocateSet(this->_device, this->_drawImageDescriptorLayout);
        this->_diffusionDescriptorSets[1] = this->_descriptorPool.allocateSet(this->_device, this->_drawImageDescriptorLayout);


        VkSamplerCreateInfo samplerCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        };
        VK_ASSERT(vkCreateSampler(this->_device, &samplerCreateInfo, nullptr, &this->_simpleSampler));
        this->_deletionQueue.push([&]() { vkDestroySampler(this->_device, this->_simpleSampler, nullptr); });

        // Fill in the descriptors
        this->_descriptorWriter.clear();
        this->_descriptorWriter.write_image(0, this->_drawImage.imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        this->_descriptorWriter.update_set(this->_device, this->_drawImageDescriptorSet);

        // fill the diffusion sets
        this->_descriptorWriter.clear();
        this->_descriptorWriter.write_image(0, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        this->_descriptorWriter.write_image(1, this->_densityImages[1].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        this->_descriptorWriter.update_set(this->_device, this->_diffusionDescriptorSets[0]);
        this->_descriptorWriter.clear();
        this->_descriptorWriter.write_image(0, this->_densityImages[1].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        this->_descriptorWriter.write_image(1, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        this->_descriptorWriter.update_set(this->_device, this->_diffusionDescriptorSets[1]);

        this->_deletionQueue.push([&]() {
            this->_descriptorPool.destroy(this->_device);
            vkDestroyDescriptorSetLayout(this->_device, this->_drawImageDescriptorLayout, nullptr);
        });
        return true;
    }

    ComputePipeline creatComputePipeline(const std::string& shaderFilename, VkDescriptorSetLayout* descriptorLayout, bool autoCleanup = true)
    {
        ComputePipeline newPipeline;
        const VkPushConstantRange pushConstants = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(ComputePushConstants),
        };

        VkPipelineLayoutCreateInfo pipelineLayout = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = descriptorLayout,

            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstants
        };

        VK_CHECK(vkCreatePipelineLayout(this->_device, &pipelineLayout, nullptr, &newPipeline.layout));

        VkShaderModule computeDrawShader;
        if(!vkutil::load_shader_module(shaderFilename, this->_device, &computeDrawShader)) {
            std::cerr << "[ERROR] Failed to load compute shader " << shaderFilename << std::endl;
            return newPipeline;
        }

        VkPipelineShaderStageCreateInfo stageInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = computeDrawShader,
            .pName = "main"
        };

        VkComputePipelineCreateInfo computePipelineCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = stageInfo,
            .layout = newPipeline.layout
        };

        VK_CHECK(vkCreateComputePipelines(this->_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &newPipeline.pipeline));

        if(autoCleanup) {
            vkDestroyShaderModule(this->_device, computeDrawShader, nullptr);
            this->_deletionQueue.push([&]() {
                vkDestroyPipelineLayout(this->_device, newPipeline.layout, nullptr);
                vkDestroyPipeline(this->_device, newPipeline.pipeline, nullptr);
            });
        }
        return newPipeline;
    }

    bool initComputePipelines()
    {
        VkPushConstantRange pushConstants = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(ComputePushConstants),
        };

        VkPipelineLayoutCreateInfo computeLayout = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &this->_drawImageDescriptorLayout,

            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstants
        };
        VK_ASSERT(vkCreatePipelineLayout(this->_device, &computeLayout, nullptr, &this->_computePipelineLayout));

        VkShaderModule computeDrawShader;
        const char* shaderFileName = SHADER_DIRECTORY"/diffusion.comp.spv";
        if(!vkutil::load_shader_module(shaderFileName, this->_device, &computeDrawShader)) {
            std::cerr << "[ERROR] Failed to load compute shader " << shaderFileName << std::endl;
            return false;
        }

        VkPipelineShaderStageCreateInfo stageInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = computeDrawShader,
            .pName = "main"
        };

        VkComputePipelineCreateInfo computePipelineCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = stageInfo,
            .layout = this->_computePipelineLayout
        };

        VK_ASSERT(vkCreateComputePipelines(this->_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &this->_computePipeline));

        vkDestroyShaderModule(this->_device, computeDrawShader, nullptr);
        this->_deletionQueue.push([&]() {
            vkDestroyPipelineLayout(this->_device, this->_computePipelineLayout, nullptr);
            vkDestroyPipeline(this->_device, this->_computePipeline, nullptr);
        });
        return true;
    }

    bool initPipelines()
    {
        if(!initComputePipelines()) {
            std::cout << "[ERROR] Failed to init background pipelines" << std::endl;
            return false;
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
    DeletionQueue _deletionQueue;

    VkInstance _instance {};
    VkDebugUtilsMessengerEXT _debugMessenger {};
    VkPhysicalDevice _gpu {};
    VkDevice _device {};
    VkSurfaceKHR _surface {};
    VkQueue _graphicsQueue {};
    uint32_t _graphicsQueueFamily {};

    VkSwapchainKHR _swapchain;
    VkFormat _swapchainImageFormat;
    std::vector<VkImage> _swapchainImages;
    std::vector<VkImageView> _swapchainImageViews;
    VkExtent2D _swapchainExtent;

    VmaAllocator _allocator {};

    FrameData _frames[Constants::FrameOverlap] {};
    uint32_t _frameNumber {};
    AllocatedImage _drawImage;

    ComputePipeline addSourcePipeline;
    ComputePipeline diffusionPipeline;

    VkPipeline _computePipeline;
    VkPipelineLayout _computePipelineLayout;

    DescriptorPool _descriptorPool;
    VkDescriptorSet _drawImageDescriptorSet;
    VkDescriptorSet _diffusionDescriptorSets[2] {};
    VkDescriptorSetLayout _drawImageDescriptorLayout;
    DescriptorWriter _descriptorWriter;

    //immediate submit structures
    VkFence _immFence;
    VkCommandBuffer _immCommandBuffer;
    VkCommandPool _immCommandPool;

    std::string _name {};
    std::atomic<bool> _shouldClose {false};
    double _elapsed {};

    //Test Scene
    AllocatedImage _densityImages[2] {};
    AllocatedImage _velocityImages[2] {};
    AllocatedBuffer _stagingBuffer {};
    VkSampler _simpleSampler {};

};

int main(int argc, char* argv[])
{
    Renderer renderer("VulkanFlow", 600, 600);
    if(!renderer.Init()) {
        std::cout << "[ERROR] Failed to initialize renderer" << std::endl;
        return EXIT_FAILURE;
    }

    const double startTime = GetTimestamp();
    double lastFrameTime = startTime;
    while(!renderer.ShouldClose()) {
        const double currentFrameTime = GetTimestamp();
        const double dt = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;
        
        renderer.Render(dt);
        // std::this_thread::sleep_for(std::chrono::duration(std::chrono::milliseconds(16)));
    }
    std::cout << "Close" << std::endl;
    return EXIT_SUCCESS;
}
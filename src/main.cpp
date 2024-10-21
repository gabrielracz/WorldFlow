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
}

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

struct ComputePushConstants
{
    float time {};
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
        this->_mainDeletionQueue.flush();
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
                                this->initSyncStructures() &&
                                // this->initDescriptors() &&
                                this->initFeedbackShader() &&
                                this->initBuffers() &&
                                this->initPipelines()
                                ;

        return this->_isInitialized;
    }

    void Render(double dt)
    {
        this->_elapsed += dt;
        this->pollEvents();
        this->draw();
    }
    
    bool ShouldClose() const { return this->_shouldClose; }

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

    void drawBackground(VkCommandBuffer cmd)
    {
        // VkClearColorValue clearValue {
        //    (1 + std::sin(this->_frameNumber / 120.0f)) / 2.0f,
        //    (1 + std::cos(this->_frameNumber / 120.0f)) / 2.0f,
        //    (1 + std::sin(this->_frameNumber * 2.0f / 120.0f)) / 2.0f,
        //    1.0f
        // };
        // VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        // vkCmdClearColorImage(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_gradientPipeline);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_gradientPipelineLayout, 0, 1, &this->_drawImageDescriptorSet, 0, nullptr);

        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed)
        };
        vkCmdPushConstants(cmd, this->_gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);

        vkCmdDispatch(cmd, std::ceil(this->_drawImage.imageExtent.width/16.0), std::ceil(this->_drawImage.imageExtent.height/16.0), 1);
    }

    bool draw()
    {
        // wait for gpu to be done with last frame and clean
        VK_ASSERT(vkWaitForFences(this->_device, 1, &this->getCurrentFrame().renderFence, true, Constants::TimeoutNs));
        VK_ASSERT(vkResetFences(this->_device, 1, &this->getCurrentFrame().renderFence));
        this->getCurrentFrame().deletionQueue.flush(); // delete per-frame objects

        // register the semaphore to be signalled once the next frame (result of this call) is ready. does not block
        uint32_t swapchainImageIndex;
        VK_CHECK(vkAcquireNextImageKHR(this->_device, this->_swapchain, Constants::TimeoutNs, this->getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex));
        VkImage frameImage = this->_swapchainImages[swapchainImageIndex];

        VkCommandBuffer cmd = this->getCurrentFrame().commandBuffer;
        VK_ASSERT(vkResetCommandBuffer(cmd, 0)); // we can safely reset as we waited on the fence


        // begin recording commands
        VkCommandBufferBeginInfo cmdBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };
        VK_ASSERT(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
        // make draw image writeable
        vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(cmd, this->_feedbackImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        
        drawBackground(cmd);
        
        // TODO: abstract into image class.
        // prepare draw image -> swapchain image copy
        vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkutil::transition_image(cmd, this->_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkExtent2D drawExtent = {.width = this->_drawImage.imageExtent.width, .height = this->_drawImage.imageExtent.height};
        vkutil::copy_image_to_image(cmd, this->_drawImage.image, this->_swapchainImages[swapchainImageIndex], drawExtent, this->_swapchainExtent);

        // transition to present format
        vkutil::transition_image(cmd, this->_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
        this->_mainDeletionQueue.push([&](){vmaDestroyAllocator(this->_allocator);});

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
        this->_drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
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

        this->_mainDeletionQueue.push([&]() {
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
        this->_mainDeletionQueue.push([&]() {
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
        this->_mainDeletionQueue.push([&](){ vkDestroyFence(this->_device, this->_immFence, nullptr); });
        return true;
    }

    AllocatedBuffer createBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
    {
        // allocate buffer
        VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.pNext = nullptr;
        bufferInfo.size = allocSize;

        bufferInfo.usage = usage;

        VmaAllocationCreateInfo vmaallocInfo = {};
        vmaallocInfo.usage = memoryUsage;
        vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        AllocatedBuffer newBuffer;

        // allocate the buffer
        VK_CHECK(vmaCreateBuffer(this->_allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation,
            &newBuffer.info));
        return newBuffer;
    }

    bool initBuffers()
    {
        return true;
    }

    bool initFeedbackShader()
    {
        int w, h, nrChannel;
        unsigned char* data = stbi_load(ASSETS_DIRECTORY"/monet-alpha-square.png", &w, &h, &nrChannel, STBI_rgb_alpha);

        std::vector<DescriptorPool::DescriptorQuantity> sizes = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}
        };
        this->_descriptorPool.init(this->_device, 10, sizes);

        // Create feedback image that gets copied to each frame
        // this->_feedbackImage.imageExtent = VkExtent3D{.width = (unsigned int)w, .height = (unsigned int)h, .depth = 1};
        this->_feedbackImage.imageExtent = _drawImage.imageExtent;
        // this->_feedbackImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        this->_feedbackImage.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo imgCreateInfo = vkinit::image_create_info(
            this->_feedbackImage.imageFormat,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            this->_drawImage.imageExtent
        );

        VmaAllocationCreateInfo imgAllocInfo = {
            .usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        };
        VK_CHECK(vmaCreateImage(this->_allocator, &imgCreateInfo, &imgAllocInfo, &this->_feedbackImage.image, &this->_feedbackImage.allocation, nullptr));
        VkImageViewCreateInfo viewCreateInfo = vkinit::imageview_create_info(this->_feedbackImage.imageFormat, this->_feedbackImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
        VK_CHECK(vkCreateImageView(this->_device, &viewCreateInfo, nullptr, &this->_feedbackImage.imageView));
        this->_mainDeletionQueue.push([&]() {
            vmaDestroyImage(this->_allocator, this->_feedbackImage.image, this->_feedbackImage.allocation);
            vkDestroyImageView(this->_device, this->_feedbackImage.imageView, nullptr);
        });
        
        // Define descriptor layout
        {
            DescriptorLayoutBuilder builder;
            builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            this->_drawImageDescriptorLayout = builder.build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        }
        this->_drawImageDescriptorSet = this->_descriptorPool.allocateSet(this->_device, this->_drawImageDescriptorLayout);

        VkSamplerCreateInfo samplerCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        };
        VK_ASSERT(vkCreateSampler(this->_device, &samplerCreateInfo, nullptr, &this->_simpleSampler));
        this->_descriptorWriter.clear();
        this->_descriptorWriter.write_image(0, this->_drawImage.imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        this->_descriptorWriter.write_image(1, this->_feedbackImage.imageView, this->_simpleSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        this->_descriptorWriter.update_set(this->_device, this->_drawImageDescriptorSet);
        this->_mainDeletionQueue.push([&]() {
            vkDestroySampler(this->_device, this->_simpleSampler, nullptr);
            this->_descriptorPool.destroy(this->_device);
            vkDestroyDescriptorSetLayout(this->_device, this->_drawImageDescriptorLayout, nullptr);
        });
        
        
        // Create staging buffer to allow for CPU -> GPU image copies
        this->_stagingBuffer = createBuffer(w*h*nrChannel,
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VMA_MEMORY_USAGE_CPU_TO_GPU);
        this->_mainDeletionQueue.push([&]() { vmaDestroyBuffer(this->_allocator, this->_stagingBuffer.buffer, this->_stagingBuffer.allocation); });

        std::memcpy(this->_stagingBuffer.info.pMappedData, data, w*h*nrChannel);

        this->immediateSubmit([&](VkCommandBuffer cmd) {
            vkutil::transition_image(cmd, this->_feedbackImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copyRegion = {};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;
            copyRegion.bufferImageHeight = 0;

            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel = 0;
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageExtent = this->_feedbackImage.imageExtent;

            vkCmdCopyBufferToImage(cmd, this->_stagingBuffer.buffer, this->_feedbackImage.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

            vkutil::transition_image(cmd, this->_feedbackImage.image, 
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        return true;
    }

    bool initDescriptors()
    {
        // Create 10 sets, each containing two image binding
        std::vector<DescriptorPool::DescriptorQuantity> sizes = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}
        };
        this->_descriptorPool.init(this->_device, 10, sizes);
        
        // make the layout describing the bindings corresponding to the set
        {
            DescriptorLayoutBuilder builder;
            builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            this->_drawImageDescriptorLayout = builder.build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        }

        // allocate the descriptor set from the layout
        this->_drawImageDescriptorSet = _descriptorPool.allocateSet(this->_device, this->_drawImageDescriptorLayout);

        VkDescriptorImageInfo imgInfo = {
            .imageView = this->_drawImage.imageView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };

        VkWriteDescriptorSet drawImageWrite = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = this->_drawImageDescriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imgInfo
        };

        vkUpdateDescriptorSets(this->_device, 1, &drawImageWrite, 0, nullptr);

        this->_mainDeletionQueue.push([&]() {
            this->_descriptorPool.destroy(this->_device);
            vkDestroyDescriptorSetLayout(this->_device, this->_drawImageDescriptorLayout, nullptr);
        });
        return true;
    }

    bool initBackgroundPipelines()
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
        VK_ASSERT(vkCreatePipelineLayout(this->_device, &computeLayout, nullptr, &this->_gradientPipelineLayout));

        VkShaderModule computeDrawShader;
        const char* shaderFileName = SHADER_DIRECTORY"/random.comp.spv";
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
            .layout = this->_gradientPipelineLayout
        };

        VK_ASSERT(vkCreateComputePipelines(this->_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &this->_gradientPipeline));

        vkDestroyShaderModule(this->_device, computeDrawShader, nullptr);
        this->_mainDeletionQueue.push([&]() {
            vkDestroyPipelineLayout(this->_device, this->_gradientPipelineLayout, nullptr);
            vkDestroyPipeline(this->_device, this->_gradientPipeline, nullptr);
        });
        return true;
    }

    bool initPipelines()
    {
        if(!initBackgroundPipelines()) {
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
    DeletionQueue _mainDeletionQueue;

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

    VkPipeline _gradientPipeline;
    VkPipelineLayout _gradientPipelineLayout;

    DescriptorPool _descriptorPool;
    VkDescriptorSet _drawImageDescriptorSet;
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
    AllocatedImage _feedbackImage {};
    AllocatedBuffer _stagingBuffer {};
    VkSampler _simpleSampler {};

};

void
signalHandler(int sig)
{
    std::cout << "sig" << std::endl;
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, &signalHandler);
    Renderer renderer("VulkanFlow", 600, 600);
    if(!renderer.Init()) {
        std::cout << "[ERROR] Failed to initialize renderer" << std::endl;
        return EXIT_FAILURE;
    }

    const double startTime = GetTimestamp();
    while(!renderer.ShouldClose()) {
        const double dt = GetTimestamp() - startTime;
        renderer.Render(dt);
        // std::this_thread::sleep_for(std::chrono::duration(std::chrono::milliseconds(16)));
    }
    std::cout << "Close" << std::endl;
    return EXIT_SUCCESS;
}
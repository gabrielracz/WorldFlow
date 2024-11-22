#include <SDL2/SDL.h>
#include <SDL_vulkan.h>

#include <chrono>
#include <iomanip>
#include <stdexcept>
#include <system_error>
#include <vulkan/vulkan.h>
#include <VkBootstrap.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <stb_image.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>

#include "path_config.hpp"
#include "utils.hpp"

#include "shared/vk_types.h"
#include "shared/vk_images.h"
#include "shared/vk_initializers.h"
#include "shared/vk_descriptors.h"
#include "shared/vk_pipelines.h"
#include "shared/vk_loader.h"

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
constexpr bool VSYNCEnabled = true;
constexpr uint32_t FPSMeasurePeriod = 60;
constexpr uint32_t FrameOverlap = 2;
constexpr uint64_t TimeoutNs = 1000000000;
constexpr uint32_t MaxDescriptorSets = 10;
constexpr uint32_t DiffusionIterations = 11;
constexpr uint32_t PressureIterations = 11;
constexpr uint64_t StagingBufferSize = 1024ul * 1024ul * 8ul;
}
//should be odd to ensure consistency of final result buffer index
static_assert(Constants::DiffusionIterations % 2 == 1); 
static_assert(Constants::PressureIterations % 2 == 1);

/* TODO:
- Make image class to handle transitions statefully

*/

// LIFO stack
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
    VkPipeline pipeline {};
    VkPipelineLayout layout {};
    std::vector<VkDescriptorSet> descriptorSets {};
    VkDescriptorSetLayout descriptorLayout {};
};

struct TrianglePipeline
{
    VkPipeline pipeline {};
    VkPipelineLayout layout {};
};

struct ComputePushConstants
{
    float time {};
    float dt {};
};

// push constants for our mesh object draws
struct GraphicsPushConstants
{
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
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

        for(GPUMeshBuffers& mesh : this->_testMeshes) {
            destroyBuffer(mesh.vertexBuffer);
            destroyBuffer(mesh.indexBuffer);
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
                                initBuffers() &&
                                initAssets() &&
                                initDescriptors() &&
                                initPipelines()
                                ;
        return this->_isInitialized;
    }

    void Render(double dt)
    {
        this->_elapsed += dt;
        updatePerformanceCounters(dt);
        pollEvents();
        draw(dt);
        this->_frameNumber++;
    }
    
    bool ShouldClose() const { return this->_shouldClose; }
    void Close() { this->_shouldClose = true; }

private:
    void updatePerformanceCounters(double dt)
    {
        if(this->_frameNumber > 0 && this->_frameNumber % Constants::FPSMeasurePeriod == 0) {
            const double delta = (this->_elapsed - this->_lastFpsMeasurementTime);
            const double averageFrameTime =  delta / Constants::FPSMeasurePeriod;
            const double fps = Constants::FPSMeasurePeriod / delta;
            std::cout << std::fixed << std::setprecision(3) << "FPS: " << fps  << std::setprecision(5) << "  (" << averageFrameTime << ")" << std::endl;
            this->_lastFpsMeasurementTime = this->_elapsed;
        }
        
    }

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

    void clearImage(VkCommandBuffer cmd, AllocatedImage& img, VkClearColorValue color = {0.0, 0.0, 0.0, 0.0})
    {
        VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &clearRange);
    }

    void drawBackground(VkCommandBuffer cmd, double dt)
    {

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_drawImagePipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_drawImagePipeline.layout, 0, 1, &this->_drawImagePipeline.descriptorSets[0], 0, nullptr);
        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed),
            .dt   = static_cast<float>(dt)
        };
        vkCmdPushConstants(cmd, this->_drawImagePipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);
        const VkExtent3D wgCount = getWorkgroupCounts();
        vkCmdDispatch(cmd, wgCount.width, wgCount.height, wgCount.depth);
    }

    bool addSources(VkCommandBuffer cmd, double dt)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_addSourcePipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_addSourcePipeline.layout, 0, 1, &this->_addSourcePipeline.descriptorSets[0], 0, nullptr);
        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed),
            .dt   = static_cast<float>(dt)
        };
        vkCmdPushConstants(cmd, this->_addSourcePipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);

        const VkExtent3D wgCount = getWorkgroupCounts();
        vkCmdDispatch(cmd, wgCount.width, wgCount.height, wgCount.depth);
        return true;
    }

    void pingPongDispatch(VkCommandBuffer cmd, ComputePipeline& pipeline, AllocatedImage* images, ComputePushConstants& pc, uint32_t iterations)
    {
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

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
        for(int i = 0; i < iterations; i++) {
            int pingPongDescriptorIndex = i % 2;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &pipeline.descriptorSets[pingPongDescriptorIndex], 0, nullptr);
            vkCmdPushConstants(cmd, this->_diffusionPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

            const VkExtent3D wgCount = getWorkgroupCounts();
            vkCmdDispatch(cmd, wgCount.width, wgCount.height, wgCount.depth);
            
            // wait for all image writes to be complete
            imageBarrier.image = images[pingPongDescriptorIndex].image;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &imageBarrier);
        }

    }

    void diffuseDensity(VkCommandBuffer cmd, double dt)
    {
        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed),
            .dt   = static_cast<float>(dt)
        };
        pingPongDispatch(cmd, this->_diffusionPipeline, this->_densityImages, constants, Constants::DiffusionIterations);
        vkutil::transition_image(cmd, this->_densityImages[0].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkutil::transition_image(cmd, this->_densityImages[2].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkutil::copy_image_to_image(cmd, this->_densityImages[0].image, this->_densityImages[2].image, this->_densityImages[0].imageExtent, this->_densityImages[2].imageExtent);
        // transition to present format
        vkutil::transition_image(cmd, this->_densityImages[2].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(cmd, this->_densityImages[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }

    void advectDensity(VkCommandBuffer cmd, double dt)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_advectionPipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_advectionPipeline.layout, 0, 1, &this->_advectionPipeline.descriptorSets[0], 0, nullptr);
        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed),
            .dt   = static_cast<float>(dt)
        };
        vkCmdPushConstants(cmd, this->_advectionPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);
        const VkExtent3D wgCount = getWorkgroupCounts();
        vkCmdDispatch(cmd, wgCount.width, wgCount.height, wgCount.depth);
    }

    void diffuseVelocity()
    {

    }

    void projectIncompressible(VkCommandBuffer cmd, double dt)
    {
        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed),
            .dt   = static_cast<float>(dt)
        };

        // Divergence
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_divergencePipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_divergencePipeline.layout, 0, 1, &this->_divergencePipeline.descriptorSets[0], 0, nullptr);
        vkCmdPushConstants(cmd, this->_divergencePipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);
        const VkExtent3D wgCount = getWorkgroupCounts();
        vkCmdDispatch(cmd, wgCount.width, wgCount.height, wgCount.depth);

        //Pressure
        pingPongDispatch(cmd, this->_pressurePipeline, this->_densityImages, constants, Constants::PressureIterations);
    }

    void advectVelocity()
    {

    }

    void visualizeFluid(VkCommandBuffer cmd, double dt)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_visualizationPipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_visualizationPipeline.layout, 0, 1, &this->_visualizationPipeline.descriptorSets[0], 0, nullptr);
        ComputePushConstants constants = {
            .time = static_cast<float>(this->_elapsed),
            .dt   = static_cast<float>(dt)
        };
        vkCmdPushConstants(cmd, this->_visualizationPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &constants);
        const VkExtent3D wgCount = getWorkgroupCounts();
        vkCmdDispatch(cmd, wgCount.width, wgCount.height, wgCount.depth);
    }

    VkExtent3D getWorkgroupCounts()
    {
        return VkExtent3D {
            .width = static_cast<uint32_t>(std::ceil(this->_drawImage.imageExtent.width/16.0)),
            .height = static_cast<uint32_t>(std::ceil(this->_drawImage.imageExtent.height/16.0)),
            .depth = 1
        };
    }

    void drawFluid(VkCommandBuffer cmd, double dt)
    {
        if(this->_frameNumber < 1)
            addSources(cmd, dt);

        diffuseDensity(cmd, dt);
        advectDensity(cmd, dt);

        // diffuseVelocity
        // projectIncompressible(cmd, dt);
        // advectVelocity
        // projectVelocityIncompressible

        visualizeFluid(cmd, dt);
    }

    void drawGeometry(VkCommandBuffer cmd, double dt)
    {
        VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingInfo renderInfo = vkinit::rendering_info(this->_windowExtent, &colorAttachmentInfo, nullptr);
        vkCmdBeginRendering(cmd, &renderInfo);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_meshPipeline.pipeline);

        VkViewport viewport = {
            .x = 0,
            .y = 0,
            .width = (float)this->_windowExtent.width,
            .height = (float)this->_windowExtent.height,
            .minDepth = 0.0,
            .maxDepth = 1.0
        };

        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {
            .offset = { .x = 0, .y = 0 },
            .extent = { .width = this->_windowExtent.width, .height = this->_windowExtent.height }
        };

        vkCmdSetScissor(cmd, 0, 1, &scissor);

        glm::mat4 view = glm::translate(glm::vec3{ 0,0,-5 });
        // camera projection
        glm::mat4 projection = glm::perspective(glm::radians(70.f), (float)this->_windowExtent.width / (float)_windowExtent.height, 10000.f, 0.1f);

        // invert the Y direction on projection matrix so that we are more similar
        // to opengl and gltf axis
        projection[1][1] *= -1;
        GraphicsPushConstants pc = {
            .worldMatrix = projection * view,
            .vertexBuffer = this->_testMeshes[2].vertexBufferAddress
        };
        vkCmdPushConstants(cmd, this->_meshPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pc);
        vkCmdBindIndexBuffer(cmd, this->_rectangleMesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);

        vkCmdBindIndexBuffer(cmd, this->_testMeshes[2].indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, this->_testMeshes[2].numIndices, 1, 0, 0, 0);
        vkCmdEndRendering(cmd);
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

        drawFluid(cmd, dt);

        vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        drawGeometry(cmd, dt);

        // prepare drawImage to swapchainImage copy
        AllocatedImage& drawImage = this->_drawImage;
        VkImage& swapchainImage = this->_swapchainImages[swapchainImageIndex];

        vkutil::transition_image(cmd, drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkutil::copy_image_to_image(cmd, drawImage.image, swapchainImage, drawImage.imageExtent, VkExtent3D{.width = this->_swapchainExtent.width, .height = this->_swapchainExtent.height, .depth = 1});

        // transition to present format
        vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkutil::transition_image(cmd, drawImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

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
            .set_desired_present_mode(Constants::VSYNCEnabled ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
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
        // if(!createDrawImage()) {
        //     std::cout << "[ERROR] Failed to create draw image" << std::endl;
        //     return false;
        // }
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

    void destroyBuffer(AllocatedBuffer buf)
    {
        vmaDestroyBuffer(this->_allocator, buf.buffer, buf.allocation);
    }

    AllocatedBuffer createBuffer(uint64_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, bool autoCleanup = true)
    {
        // allocate buffer
        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = allocSize,
            .usage = usage
        };
        VmaAllocationCreateInfo vmaallocInfo = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT, // ignored if not host-visible
            .usage = memoryUsage,
        };

        AllocatedBuffer newBuffer;
        VK_CHECK(vmaCreateBuffer(this->_allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info));
        if(autoCleanup) {
            this->_deletionQueue.push([this, newBuffer]() {
                // destroyBuffer(newBuffer);
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

    GPUMeshBuffers uploadMesh(std::span<Vertex> vertices, std::span<uint32_t> indices)
    {
        // Initialize GPU buffers to store mesh data (vertex attributes + triangle indices)
        const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
        const size_t indexBufferSize = indices.size() * sizeof(uint32_t);
        GPUMeshBuffers mesh;
        mesh.vertexBuffer = createBuffer(
            vertexBufferSize, 
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            false
        );
        VkBufferDeviceAddressInfo deviceAddressInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = mesh.vertexBuffer.buffer
        };
        mesh.vertexBufferAddress = vkGetBufferDeviceAddress(this->_device, &deviceAddressInfo);

        mesh.indexBuffer = createBuffer(
            indexBufferSize, 
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            false
        );
        
        // Copy mesh data to cpu staging buffer (host visible and memory coherent)
        AllocatedBuffer stagingBuffer = createBuffer(
            vertexBufferSize + indexBufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_CPU_ONLY,
            false
        );
        void* stagingData = stagingBuffer.allocation->GetMappedData();
        
        std::memcpy(stagingData, vertices.data(), vertexBufferSize);
        std::memcpy((char *)stagingData + vertexBufferSize, indices.data(), indexBufferSize);

        // Copy mesh data from CPU to GPU
        immediateSubmit([&](VkCommandBuffer cmd) {
                VkBufferCopy vertexCopy = {
                    .srcOffset = 0,
                    .dstOffset = 0,
                    .size = vertexBufferSize
                };
                vkCmdCopyBuffer(cmd, stagingBuffer.buffer, mesh.vertexBuffer.buffer, 1, &vertexCopy);

                VkBufferCopy indexCopy = {
                    .srcOffset = vertexBufferSize,
                    .dstOffset = 0,
                    .size = indexBufferSize
                };
                vkCmdCopyBuffer(cmd, stagingBuffer.buffer, mesh.indexBuffer.buffer, 1, &indexCopy);
        });
        destroyBuffer(stagingBuffer);
        mesh.numIndices = indices.size();
        mesh.numVertices = vertices.size();
        return mesh;
    }

    bool initAssets()
    {
        int w, h, nrChannel;
        float* data = stbi_loadf(ASSETS_DIRECTORY"/monet-alpha-square.png", &w, &h, &nrChannel, STBI_rgb_alpha);
        VkExtent3D dataExtent {.width = (unsigned int)w, .height = (unsigned int)h, .depth = 1};
        this->_stagingBuffer = createBuffer( // allows for CPU -> GPU copies
            Constants::StagingBufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        // std::memcpy(this->_stagingBuffer.info.pMappedData, data, w*h*nrChannel*sizeof(float));
        int imgLen = w*h*nrChannel;
        for(int i = 0; i < 600*600*4; i += 4) {
            ((float*)this->_stagingBuffer.info.pMappedData)[i] = 0.0;
            ((float*)this->_stagingBuffer.info.pMappedData)[i+1] = 0.0;
            ((float*)this->_stagingBuffer.info.pMappedData)[i+2] = 0.0;
            ((float*)this->_stagingBuffer.info.pMappedData)[i+3] = 0.0;
        }

        for(AllocatedImage& img : this->_densityImages) {
            img = createImage(
                dataExtent,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,
                // VK_FORMAT_R8G8B8A8_UNORM
                VK_FORMAT_R32G32B32A32_SFLOAT
            );
        }
        copyBufferToImage(this->_stagingBuffer, this->_densityImages[0]);
        copyBufferToImage(this->_stagingBuffer, this->_densityImages[2]);

        for(AllocatedImage& img : this->_velocityImages) {
            img = createImage(
                dataExtent,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_FORMAT_R32G32B32A32_SFLOAT
            );
        }

        this->_drawImage = createImage(
            dataExtent,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FORMAT_R32G32B32A32_SFLOAT
        );

        immediateSubmit([&](VkCommandBuffer cmd) {
            for(AllocatedImage& img : this->_densityImages)
                vkutil::transition_image(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            for(AllocatedImage& img : this->_velocityImages)
                vkutil::transition_image(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        });

        std::array<Vertex,4> rect_vertices;

        rect_vertices[0].position = {0.5,-0.5, 0};
        rect_vertices[1].position = {0.5,0.5, 0};
        rect_vertices[2].position = {-0.5,-0.5, 0};
        rect_vertices[3].position = {-0.5,0.5, 0};

        rect_vertices[0].color = {0,0, 0,1};
        rect_vertices[1].color = { 0.5,0.5,0.5 ,1};
        rect_vertices[2].color = { 1,0, 0,1 };
        rect_vertices[3].color = { 0,1, 0,1 };

        std::array<uint32_t,6> rect_indices;

        rect_indices[0] = 0;
        rect_indices[1] = 1;
        rect_indices[2] = 2;

        rect_indices[3] = 2;
        rect_indices[4] = 1;
        rect_indices[5] = 3;

        this->_rectangleMesh = uploadMesh(rect_vertices, rect_indices);
        this->_deletionQueue.push([&]() {
            destroyBuffer(this->_rectangleMesh.indexBuffer);
            destroyBuffer(this->_rectangleMesh.vertexBuffer);
        });

        std::vector<std::vector<Vertex>> vertexBuffers;
        std::vector<std::vector<uint32_t>> indexBuffers;
        if(!loadGltfMeshes({ASSETS_DIRECTORY"/basicmesh.glb"}, vertexBuffers, indexBuffers)) {
            std::cout << "[ERROR] Failed to load meshes" << std::endl;
        }
        
        // upload the meshes
        for(int m = 0; m < vertexBuffers.size(); m++) {
            this->_testMeshes.emplace_back(uploadMesh(vertexBuffers[m], indexBuffers[m]));
        }

        return true;
    }

    bool initDescriptors()
    {
        std::vector<DescriptorPool::DescriptorQuantity> sizes = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}
        };
        this->_descriptorPool.init(this->_device, 10, sizes);

        VkSamplerCreateInfo samplerCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        };
        VK_ASSERT(vkCreateSampler(this->_device, &samplerCreateInfo, nullptr, &this->_simpleSampler));
        this->_deletionQueue.push([&]() { vkDestroySampler(this->_device, this->_simpleSampler, nullptr); });

        // Draw Image
        this->_drawImagePipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_drawImagePipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_drawImagePipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_image(0, this->_drawImage.imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_drawImagePipeline.descriptorSets[0]);

        // Visualization
        this->_visualizationPipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_visualizationPipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_visualizationPipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_image(0, this->_densityImages[2].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_velocityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(2, this->_drawImage.imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_visualizationPipeline.descriptorSets[0]);

        // Add Source
        this->_addSourcePipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_addSourcePipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_addSourcePipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_image(0, this->_densityImages[2].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_velocityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_addSourcePipeline.descriptorSets[0]);

        // Diffusion
        this->_diffusionPipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_diffusionPipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_diffusionPipeline.descriptorLayout));
        this->_diffusionPipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_diffusionPipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_image(0, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_densityImages[1].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(2, this->_densityImages[2].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_diffusionPipeline.descriptorSets[0])
            .clear()
            .write_image(0, this->_densityImages[1].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(2, this->_densityImages[2].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_diffusionPipeline.descriptorSets[1]);

        // Advection
        this->_advectionPipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_advectionPipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_advectionPipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_image(0, this->_densityImages[1].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_velocityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(2, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_advectionPipeline.descriptorSets[0]);

        // Divergence (writes into alpha channel of density)
        this->_divergencePipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_divergencePipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_divergencePipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_image(0, this->_velocityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_divergencePipeline.descriptorSets[0]);
        
        // Pressure (writes into green channel of density)
        this->_pressurePipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_pressurePipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_pressurePipeline.descriptorLayout));
        this->_pressurePipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_pressurePipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_image(0, this->_velocityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(2, this->_densityImages[1].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_pressurePipeline.descriptorSets[0])
            .clear()
            .write_image(0, this->_velocityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(1, this->_densityImages[1].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .write_image(2, this->_densityImages[0].imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_pressurePipeline.descriptorSets[1]);
        

        this->_deletionQueue.push([&]() {
            this->_descriptorPool.destroy(this->_device);
            vkDestroyDescriptorSetLayout(this->_device, _drawImagePipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, _diffusionPipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, _addSourcePipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, _advectionPipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, _divergencePipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, _pressurePipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, _visualizationPipeline.descriptorLayout, nullptr);
        });
        return true;
    }

    bool createComputePipeline(const std::string& shaderFilename, ComputePipeline& newPipeline, bool autoCleanup = true)
    {
        const VkPushConstantRange pushConstants = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(ComputePushConstants),
        };

        VkPipelineLayoutCreateInfo pipelineLayout = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &newPipeline.descriptorLayout,

            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstants
        };

        VK_CHECK(vkCreatePipelineLayout(this->_device, &pipelineLayout, nullptr, &newPipeline.layout));

        VkShaderModule computeDrawShader;
        if(!vkutil::load_shader_module(shaderFilename, this->_device, &computeDrawShader)) {
            std::cerr << "[ERROR] Failed to load compute shader " << shaderFilename << std::endl;
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
            .layout = newPipeline.layout
        };

        VK_CHECK(vkCreateComputePipelines(this->_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &newPipeline.pipeline));

        if(autoCleanup) {
            vkDestroyShaderModule(this->_device, computeDrawShader, nullptr);
            this->_deletionQueue.push([this, newPipeline]() {
                vkDestroyPipelineLayout(this->_device, newPipeline.layout, nullptr);
                vkDestroyPipeline(this->_device, newPipeline.pipeline, nullptr);
            });
        }
        return true;
    }

    bool initComputePipelines()
    {
        //TODO: these need pipeline.descriptorLayout set previously to work
        return createComputePipeline(SHADER_DIRECTORY"/diffusion.comp.spv", this->_diffusionPipeline) &&
               createComputePipeline(SHADER_DIRECTORY"/addSources.comp.spv", this->_addSourcePipeline) &&
               createComputePipeline(SHADER_DIRECTORY"/advection.comp.spv", this->_advectionPipeline) &&
               createComputePipeline(SHADER_DIRECTORY"/divergence.comp.spv", this->_divergencePipeline) &&
               createComputePipeline(SHADER_DIRECTORY"/pressure.comp.spv", this->_pressurePipeline) &&
               createComputePipeline(SHADER_DIRECTORY"/visualization.comp.spv", this->_visualizationPipeline);
    }

    bool initGraphicsPipelines()
    {

        const VkPushConstantRange pushConstantsRange = {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(GraphicsPushConstants),
        };

        // No descriptors
        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantsRange
        };
        VK_CHECK(vkCreatePipelineLayout(this->_device, &pipelineLayoutInfo, nullptr, &this->_meshPipeline.layout));

        VkShaderModule fragShader;
        if(!vkutil::load_shader_module(SHADER_DIRECTORY"/mesh.frag.spv", this->_device, &fragShader)) {
            std::cout << "[ERROR] Failed to load fragment shader" << std::endl;
            return false;
        }

        VkShaderModule vertShader;
        if(!vkutil::load_shader_module(SHADER_DIRECTORY"/mesh.vert.spv", this->_device, &vertShader)) {
            std::cout << "[ERROR] Failed to load vertex shader" << std::endl;
            return false;
        }

        PipelineBuilder builder;
        builder._pipelineLayout = this->_meshPipeline.layout;
        this->_meshPipeline.pipeline = builder
            .set_shaders(vertShader, fragShader)
            .set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .set_polygon_mode(VK_POLYGON_MODE_FILL)
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE)
            .set_multisampling_none()
            .disable_blending()
            .disable_depthtest()
            .set_color_attachment_format(this->_drawImage.imageFormat)
            .set_depth_format(VK_FORMAT_UNDEFINED)
            .build_pipeline(this->_device);

        vkDestroyShaderModule(this->_device, fragShader, nullptr);
        vkDestroyShaderModule(this->_device, vertShader, nullptr);

        this->_deletionQueue.push([this]() {
            vkDestroyPipelineLayout(this->_device, this->_meshPipeline.layout, nullptr);
            vkDestroyPipeline(this->_device, this->_meshPipeline.pipeline, nullptr);
        });

        return true;
    }

    bool initPipelines()
    {
        if(!initComputePipelines()) {
            std::cout << "[ERROR] Failed to init compute pipelines" << std::endl;
            return false;
        }

        if(!initGraphicsPipelines()) {
            std::cout << "[ERROR] Failed to init triangle pipelines" << std::endl;
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
    uint32_t _frameNumber = 0;

    DescriptorWriter _descriptorWriter {};
    DescriptorPool _descriptorPool;

    // Compute Shaders
    ComputePipeline _drawImagePipeline;
    ComputePipeline _addSourcePipeline;
    ComputePipeline _diffusionPipeline;
    ComputePipeline _advectionPipeline;
    ComputePipeline _divergencePipeline;
    ComputePipeline _pressurePipeline;
    ComputePipeline _projectionPipeline;
    ComputePipeline _visualizationPipeline;

    // Triangle Rasterization 
    TrianglePipeline _meshPipeline;

    // Shader Resources
    AllocatedImage _drawImage;
    AllocatedImage _densityImages[3] {};
    AllocatedImage _velocityImages[2] {};
    AllocatedBuffer _stagingBuffer {};
    VkSampler _simpleSampler {};

    // Meshes
    GPUMeshBuffers _rectangleMesh;
    std::vector<GPUMeshBuffers> _testMeshes;

    //immediate submit structures
    VkFence _immFence;
    VkCommandBuffer _immCommandBuffer;
    VkCommandPool _immCommandPool;

    std::string _name {};
    std::atomic<bool> _shouldClose {false};
    double _elapsed {};
    double _lastFpsMeasurementTime {};


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
    }

    std::cout << "Closed" << std::endl;
    return EXIT_SUCCESS;

}
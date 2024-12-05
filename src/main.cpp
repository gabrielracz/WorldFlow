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

#define GLM_FORCE_RADIANS
#include <stb_image.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/string_cast.hpp>

#include "path_config.hpp"
#include "defines.hpp"
#include "camera.hpp"
#include "transform.hpp"

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
#include <unordered_map>

namespace Constants
{
// #ifdef NDEBUG
// constexpr bool IsValidationLayersEnabled = false;
// #else
// constexpr bool IsValidationLayersEnabled = true;
// #endif
constexpr bool IsValidationLayersEnabled = true;
constexpr bool VSYNCEnabled = true;

constexpr uint32_t FPSMeasurePeriod = 60;
constexpr uint32_t FrameOverlap = 2;
constexpr uint64_t TimeoutNs = 1000000000;
constexpr uint32_t MaxDescriptorSets = 10;
constexpr uint32_t DiffusionIterations = 11;
constexpr uint32_t PressureIterations = 11;
constexpr uint64_t StagingBufferSize = 1024ul * 1024ul * 8ul;
constexpr VkExtent3D DrawImageResolution {2560, 1440, 1};

constexpr size_t VoxelGridResolution = 512 + 256;
constexpr size_t VoxelGridSize = VoxelGridResolution * VoxelGridResolution * VoxelGridResolution * sizeof(float);
constexpr float VoxelGridScale = 3.0f;

constexpr uint32_t MeshIdx = 0;
constexpr float MeshScale = 0.01;
// constexpr float MeshScale = 0.60;

// constexpr glm::vec3 CameraPosition = glm::vec3(0.1, -0.15, -0.1);
constexpr glm::vec3 CameraPosition = glm::vec3(0.0, 0.0, -3.0);
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

struct GraphicsPipeline
{
    VkPipeline pipeline {};
    VkPipelineLayout layout {};
    std::vector<VkDescriptorSet> descriptorSets {};
    VkDescriptorSetLayout descriptorLayout {};
};

struct VoxelizerPushConstants
{
    glm::uvec3 gridSize;
    float gridScale;
    float time;
};

struct VoxelInfo
{
    glm::vec3 gridDimensions;
    float gridScale;
};

struct VoxelFragmentCounter
{
    uint32_t fragCount;
};

struct RayTracerPushConstants
{
    glm::mat4 inverseProjection;  // Inverse projection matrix
    glm::mat4 inverseView;        // Inverse view matrix
    glm::vec3 cameraPos;         // Camera position in world space
    float nearPlane;        // Near plane distance
    glm::vec2 screenSize;        // Width and height of output image
    float maxDistance;      // Maximum ray travel distance
    float stepSize;         // Base color accumulation per step
    glm::vec3 gridSize;          // Size of the voxel grid in each dimension
    float gridScale;          // Size of the voxel grid in each dimension
    glm::vec4 lightSource;
    glm::vec4 baseColor;
};

// push constants for our mesh object draws
struct GraphicsPushConstants
{
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
    uint32_t padding[2];
};

// struct Camera
// {
//     glm::vec3 pos;
//     glm::mat4 view;
//     glm::mat4 projection;
// };

typedef std::unordered_map<int, bool> MouseMap;
struct Mouse 
{
    bool first_captured = true;
    bool captured = true;
    glm::vec2 prev;
    glm::vec2 move;
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
                                initPipelines() &&
                                initCamera()
                                // preProcess()
                                ;
        return this->_isInitialized;
    }

    void Render(double dt)
    {
        this->_elapsed += dt;
        updatePerformanceCounters(dt);
        pollEvents();
        if(this->_resizeRequested) {
            resizeSwapchain();
            initCamera();
            this->_mouse.first_captured = true;
        }
        // this->_camera.OrbitYaw(glm::radians(30.0) * dt);
        this->_camera.Update();
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
            
            else if(event.type == SDL_MOUSEBUTTONDOWN) {
                this->_mouseButtons[event.button.button] = true;
            } else if(event.type == SDL_MOUSEBUTTONUP) {
                this->_mouseButtons[event.button.button] = false;
            }
            
            else if(event.type == SDL_MOUSEMOTION) {
                Mouse& mouse = this->_mouse;
                if (mouse.first_captured) {
                    mouse.prev = {event.motion.x, event.motion.y};
                    mouse.first_captured = false;
                }
                mouse.move = glm::vec2(event.motion.x, event.motion.y) - mouse.prev;
                mouse.prev = {event.motion.x, event.motion.y};

                float mouse_sens = -0.003f;
                glm::vec2 look = mouse.move * mouse_sens;
                if(this->_mouseButtons[SDL_BUTTON_LEFT]) {
                    this->_camera.OrbitYaw(-look.x);
                    this->_camera.OrbitPitch(look.y);
                }
            }

            else if(event.type == SDL_MOUSEWHEEL) {
                float delta = -event.wheel.preciseY * 0.1f;
                this->_camera.distance += delta;
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
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &clearRange);
    }

    VkExtent3D getWorkgroupCounts(uint32_t localGroupSize = 16)
    {
        return VkExtent3D {
            .width = static_cast<uint32_t>(std::ceil(this->_drawImage.imageExtent.width/(float)localGroupSize)),
            .height = static_cast<uint32_t>(std::ceil(this->_drawImage.imageExtent.height/(float)localGroupSize)),
            .depth = 1
        };
    }

    void drawGeometry(VkCommandBuffer cmd, double dt)
    {
        VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingInfo renderInfo = vkinit::rendering_info(VkExtent2D{this->_windowExtent.width, this->_windowExtent.height}, &colorAttachmentInfo, nullptr);
        vkCmdBeginRendering(cmd, &renderInfo);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_meshPipeline.pipeline);

        VkViewport viewport = {
            .x = 0,
            .y = (float)this->_windowExtent.height,
            .width = (float)this->_windowExtent.width,
            .height = -(float)this->_windowExtent.height,
            .minDepth = 0.0,
            .maxDepth = 1.0
        };

        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {
            .offset = { .x = 0, .y = 0 },
            .extent = { .width = this->_windowExtent.width, .height = this->_windowExtent.height }
        };

        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // invert the Y direction on projection matrix so that we are more similar
        // to opengl and gltf axis
        GraphicsPushConstants pc = {
            // .worldMatrix = this->_camera.projection * this->_camera.view,
            .worldMatrix = this->_camera.GetProjectionMatrix() * this->_camera.GetViewMatrix() * glm::scale(glm::vec3(Constants::MeshScale)),
            .vertexBuffer = this->_testMeshes[Constants::MeshIdx].vertexBufferAddress
        };

        vkCmdPushConstants(cmd, this->_meshPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pc);
        vkCmdBindIndexBuffer(cmd, this->_testMeshes[Constants::MeshIdx].indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, this->_testMeshes[Constants::MeshIdx].numIndices, 1, 0, 0, 0);
        vkCmdEndRendering(cmd);
    }

    void voxelRasterizeGeometry(VkCommandBuffer cmd)
    {
        // Zero the counter
        // std::memset(this->_voxelFragmentCounter.allocation->GetMappedData(), 0, sizeof(VoxelFragmentCounter));
        vkCmdFillBuffer(cmd, this->_voxelFragmentCounter.buffer, 0, VK_WHOLE_SIZE, 0);
        // Synchronization barrier
        VkMemoryBarrier2 memBarrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        };

        VkDependencyInfo dependencyInfo = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &memBarrier
        };

        vkCmdPipelineBarrier2(cmd, &dependencyInfo);

        vkutil::transition_image(cmd, this->_voxelImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_voxelImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingInfo renderInfo = vkinit::rendering_info(VkExtent2D{Constants::VoxelGridResolution, Constants::VoxelGridResolution}, &colorAttachmentInfo, nullptr);
        vkCmdBeginRendering(cmd, &renderInfo);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_voxelRasterPipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_voxelRasterPipeline.layout, 0, 1, this->_voxelRasterPipeline.descriptorSets.data(), 0, nullptr);

        VkViewport viewport = {
            .x = 0,
            .y = (float)Constants::VoxelGridResolution,
            .width = (float)Constants::VoxelGridResolution,
            .height = -(float)Constants::VoxelGridResolution,
            .minDepth = 0.0,
            .maxDepth = 1.0
        };
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {
            .offset = { .x = 0, .y = 0 },
            .extent = { .width = Constants::VoxelGridResolution, .height = Constants::VoxelGridResolution}
        };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // invert the Y direction on projection matrix so that we are more similar
        // to opengl and gltf axis
        GraphicsPushConstants pc = {
            // .worldMatrix = glm::mat4(1.0),
            .worldMatrix = glm::mat4(1.0) * glm::scale(glm::vec3(Constants::MeshScale)),
            .vertexBuffer = this->_testMeshes[Constants::MeshIdx].vertexBufferAddress
        };

        vkCmdPushConstants(cmd, this->_voxelRasterPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pc);
        vkCmdBindIndexBuffer(cmd, this->_testMeshes[Constants::MeshIdx].indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, this->_testMeshes[Constants::MeshIdx].numIndices, 1, 0, 0, 0);
        vkCmdEndRendering(cmd);
    }

    void updateVoxelVolume(VkCommandBuffer cmd)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_voxelizerPipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_voxelizerPipeline.layout, 0, 1, this->_voxelizerPipeline.descriptorSets.data(), 0, nullptr);
        VoxelizerPushConstants pc = {
            .gridSize = glm::vec3(Constants::VoxelGridResolution),
            .gridScale = 1.0f/Constants::VoxelGridResolution,
            .time = static_cast<float>(this->_elapsed)
        };
        vkCmdPushConstants(cmd, this->_voxelizerPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelizerPushConstants), &pc);

        constexpr uint32_t localWorkgroupSize = 8;
        constexpr uint32_t groupCount = Constants::VoxelGridResolution / localWorkgroupSize;
        vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
    }

    void rayCastVoxelVolume(VkCommandBuffer cmd)
    {
        // wait for rasterizer to complete
        VkBufferMemoryBarrier bufferBarriers[] = {
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .buffer = this->_voxelVolume.buffer,
                .offset = 0,
                .size = Constants::VoxelGridSize,
            },
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .buffer = this->_voxelFragmentCounter.buffer,
                .offset = 0,
                .size = sizeof(VoxelFragmentCounter)
            }
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, bufferBarriers, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_raytracerPipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_raytracerPipeline.layout, 0, 1, this->_raytracerPipeline.descriptorSets.data(), 0, nullptr);

        RayTracerPushConstants rtpc = {
            .inverseProjection = glm::inverse(this->_camera.GetProjectionMatrix()),
            .inverseView = glm::inverse(this->_camera.GetViewMatrix()),
            .cameraPos = glm::inverse(this->_camera.GetViewMatrix()) * glm::vec4(0.0, 0.0, 0.0, 1.0),
            .nearPlane = 0.1f,
            .screenSize = glm::vec2(this->_windowExtent.width, this->_windowExtent.height),
            .maxDistance = 2000.0f,
            .stepSize = 0.1,
            .gridSize = glm::vec3(Constants::VoxelGridResolution),
            .gridScale = Constants::VoxelGridScale,
            .lightSource = glm::vec4(30.0, 50.0, 10.0, 1.0),
            // .baseColor = glm::vec4(HEXCOLOR(0xFFBF00))
            .baseColor = glm::vec4(HEXCOLOR(0x675CFF))
        };
        vkCmdPushConstants(cmd, this->_raytracerPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
        VkExtent3D groupCounts = getWorkgroupCounts(8);
        vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
    }

    bool draw(double dt)
    {
        // std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        // wait for gpu to be done with last frame and clean
        VK_ASSERT(vkWaitForFences(this->_device, 1, &getCurrentFrame().renderFence, true, Constants::TimeoutNs));
        VK_ASSERT(vkResetFences(this->_device, 1, &getCurrentFrame().renderFence));
        getCurrentFrame().deletionQueue.flush(); // delete per-frame objects

        // register the semaphore to be signalled once the next frame (result of this call) is ready. does not block
        VkResult res;
        uint32_t swapchainImageIndex;
        res = vkAcquireNextImageKHR(this->_device, this->_swapchain, Constants::TimeoutNs, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex);
        if(res == VK_ERROR_OUT_OF_DATE_KHR) {
            this->_resizeRequested = true;
            return false;
        }
        VkImage frameImage = this->_swapchainImages[swapchainImageIndex];

        VkCommandBuffer cmd = getCurrentFrame().commandBuffer;
        VK_ASSERT(vkResetCommandBuffer(cmd, 0)); // we can safely reset as we waited on the fence


        // begin recording commands
        VkCommandBufferBeginInfo cmdBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };
        VK_ASSERT(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

        clearImage(cmd, this->_drawImage);
        // vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // drawGeometry(cmd, dt);
        // vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

        // updateVoxelVolume(cmd);
        voxelRasterizeGeometry(cmd);
        rayCastVoxelVolume(cmd);

        // prepare drawImage to swapchainImage copy
        AllocatedImage& drawImage = this->_drawImage;
        VkImage& swapchainImage = this->_swapchainImages[swapchainImageIndex];

        vkutil::transition_image(cmd, drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkutil::copy_image_to_image(cmd, drawImage.image, swapchainImage, this->_windowExtent, VkExtent3D{.width = this->_swapchainExtent.width, .height = this->_swapchainExtent.height, .depth = 1});


        // vkutil::transition_image(cmd, this->_voxelImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        // vkutil::copy_image_to_image(cmd, this->_voxelImage.image, swapchainImage, this->_voxelImage.imageExtent, VkExtent3D{.width = this->_swapchainExtent.width, .height = this->_swapchainExtent.height, .depth = 1});
        vkutil::transition_image(cmd, this->_voxelImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        clearImage(cmd, this->_voxelImage);

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
            // .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
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
        res = vkQueuePresentKHR(this->_graphicsQueue, &presentInfo);
        if(res == VK_ERROR_OUT_OF_DATE_KHR) {
            this->_resizeRequested = true;
        }
        return true;
    }

    bool preProcess()
    {
        immediateSubmit([&](VkCommandBuffer cmd) {
            voxelRasterizeGeometry(cmd);
        });
        VoxelFragmentCounter fragCounter;
        std::memcpy(&fragCounter, this->_voxelFragmentCounter.allocation->GetMappedData(), sizeof(VoxelFragmentCounter));
        std::cout << "Voxel Fragment Count: " << fragCounter.fragCount << std::endl;
        // exit(0);
        return true;
    }

    bool initWindow()
    {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        SDL_Init(SDL_INIT_VIDEO);
        SDL_WindowFlags windowFlags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
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

        VkPhysicalDeviceFeatures featuresBase {
            .geometryShader = true,
            .fragmentStoresAndAtomics = true
        };

        vkb::PhysicalDeviceSelector selector {vkbInstance};
        vkb::PhysicalDevice physDevice = selector
            .set_minimum_version(1, 3)
            .set_required_features_13(features)
            .set_required_features_12(features12)
            .set_required_features(featuresBase)
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

    void resizeSwapchain()
    {
        vkDeviceWaitIdle(this->_device);
        destroySwapchain();

        int w, h;
        SDL_GetWindowSize(this->_window, &w, &h);
        this->_windowExtent.width = w;
        this->_windowExtent.height = h;

        createSwapchain(this->_windowExtent.width, this->_windowExtent.height);
        this->_resizeRequested = false;
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
            .imageType = (extent.depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
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
        // IMAGES
        this->_drawImage = createImage(
            Constants::DrawImageResolution,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FORMAT_R32G32B32A32_SFLOAT
        );
        immediateSubmit([&](VkCommandBuffer cmd) {
            vkutil::transition_image(cmd, this->_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        });

        this->_voxelImage = createImage(
            VkExtent3D{Constants::VoxelGridResolution, Constants::VoxelGridResolution, 1},
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FORMAT_R32G32B32A32_SFLOAT
        );
        immediateSubmit([&](VkCommandBuffer cmd) {
            vkutil::transition_image(cmd, this->_voxelImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        });

        // BUFFERS
        this->_stagingBuffer = createBuffer(Constants::StagingBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
        this->_voxelVolume = createBuffer(Constants::VoxelGridSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
        this->_voxelInfoBuffer = createBuffer(sizeof(VoxelInfo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
        VoxelInfo voxInfo = {
            .gridDimensions = glm::vec3(Constants::VoxelGridResolution),
            .gridScale = Constants::VoxelGridScale
        };
        void* data = this->_stagingBuffer.allocation->GetMappedData();
        std::memcpy(data, &voxInfo, sizeof(voxInfo));
        immediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy copy = {
                .srcOffset = 0,
                .dstOffset = 0,
                .size = sizeof(VoxelInfo)
            };
            vkCmdCopyBuffer(cmd, this->_stagingBuffer.buffer, this->_voxelInfoBuffer.buffer, 1, &copy);
        });
        std::cout << "VOXBUFFMEM: " << this->_voxelVolume.info.size << std::endl;

        this->_voxelFragmentCounter = createBuffer(
            sizeof(VoxelFragmentCounter),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_MEMORY_USAGE_CPU_ONLY
        );
        uint32_t zero = 0;
        // std::memcpy(this->_voxelFragmentCounter.allocation->GetMappedData(), &zero, sizeof(uint32_t));
        // std::memset(this->_voxelFragmentCounter.allocation->GetMappedData(), 0, sizeof(VoxelFragmentCounter));

        // MESHES
        std::vector<std::vector<Vertex>> vertexBuffers;
        std::vector<std::vector<uint32_t>> indexBuffers;
        if(!loadGltfMeshes({ASSETS_DIRECTORY"/xyz.glb"}, vertexBuffers, indexBuffers)) {
            std::cout << "[ERROR] Failed to load meshes" << std::endl;
        }
        for(int m = 0; m < vertexBuffers.size(); m++) {
            this->_testMeshes.emplace_back(uploadMesh(vertexBuffers[m], indexBuffers[m]));
        }


        return true;
    }

    bool initDescriptors()
    {
        std::vector<DescriptorPool::DescriptorQuantity> sizes = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}
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

        // MANUAL VOXELIZER
        this->_voxelizerPipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_voxelizerPipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_voxelizerPipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_buffer(0, this->_voxelVolume.buffer, Constants::VoxelGridSize, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .update_set(this->_device, this->_voxelizerPipeline.descriptorSets[0]);

        // VOXEL RASTERIZER
        this->_voxelRasterPipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .add_binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            .add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .build(this->_device, VK_SHADER_STAGE_FRAGMENT_BIT);
        this->_voxelRasterPipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_voxelRasterPipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_buffer(0, this->_voxelVolume.buffer, this->_voxelVolume.info.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .write_buffer(1, this->_voxelInfoBuffer.buffer, sizeof(VoxelInfo), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            .write_buffer(2, this->_voxelFragmentCounter.buffer, sizeof(VoxelFragmentCounter), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .update_set(this->_device, this->_voxelRasterPipeline.descriptorSets[0]);
        
        // VOXEL RENDERER
        this->_raytracerPipeline.descriptorLayout = DescriptorLayoutBuilder::newLayout()
            .add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .build(this->_device, VK_SHADER_STAGE_COMPUTE_BIT);
        this->_raytracerPipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, this->_raytracerPipeline.descriptorLayout));
        this->_descriptorWriter
            .clear()
            .write_buffer(0, this->_voxelVolume.buffer, Constants::VoxelGridSize, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            .write_image(1, this->_drawImage.imageView, 0, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            .update_set(this->_device, this->_raytracerPipeline.descriptorSets[0]);

        this->_deletionQueue.push([&]() {
            this->_descriptorPool.destroy(this->_device);
            vkDestroyDescriptorSetLayout(this->_device, this->_drawImagePipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, this->_voxelizerPipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, this->_voxelRasterPipeline.descriptorLayout, nullptr);
            vkDestroyDescriptorSetLayout(this->_device, this->_raytracerPipeline.descriptorLayout, nullptr);
        });
        return true;
    }

    template <typename PushConstants>
    bool createComputePipeline(const std::string& shaderFilename, ComputePipeline& newPipeline, bool autoCleanup = true)
    {
        const VkPushConstantRange pushConstants = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(PushConstants),
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
        return createComputePipeline<VoxelizerPushConstants>(SHADER_DIRECTORY"/voxelizer.comp.spv", this->_voxelizerPipeline) &&
               createComputePipeline<RayTracerPushConstants>(SHADER_DIRECTORY"/voxelTracer.comp.spv", this->_raytracerPipeline);
    }

    bool initMeshPipeline()
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
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_multisampling_none()
            .enable_blending_additive()
            .enable_depthtest(false, VK_COMPARE_OP_ALWAYS)
            .set_color_attachment_format(this->_drawImage.imageFormat)
            .set_depth_format(VK_FORMAT_D32_SFLOAT)
            .build_pipeline(this->_device);

        vkDestroyShaderModule(this->_device, fragShader, nullptr);
        vkDestroyShaderModule(this->_device, vertShader, nullptr);

        this->_deletionQueue.push([this]() {
            vkDestroyPipelineLayout(this->_device, this->_meshPipeline.layout, nullptr);
            vkDestroyPipeline(this->_device, this->_meshPipeline.pipeline, nullptr);
        });

        return true;
    }

    bool initVoxelRasterPipeline()
    {
        const VkPushConstantRange pushConstantsRanges[] = {
            {
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                .size = sizeof(GraphicsPushConstants),
            }
        };

        // No descriptors
        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &this->_voxelRasterPipeline.descriptorLayout,
            .pushConstantRangeCount = sizeof(pushConstantsRanges)/sizeof(VkPushConstantRange),
            .pPushConstantRanges = pushConstantsRanges
        };
        VK_CHECK(vkCreatePipelineLayout(this->_device, &pipelineLayoutInfo, nullptr, &this->_voxelRasterPipeline.layout));

        VkShaderModule fragShader;
        if(!vkutil::load_shader_module(SHADER_DIRECTORY"/voxelRaster.frag.spv", this->_device, &fragShader)) {
            std::cout << "[ERROR] Failed to load fragment shader" << std::endl;
            return false;
        }

        VkShaderModule vertShader;
        if(!vkutil::load_shader_module(SHADER_DIRECTORY"/voxelRaster.vert.spv", this->_device, &vertShader)) {
            std::cout << "[ERROR] Failed to load vertex shader" << std::endl;
            return false;
        }

        VkShaderModule geomShader;
        if(!vkutil::load_shader_module(SHADER_DIRECTORY"/voxelRaster.geom.spv", this->_device, &geomShader)) {
            std::cout << "[ERROR] Failed to load geometry shader" << std::endl;
            return false;
        }

        PipelineBuilder builder;
        builder._pipelineLayout = this->_voxelRasterPipeline.layout;
        this->_voxelRasterPipeline.pipeline = builder
            .set_shaders(vertShader, fragShader, geomShader)
            .set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .set_polygon_mode(VK_POLYGON_MODE_FILL)
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE)
            .set_multisampling_none()
            .enable_blending_additive()
            .disable_depthtest()
            .set_color_attachment_format(this->_voxelImage.imageFormat)
            .set_depth_format(VK_FORMAT_UNDEFINED)
            .build_pipeline(this->_device);

        vkDestroyShaderModule(this->_device, fragShader, nullptr);
        vkDestroyShaderModule(this->_device, vertShader, nullptr);
        vkDestroyShaderModule(this->_device, geomShader, nullptr);

        this->_deletionQueue.push([this]() {
            vkDestroyPipelineLayout(this->_device, this->_voxelRasterPipeline.layout, nullptr);
            vkDestroyPipeline(this->_device, this->_voxelRasterPipeline.pipeline, nullptr);
        });

        return true;        
    }

    bool initPipelines()
    {
        if(!initComputePipelines()) {
            std::cout << "[ERROR] Failed to init compute pipelines" << std::endl;
            return false;
        }

        if(!initMeshPipeline()) {
            std::cout << "[ERROR] Failed to init mesh graphics pipeline" << std::endl;
            return false;
        }

        if(!initVoxelRasterPipeline()) {
            std::cout << "[ERROR] Failed to init voxel raster graphics pipeline" << std::endl;
            return false;
        }

        return true;
    }

    bool initCamera()
    {
        // this->_camera.pos = glm::vec3(3.0, -1.5, 3.0);
        // this->_camera.view = glm::translate(-this->_camera.pos) * glm::rotate(glm::radians(-80.0f), glm::vec3(0.0, -1.0, 0.0));
        // this->_camera.view = glm::lookAt(this->_camera.pos, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        // this->_camera.projection = glm::perspective(glm::radians(70.f), (float)this->_windowExtent.width / (float)_windowExtent.height, 0.1f, 1000.0f);
        this->_origin.SetPosition(glm::vec3(0.0, Constants::CameraPosition.y, 0.0));
        this->_origin.Update();

        this->_camera.SetView(Constants::CameraPosition, glm::vec3(0.0, 0.0, 0.0), glm::vec3(0.0, 1.0, 0.0));
        // this->_camera.SetViewMatrix(glm::lookAt(Constants::CameraPosition, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0)));
        this->_camera.SetPerspective(glm::perspective(glm::radians(70.f), (float)this->_windowExtent.width / (float)_windowExtent.height, 0.01f, 1000.0f));
        this->_camera.Attach(&this->_origin);
        this->_camera.OrbitYaw(PI/2.0f);
        this->_camera.OrbitPitch(PI/2.0f);
        this->_camera.SetupViewMatrix();

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
    /* ENGINE */
    SDL_Window* _window {};
    VkExtent3D _windowExtent {}; 
    VkExtent2D _drawExtent;
    bool _isInitialized {};
    bool _resizeRequested {false};
    DeletionQueue _deletionQueue;
    std::string _name {};
    std::atomic<bool> _shouldClose {false};
    double _elapsed {};
    double _lastFpsMeasurementTime {};
    Mouse _mouse;
    MouseMap _mouseButtons;

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
    std::vector<VkImage> _swapchainImages;
    std::vector<VkImageView> _swapchainImageViews;
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

    /* RENDER DATA */
    Camera _camera;
    Transform _origin;

    // Compute Shaders
    ComputePipeline _drawImagePipeline;
    ComputePipeline _voxelizerPipeline;
    ComputePipeline _raytracerPipeline;

    // Triangle Rasterization 
    GraphicsPipeline _meshPipeline;
    GraphicsPipeline _voxelRasterPipeline;

    // Shader Resources
    AllocatedImage _drawImage;
    AllocatedImage _voxelImage;
    AllocatedBuffer _stagingBuffer;
    AllocatedBuffer _voxelVolume;
    AllocatedBuffer _voxelInfoBuffer;
    AllocatedBuffer _voxelFragmentCounter;
    VkSampler _simpleSampler {};

    // Meshes
    std::vector<GPUMeshBuffers> _testMeshes;

};

int main(int argc, char* argv[])
{
    // {
    //     using namespace glm;
    // //Orthograhic projection
    // mat4 Ortho; 
    // //Create an modelview-orthographic projection matrix see from +X axis
    // Ortho = glm::ortho( -0.5f, 0.5f, -0.5f, 0.5f, 0.0f, -1.0f ) * Constants::VoxelGridScale;

    // mat4 mvpX = Ortho * glm::lookAt( vec3( 0.5 * Constants::VoxelGridScale, 0, 0 ), vec3( 0, 0, 0 ), vec3( 0, 1.0, 0 ) );

    // //Create an modelview-orthographic projection matrix see from +Y axis
    // mat4 mvpY = Ortho * glm::lookAt( vec3( 0, 0.5 * Constants::VoxelGridScale, 0 ), vec3( 0, 0, 0 ), vec3( 0, 0, -1.0 ) );

    // //Create an modelview-orthographic projection matrix see from +Z axis
    // mat4 mvpZ = Ortho * glm::lookAt( vec3( 0, 0, 0.5 * Constants::VoxelGridScale ), vec3( 0, 0, 0 ), vec3( 0, 1.0, 0 ) );
    // std::cout << glm::to_string(mvpX) << std::endl;
    // std::cout << glm::to_string(mvpY) << std::endl;
    // std::cout << glm::to_string(mvpZ) << std::endl;
    // exit(0);
    // }

    Renderer renderer("VulkanFlow", 900, 900);
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
// #define VMA_IMPLEMENTATION
#include "renderer.hpp"
#include "SDL_events.h"
#include "vk_initializers.h"
#include "vk_pipelines.h"
#include "vk_images.h"
#include "defines.hpp"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl2.h"
#include "ui_tools.hpp"
#include <vulkan/vulkan_core.h>

namespace Constants
{
    constexpr bool IsValidationLayersEnabled = true;
    constexpr bool VSYNCEnabled = true;

    constexpr uint32_t FPSMeasurePeriod = 60;
    constexpr uint64_t TimeoutNs = 100000000;
    constexpr uint32_t MaxDescriptorSets = 10;

    constexpr glm::vec3 CameraPosition = glm::vec3(0.0, 0.0, 3.0);
    constexpr VkExtent3D DrawImageResolution {2560, 1440, 1};
}

static inline void
printDeviceProperties(vkb::PhysicalDevice& dev)
{
    const uint32_t* wgs = dev.properties.limits.maxComputeWorkGroupCount;
    std::cout << dev.properties.deviceName << "\n" <<
    "WorkGroups:      " << wgs[0] << " x " << wgs[1] << " x " << wgs[2] << ") \n" << 
    "Compute Invocations:      " << dev.properties.limits.maxComputeWorkGroupInvocations  << '\n' <<
    "PushConstants:   " << dev.properties.limits.maxPushConstantsSize << "\n" <<
    "Uniform Buffers: " << dev.properties.limits.maxUniformBufferRange << std::endl;
}

void
FillBuffers(const std::vector<Buffer>& buffers, uint32_t data, uint32_t offset)
{

}

Renderer::Renderer(const std::string& name, uint32_t width, uint32_t height)
:   _name(name), _windowExtent{.width = width, .height = height} {}

Renderer::~Renderer()
{
    // if(!this->_isInitialized) {
    //     return;
    // }

    vkDeviceWaitIdle(this->_device);
    for(FrameData &frame : this->_frames) {
        vkDestroyCommandPool(this->_device, frame.commandPool, nullptr);
        vkDestroyFence(this->_device, frame.renderFence, nullptr);
        vkDestroySemaphore(this->_device, frame.swapchainSemaphore, nullptr);
        vkDestroySemaphore(this->_device, frame.renderSemaphore, nullptr);
        frame.deletionQueue.flush();
    }

    // for(Mesh& mesh : this->_testMeshes) {
    //     mesh.Destroy(this->_allocator);
    // }

    this->_deletionQueue.flush();
    destroySwapchain();
    destroyVulkan();
    SDL_DestroyWindow(this->_window);
    SDL_Quit();
}

bool
Renderer::Init()
{
    this->_isInitialized =  initWindow() &&
                            initVulkan() &&
                            initSwapchain() &&
                            initCommands() &&
                            initSyncStructures() &&
                            initUI() &&
                            initDescriptorPool() &&
                            initResources() &&
                            initCamera() &&
                            initControls();
    return this->_isInitialized;
}

void
Renderer::RegisterPreFrameCallback(std::function<void()>&& callback)
{
    this->_userPreFrame = callback;
}

void
Renderer::RegisterUpdateCallback(std::function<void(VkCommandBuffer, float)>&& callback)
{
    this->_userUpdate = callback;
}

void
Renderer::RegisterUICallback(std::function<void()>&& callback)
{
    this->_userUI = callback;
}

void
Renderer::Update(float dt)
{
    this->_elapsed += dt;
    updatePerformanceCounters(dt);
    pollEvents();
    if(this->_resizeRequested) {
        resizeSwapchain();
        initCamera();
        this->_mouse.first_captured = true;
        this->_resizeRequested = false;
    }
    // this->_camera.OrbitYaw(glm::radians(30.0) * dt);
    this->_camera.Update();
    render(dt);
    this->_frameNumber++;
}

bool
Renderer::render(float dt)
{
    // wait for gpu to be done with last frame and clean
    VK_ASSERT(vkWaitForFences(this->_device, 1, &getCurrentFrame().renderFence, true, Constants::TimeoutNs));
    VK_ASSERT(vkResetFences(this->_device, 1, &getCurrentFrame().renderFence));
    getCurrentFrame().deletionQueue.flush(); // delete per-frame objects

    if(this->_userPreFrame) {
        this->_userPreFrame();
    }

    if(this->_userUI) {
#include "defines.hpp"
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        this->_userUI();
        ImGui::Render();
    }

    // register the semaphore to be signalled once the next frame (result of this call) is ready. does not block
    VkResult res;
    uint32_t swapchainImageIndex;
    res = vkAcquireNextImageKHR(this->_device, this->_swapchain, Constants::TimeoutNs, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex);
    if(res == VK_ERROR_OUT_OF_DATE_KHR) {
        this->_resizeRequested = true;
        return false;
    }

    VkCommandBuffer cmd = getCurrentFrame().commandBuffer;
    VK_ASSERT(vkResetCommandBuffer(cmd, 0)); // we can safely reset as we waited on the fence

    // begin recording commands
    VkCommandBufferBeginInfo cmdBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    VK_ASSERT(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    this->_drawImage.Clear(cmd);
    VkImageMemoryBarrier clearBarrier = this->_drawImage.CreateBarrier(
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
    );
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 1, &clearBarrier);

    /* BEGIN USER COMMANDS */
    
    if(this->_userUpdate)
        this->_userUpdate(cmd, dt);

    /* END USER COMMANDS */

    // prepare drawImage to swapchainImage copy
    Image& drawImage = this->_drawImage;
    Image& swapchainImage = this->_swapchainImages[swapchainImageIndex];

    // wait for any user commands on draw image to complete
    VkImageMemoryBarrier drawImageBarrier = this->_drawImage.CreateBarrier(
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT
    );
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 1, &drawImageBarrier);

    drawImage.Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    swapchainImage.Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // vkutil::transition_image(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkutil::copy_image_to_image(cmd, drawImage.image, swapchainImage.image, this->_windowExtent, VkExtent3D{.width = this->_swapchainExtent.width, .height = this->_swapchainExtent.height, .depth = 1});

    if(this->_userUI) {
        swapchainImage.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        drawUI(cmd, swapchainImage.imageView);
    }

    // transition to present format
    swapchainImage.Transition(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    drawImage.Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);

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

void
Renderer::drawUI(VkCommandBuffer cmd, VkImageView targetImageView)
{
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(_swapchainExtent, &colorAttachment, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkCmdEndRendering(cmd);
}

// NOT SAFE TO RUN AFTER RENDER LOOP STARTED:
// TODO: update to use different queue so concurrent immediate operations are safe
bool
Renderer::ImmediateSubmit(std::function<void(VkCommandBuffer)>&& immediateFunction)
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

void
Renderer::CreateBuffer(Buffer &newBuffer, uint64_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, bool autoCleanup)
{
    newBuffer.Allocate(this->_allocator, allocSize, usage, memoryUsage);
    if(autoCleanup) {
        this->_deletionQueue.push([this, &newBuffer]() {
            newBuffer.Destroy(this->_allocator);
            // vmaDestroyBuffer(this->_allocator, newBuffer.buffer, newBuffer.allocation);
        });
    }
}


void
Renderer::CreateImage(Image &newImg, VkExtent3D extent, VkImageUsageFlags usageFlags, VkImageAspectFlags aspectFlags, VkFormat format, VkImageLayout layout, bool autoCleanup)
{
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
   
    ImmediateSubmit([&newImg, layout](VkCommandBuffer cmd) {
        newImg.Transition(cmd, layout);
    });

    if(autoCleanup) {
        this->_deletionQueue.push([this, newImg]() {
            vmaDestroyImage(this->_allocator, newImg.image, newImg.allocation);
            vkDestroyImageView(this->_device, newImg.imageView, nullptr);
        });
    }
}

bool
Renderer::CreateComputePipeline(ComputePipeline& newPipeline, const std::string& shaderFilename, std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors, uint32_t pushConstantsSize, bool autoCleanup)
{
    createPipelineDescriptors(newPipeline, VK_SHADER_STAGE_COMPUTE_BIT, descriptors);

    const VkPushConstantRange pushConstants = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = pushConstantsSize,
    };

    VkPipelineLayoutCreateInfo pipelineLayout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &newPipeline.descriptorLayout,

        .pushConstantRangeCount = (pushConstantsSize > 0),
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
            vkDestroyDescriptorSetLayout(this->_device, newPipeline.descriptorLayout, nullptr);
            vkDestroyPipelineLayout(this->_device, newPipeline.layout, nullptr);
            vkDestroyPipeline(this->_device, newPipeline.pipeline, nullptr);
        });
    }
    return true;
}

bool
Renderer::CreateGraphicsPipeline(GraphicsPipeline &newPipeline, const std::string &vertexShaderFile, const std::string &fragmentShaderFile, const std::string &geometryShaderFile, 
                                 std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors, VkShaderStageFlags descriptorShaderStages, uint32_t pushConstantsSize, GraphicsPipelineOptions options, bool autoCleanup)
{
    createPipelineDescriptors(newPipeline, descriptorShaderStages, descriptors);
    const VkPushConstantRange pushConstantsRange = {
        .stageFlags = options.pushConstantsStages,
        .offset = 0,
        .size = pushConstantsSize,
    };

    // No descriptors
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = newPipeline.descriptorLayout != VK_NULL_HANDLE,
        .pSetLayouts = &newPipeline.descriptorLayout,
        .pushConstantRangeCount = (pushConstantsSize > 0),
        .pPushConstantRanges = &pushConstantsRange
    };
    VK_CHECK(vkCreatePipelineLayout(this->_device, &pipelineLayoutInfo, nullptr, &newPipeline.layout));

    VkShaderModule vertShader;
    if(!vkutil::load_shader_module(vertexShaderFile, this->_device, &vertShader)) {
        std::cout << "[ERROR] Failed to load vertex shader " << vertexShaderFile << std::endl;
        return false;
    }

    VkShaderModule fragShader;
    if(!vkutil::load_shader_module(fragmentShaderFile, this->_device, &fragShader)) {
        std::cout << "[ERROR] Failed to load fragment shader" << std::endl;
        return false;
    }

    VkShaderModule geomShader;
    if(!geometryShaderFile.empty()) {
        if(!vkutil::load_shader_module(geometryShaderFile, this->_device, &geomShader)) {
            std::cout << "[ERROR] Failed to load geometry shader" << std::endl;
            return false;
        }
    }

    PipelineBuilder builder;
    builder._pipelineLayout = newPipeline.layout;

    if(geometryShaderFile.empty()) {
        builder.set_shaders(vertShader, fragShader);
    } else {
        builder.set_shaders(vertShader, fragShader, geomShader);
    }

    builder
        .set_input_topology(options.inputTopology)
        .set_polygon_mode(options.polygonMode)
        .set_cull_mode(options.cullMode, options.frontFace)
        .set_multisampling_none()
        .enable_depthtest(options.depthTestEnabled , options.depthTestOp)
        .set_color_attachment_format(options.colorAttachmentFormat)
        .set_depth_format(options.depthFormat);

    switch(options.blendMode) {
        case BlendMode::Additive:
            builder.enable_blending_additive();
            break;
        case BlendMode::AlphaBlend:
            builder.enable_blending_alphablend();
            break;
        default:
            builder.disable_blending();
            break;
    }

    newPipeline.pipeline = builder.build_pipeline(this->_device);

    vkDestroyShaderModule(this->_device, fragShader, nullptr);
    vkDestroyShaderModule(this->_device, vertShader, nullptr);
    if(!geometryShaderFile.empty()) {
        vkDestroyShaderModule(this->_device, geomShader, nullptr);
    }

    this->_deletionQueue.push([this, newPipeline]() {
        vkDestroyDescriptorSetLayout(this->_device, newPipeline.descriptorLayout, nullptr);
        vkDestroyPipelineLayout(this->_device, newPipeline.layout, nullptr);
        vkDestroyPipeline(this->_device, newPipeline.pipeline, nullptr);
    });

    return true;
}

template <typename PipelineType>
void Renderer::createPipelineDescriptors(PipelineType& pipeline, VkShaderStageFlags shaderStages, std::vector<std::variant<BufferDescriptor, ImageDescriptor>> descriptors)
{
    if(descriptors.empty()) {
        return;
    }
    // create the layout based on the first descriptor set (we assume all sets have the same layout)
    std::unordered_map<unsigned int, unsigned int> descriptorSetCounts;
    DescriptorLayoutBuilder layoutBuilder = DescriptorLayoutBuilder::newLayout();
    for(const auto& desc : descriptors) {
        std::visit([&descriptorSetCounts, &layoutBuilder](const auto& d) {
            descriptorSetCounts[d.set] += 1;
            if(d.set != 0) { return; }
            layoutBuilder.add_binding(d.binding, d.type);
        }, desc);
    }
    pipeline.descriptorLayout = layoutBuilder.build(this->_device, shaderStages);

    // allocate number of implicitely requested sets
    for(int i = 0; i < descriptorSetCounts.size(); i++) {
        pipeline.descriptorSets.push_back(this->_descriptorPool.allocateSet(this->_device, pipeline.descriptorLayout));
    }

    unsigned int currentSet = 0;
    this->_descriptorWriter.clear();
    for(const auto& desc : descriptors) {
        unsigned int set = std::visit([](const auto& d){return d.set;}, desc);
        if(currentSet != set) {
            // moved onto new set description, finalize old one
            this->_descriptorWriter.update_set(this->_device, pipeline.descriptorSets[currentSet]);
            currentSet = set;
        }

        if(std::holds_alternative<BufferDescriptor>(desc)) {
            const auto& buf = std::get<BufferDescriptor>(desc);
            this->_descriptorWriter.write_buffer(buf.binding, buf.handle, buf.size, buf.offset, buf.type);
        }
        else if(std::holds_alternative<ImageDescriptor>(desc)) {
            const auto& img = std::get<ImageDescriptor>(desc);
            this->_descriptorWriter.write_image(img.binding, img.imageView, img.sampler, img.layout, img.type);
        }
    }

    // update last set
    this->_descriptorWriter.update_set(this->_device, pipeline.descriptorSets[currentSet]);
}


void
Renderer::UploadMesh(Mesh& mesh, std::span<Vertex> vertices, std::span<uint32_t> indices)
{
	// Initialize GPU buffers to store mesh data (vertex attributes + triangle indices)
	const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
	const size_t indexBufferSize = indices.size() * sizeof(uint32_t);
	mesh.vertexBuffer.Allocate(this->_allocator,
		vertexBufferSize, 
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	VkBufferDeviceAddressInfo deviceAddressInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = mesh.vertexBuffer.bufferHandle
	};
	mesh.vertexBufferAddress = vkGetBufferDeviceAddress(this->_device, &deviceAddressInfo);

	CreateBuffer(
		mesh.indexBuffer,
		indexBufferSize, 
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY,
		false
	);
	
	// Copy mesh data to cpu staging buffer (host visible and memory coherent)
	Buffer meshStagingBuffer;
	CreateBuffer(
		meshStagingBuffer,
		vertexBufferSize + indexBufferSize,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_CPU_ONLY,
		false
	);
	void* stagingData = meshStagingBuffer.info.pMappedData;
	
	std::memcpy(stagingData, vertices.data(), vertexBufferSize);
	std::memcpy((char *)stagingData + vertexBufferSize, indices.data(), indexBufferSize);

	// Copy mesh data from CPU to GPU
	ImmediateSubmit([&](VkCommandBuffer cmd) {
			VkBufferCopy vertexCopy = {
				.srcOffset = 0,
				.dstOffset = 0,
				.size = vertexBufferSize
			};
			vkCmdCopyBuffer(cmd, meshStagingBuffer.bufferHandle, mesh.vertexBuffer.bufferHandle, 1, &vertexCopy);

			VkBufferCopy indexCopy = {
				.srcOffset = vertexBufferSize,
				.dstOffset = 0,
				.size = indexBufferSize
			};
			vkCmdCopyBuffer(cmd, meshStagingBuffer.bufferHandle, mesh.indexBuffer.bufferHandle, 1, &indexCopy);
	});
	meshStagingBuffer.Destroy(this->_allocator);
	mesh.numIndices = indices.size();
	mesh.numVertices = vertices.size();

    this->_deletionQueue.push([this, mesh]() mutable {
        mesh.vertexBuffer.Destroy(this->_allocator);
        mesh.indexBuffer.Destroy(this->_allocator);
    });
}

void
Renderer::CreateTimestampQueryPool(TimestampQueryPool &pool, uint32_t numTimestamps)
{
	std::cout << this->_vkbDev.properties.limits.timestampPeriod << std::endl;
	pool.init(this->_device, numTimestamps, Constants::FrameOverlap, this->_vkbDev.properties.limits.timestampPeriod);
	this->_deletionQueue.push([this, pool](){
		vkDestroyQueryPool(this->_device, pool.queryPool, nullptr);
	});
}

bool
Renderer::initWindow()
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
	SDL_AddEventWatch(&Renderer::eventCallback, this);

    return this->_window != nullptr;
}

bool
Renderer::initVulkan()
{
    vkb::InstanceBuilder builder;
    vkb::Result<vkb::Instance> instanceRet = builder
        .set_app_name(this->_name.c_str())
        .request_validation_layers(Constants::IsValidationLayersEnabled)
        .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
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
        .hostQueryReset = true,
        .bufferDeviceAddress = true,
    };

    VkPhysicalDeviceFeatures featuresBase {
        .geometryShader = true,
        .wideLines = true,
        .fragmentStoresAndAtomics = true,
        .shaderInt64 = true
    };

	VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeatures{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT,
		.shaderBufferFloat32Atomics = true
	};

    vkb::PhysicalDeviceSelector selector {vkbInstance};
    vkb::PhysicalDevice physDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features)
        .set_required_features_12(features12)
        .set_required_features(featuresBase)
		.add_required_extension("VK_EXT_shader_atomic_float")
        .set_surface(this->_surface)
        .select()
        .value();
    
    vkb::DeviceBuilder deviceBuilder {physDevice};
	deviceBuilder.add_pNext(&atomicFloatFeatures);
    vkb::Device vkbDevice = deviceBuilder.build().value();
    this->_device = vkbDevice.device;
    this->_gpu = physDevice.physical_device;
    // print gpu properties
    printDeviceProperties(physDevice);
	this->_vkbDev = physDevice;

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

void
Renderer::resizeSwapchain()
{
    vkDeviceWaitIdle(this->_device);
    destroySwapchain();

    int w, h;
    SDL_GetWindowSize(this->_window, &w, &h);
    this->_windowExtent.width = w;
    this->_windowExtent.height = h;

    createSwapchain(this->_windowExtent.width, this->_windowExtent.height);
}

bool
Renderer::createSwapchain(uint32_t width, uint32_t height)
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

    const std::vector<VkImage>& vkbImages = vkbSwapchain.get_images().value();
    const std::vector<VkImageView>& vkbImageViews = vkbSwapchain.get_image_views().value();

    this->_swapchainImages.clear();
    for(int i = 0; i < vkbImages.size(); i++) {
        this->_swapchainImages.push_back(Image{
            .image = vkbImages[i],
            .imageView = vkbImageViews[i],
            .imageExtent = VkExtent3D{this->_swapchainExtent.width, this->_swapchainExtent.height, 1},
            .imageFormat = this->_swapchainImageFormat
        });
    }
    this->_swapchain           = vkbSwapchain.swapchain;
    return true;
}

bool
Renderer::initSwapchain()
{
    if(!createSwapchain(this->_windowExtent.width, this->_windowExtent.height)) {
        std::cout << "[ERROR] Failed to create swapchain" << std::endl;
        return false;
    }
    return true;
}

bool
Renderer::initCommands()
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

bool 
Renderer::initSyncStructures()
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

bool
Renderer::initUI()
{
	// 1: create descriptor pool for IMGUI
	//  the size of the pool is very oversize, but it's copied from imgui demo
	//  itself.
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	VkDescriptorPool imguiPool;
	VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imguiPool));

	// 2: initialize imgui library

	// this initializes the core structures of imgui
	ImGui::CreateContext();

	// this initializes imgui for SDL
	ImGui_ImplSDL2_InitForVulkan(_window);

	// this initializes imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = this->_instance;
	init_info.PhysicalDevice = this->_gpu;
	init_info.Device = this->_device;
	init_info.Queue = this->_graphicsQueue;
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 3;
	init_info.ImageCount = 3;
	init_info.UseDynamicRendering = true;

	//dynamic rendering parameters for imgui to use
	init_info.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &this->_swapchainImageFormat;
	

	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&init_info);

	ImGui_ImplVulkan_CreateFontsTexture();

	ImGui::GetIO().WantCaptureMouse = false;

	// add the destroy the imgui created structures
	this->_deletionQueue.push([this, imguiPool]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(this->_device, imguiPool, nullptr);
	});
    return true;
}

bool
Renderer::initResources()
{
    CreateImage(this->_drawImage,
        Constants::DrawImageResolution,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_LAYOUT_GENERAL
    );
    return true;
}

bool
Renderer::initDescriptorPool()
{
    std::vector<DescriptorPool::DescriptorQuantity> sizes = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10}
    };
    this->_descriptorPool.init(this->_device, 10, sizes);
    this->_deletionQueue.push([&]() {
        this->_descriptorPool.destroy(this->_device);
    });
    return true;
}

bool Renderer::initCamera()
{
    this->_camera.SetPerspective(glm::perspective(glm::radians(70.f), (float)this->_windowExtent.width / (float)_windowExtent.height, 0.01f, 1000.0f));
    if(this->_resizeRequested) return true;
    this->_origin.SetPosition(glm::vec3(0.0, Constants::CameraPosition.y, 0.0));
    this->_origin.Update();

    this->_camera.SetView(Constants::CameraPosition, glm::vec3(0.0, 0.0, 0.0), glm::vec3(0.0, 1.0, 0.0));
    this->_camera.Attach(&this->_origin);
	this->_camera.OrbitYaw(PI/2); //TODO: why is this necessary to get proper coords
    this->_camera.SetupViewMatrix();

    return true;
}

bool Renderer::initControls()
{
    for (SDL_Keycode k = SDLK_F1; k <= SDLK_F12; ++k) {
        this->_keyMap[k] = false;
    }
    
    // Numbers
    for (SDL_Keycode k = SDLK_0; k <= SDLK_9; ++k) {
        this->_keyMap[k] = false;
    }
    
    // Letters
    for (SDL_Keycode k = SDLK_a; k <= SDLK_z; ++k) {
        this->_keyMap[k] = false;
    }
    
    // Special keys
    const SDL_Keycode specialKeys[] = {
        SDLK_RETURN, SDLK_ESCAPE, SDLK_BACKSPACE, SDLK_TAB,
        SDLK_SPACE, SDLK_CAPSLOCK,
        SDLK_PRINTSCREEN, SDLK_SCROLLLOCK, SDLK_PAUSE,
        SDLK_INSERT, SDLK_HOME, SDLK_PAGEUP,
        SDLK_DELETE, SDLK_END, SDLK_PAGEDOWN,
        SDLK_RIGHT, SDLK_LEFT, SDLK_DOWN, SDLK_UP,
        SDLK_NUMLOCKCLEAR,
        SDLK_KP_DIVIDE, SDLK_KP_MULTIPLY, SDLK_KP_MINUS,
        SDLK_KP_PLUS, SDLK_KP_ENTER, SDLK_KP_PERIOD,
        SDLK_LCTRL, SDLK_LSHIFT, SDLK_LALT, SDLK_LGUI,
        SDLK_RCTRL, SDLK_RSHIFT, SDLK_RALT, SDLK_RGUI
    };
    
    for (const auto& key : specialKeys) {
        this->_keyMap[key] = false;
    }
    
    // Numpad numbers
    for (SDL_Keycode k = SDLK_KP_1; k <= SDLK_KP_9; ++k) {
        this->_keyMap[k] = false;
    }
    this->_keyMap[SDLK_KP_0] = false;  // Add KP_0 separately since it's not consecutive

    const Uint8 mouseButtons[] = {
        SDL_BUTTON_LEFT,    // 1
        SDL_BUTTON_MIDDLE,  // 2
        SDL_BUTTON_RIGHT,   // 3
    };

    this->_mouseMap[SDL_BUTTON_LEFT] = false;
    this->_mouseMap[SDL_BUTTON_RIGHT] = false;
    this->_mouseMap[SDL_BUTTON_MIDDLE] = false;

    return true;
}

void Renderer::updatePerformanceCounters(float dt)
{
    if(this->_frameNumber > 0 && this->_frameNumber % Constants::FPSMeasurePeriod == 0) {
        const double delta = (this->_elapsed - this->_lastFpsMeasurementTime);
        const double averageFrameTime =  delta / Constants::FPSMeasurePeriod;
        const double fps = Constants::FPSMeasurePeriod / delta;
        std::cout << std::fixed << std::setprecision(3) << "FPS: " << fps  << std::setprecision(5) << "  (" << averageFrameTime << ") " << std::endl;
        this->_lastFpsMeasurementTime = this->_elapsed;
    }
}

int
Renderer::eventCallback(void* userdata, SDL_Event* event)
{
	Renderer* renderer = (Renderer*) userdata;
	if(event->type == SDL_MOUSEBUTTONDOWN) {
        int button = event->button.button;
		renderer->_mouseMap[button] = true;
	} else if(event->type == SDL_MOUSEBUTTONUP) {
		renderer->_mouseMap[event->button.button] = false;
        int button = event->button.button;
	}
	
	else if(event->type == SDL_MOUSEMOTION) {
		Mouse& mouse = renderer->_mouse;
		if (mouse.first_captured) {
			mouse.prev = {event->motion.x, event->motion.y};
			mouse.first_captured = false;
		}
		mouse.move = glm::vec2(event->motion.x, event->motion.y) - mouse.prev;
		mouse.prev = {event->motion.x, event->motion.y};
	}

	else if(event->type == SDL_MOUSEWHEEL) {
		renderer->_mouse.scroll += event->wheel.preciseY;
	}
    
    SDL_Keycode key = event->key.keysym.sym;
	if(event->type == SDL_KEYDOWN) {
		renderer->_keyMap[key] = true;
	}

	else if(event->type == SDL_KEYUP) {
		renderer->_keyMap[key] = false;
	}
	return 1;
}

void Renderer::pollEvents()
{
    SDL_Event event;
    while(SDL_PollEvent(&event)) {
		ImGui_ImplSDL2_ProcessEvent(&event);
        if(event.type == SDL_QUIT) {
            this->_shouldClose = true;
        } 
    }
}


void
Renderer::destroySwapchain()
{
    vkDestroySwapchainKHR(this->_device, this->_swapchain, nullptr);
    for(const Image& img : this->_swapchainImages)
    {
        vkDestroyImageView(this->_device, img.imageView, nullptr);
    }
}

void
Renderer::destroyVulkan()
{
    vkDestroySurfaceKHR(this->_instance, this->_surface, nullptr);
    vkDestroyDevice(this->_device, nullptr);
    vkb::destroy_debug_utils_messenger(this->_instance, this->_debugMessenger);
    vkDestroyInstance(this->_instance, nullptr);
}

FrameData&
Renderer::getCurrentFrame()
{ 
    return this->_frames[this->_frameNumber % Constants::FrameOverlap];
}

Camera&
Renderer::GetCamera()
{
    return this->_camera;
}

Image&
Renderer::GetDrawImage()
{
    return this->_drawImage;
}

VkDevice
Renderer::GetDevice()
{
	return this->_device;
}

VkViewport
Renderer::GetWindowViewport()
{
	return VkViewport{
		.x = 0,
		.y = (float)this->_windowExtent.height,
		.width = (float)this->_windowExtent.width,
		.height = -(float)this->_windowExtent.height,
		.minDepth = 0.0,
		.maxDepth = 1.0
	};
}

VkRect2D
Renderer::GetWindowScissor()
{
	return VkRect2D{
		.offset = { .x = 0, .y = 0 },
		.extent = { .width = this->_windowExtent.width, .height = this->_windowExtent.height }
	};
}

VkExtent2D
Renderer::GetWindowExtent2D()
{
    return VkExtent2D{this->_windowExtent.width, this->_windowExtent.height};
}

VkExtent3D
Renderer::GetWindowExtent3D()
{
    return VkExtent3D{this->_windowExtent.width, this->_windowExtent.height, 1};
}

float
Renderer::GetElapsedTime()
{
    return this->_elapsed;
}

uint32_t
Renderer::GetFrameNumber()
{
    return this->_frameNumber;
}

VkExtent3D
Renderer::GetWorkgroupCounts(uint32_t localGroupSize)
{
    return VkExtent3D {
        .width = static_cast<uint32_t>(std::ceil(this->_windowExtent.width/(float)localGroupSize)),
        .height = static_cast<uint32_t>(std::ceil(this->_windowExtent.height/(float)localGroupSize)),
        .depth = 1
    };
}

KeyMap&
Renderer::GetKeyMap()
{
    return this->_keyMap;
}

MouseMap&
Renderer::GetMouseMap()
{
    return this->_mouseMap;
}

Mouse&
Renderer::GetMouse()
{
    return this->_mouse;
}

bool
Renderer::ShouldClose() const
{
    return this->_shouldClose;
}

void 
Renderer::Close()
{
    this->_shouldClose = true;
}


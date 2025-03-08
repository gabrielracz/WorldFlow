#include "vine_generator.hpp"
#include "renderer.hpp"
#include "defines.hpp"
#include "path_config.hpp"

#include <functional>
#include <iostream>

namespace Constants {
    constexpr uint32_t MaxResolution = 2560 * 1440;
    constexpr uint32_t StagingBufferSize = MaxResolution * 4 * sizeof(float);

    constexpr uint32_t MaxKernelSize = 16*16;

    constexpr uint32_t LocalGroupSize = 8;
}

struct alignas(16) Kernel
{
    glm::uvec4 size;
    alignas(16) float weights[Constants::MaxKernelSize];
};

void generateGaussianKernel(Kernel& k, float sigma = 0.0) {
    if(sigma == 0.0) {sigma = k.size.x / 6.0;}
    float sum = 0.0f; 
    glm::uvec2 h = glm::uvec2(k.size) / 2U;
    for (int y = 0; y < k.size.y; y++) {
        for (int x = 0; x < k.size.x; x++) {
            int dx = x - h.x;
            int dy = y - h.y;
            float w = std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
            k.weights[y * k.size.x + x] = w;
            sum += w;
        }
    }
    for (int y = 0; y < k.size.y; y++) {
        for (int x = 0; x < k.size.x; x++) {
            k.weights[y * k.size.x + x] /= sum; // normalize
        }
    }
}

VineGenerator::VineGenerator(Renderer& renderer)
    : _renderer(renderer), _rng(1337) {}

bool
VineGenerator::Init()
{
    return initResources() &&
           initPipelines() &&
           initRendererOptions();
}

void
VineGenerator::update(VkCommandBuffer cmd, float dt)
{
    this->_imgVine[0].Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    this->_imgVine[1].Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    // if(this->_renderer.GetFrameNumber() % 60 == 0) 
        applyKernel(cmd);

    this->_imgVine[0].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    this->_imgVine[1].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Image::Copy(cmd, this->_imgVine[1], this->_renderer.GetDrawImage(), false);
    Image::Copy(cmd, this->_imgVine[1], this->_imgVine[0]);
}

void
VineGenerator::applyKernel(VkCommandBuffer cmd)
{
    this->_computeApplyKernel.Bind(cmd);
    const VkExtent2D imgSize = this->_renderer.GetWindowExtent2D();
    vkCmdDispatch(cmd, ceil(imgSize.width / Constants::LocalGroupSize), ceil(imgSize.height / Constants::LocalGroupSize), 1);
    VkImageMemoryBarrier barriers[] = {
        this->_imgVine[1].CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, ARRLEN(barriers), barriers);
}

bool
VineGenerator::initRendererOptions()
{
    this->_renderer.RegisterUpdateCallback(std::bind(&VineGenerator::update, this, std::placeholders::_1, std::placeholders::_2));
    return true;
}

bool
VineGenerator::initResources()
{
    for(int i = 0; i < ARRLEN(this->_imgVine); i++) {
        this->_renderer.CreateImage(
            this->_imgVine[i], this->_renderer.GetWindowExtent3D(),
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_LAYOUT_GENERAL
        );
    }

    this->_renderer.CreateBuffer(
        this->_buffStaging, Constants::StagingBufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU
    );

    this->_renderer.CreateBuffer(
        this->_buffKernel, sizeof(Kernel),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );
    Kernel kernel = {.size = glm::uvec4(5, 5, 0, 0)};
    generateGaussianKernel(kernel);
    float f[Constants::MaxKernelSize + 4] = {0.0};
    f[0] = kernel.size.x;
    f[1] = kernel.size.y;
    std::memcpy(f + 4, kernel.weights, Constants::MaxKernelSize * sizeof(float));
    this->_renderer.ImmediateSubmit([&f, kernel, this](VkCommandBuffer cmd) {
        for(int i = 0; i < ARRLEN(kernel.weights); i++) {
            std::cout << kernel.weights[i] << std::endl;
        }
        vkCmdUpdateBuffer(cmd, this->_buffKernel.bufferHandle, 0, sizeof(Kernel), &kernel);
        // vkCmdUpdateBuffer(cmd, this->_buffKernel.bufferHandle, 0, ARRLEN(f) * sizeof(float), f);
    });


    const uint32_t imgSize = this->_renderer.GetWindowExtent2D().width * this->_renderer.GetWindowExtent2D().height;
    float* data = (float*)this->_buffStaging.info.pMappedData;
    for(int i = 0; i < imgSize * 4; i += 4) {
        data[i]   = this->_rng.rand<float>(0.0, 1.0);
        data[i+1] = this->_rng.rand<float>(0.0, 1.0);
        data[i+2] = this->_rng.rand<float>(0.0, 1.0);
        data[i+3] = 1.0;
    }
    this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
        VkBufferImageCopy copy = {
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .layerCount = 1
            },
            .imageExtent = this->_renderer.GetWindowExtent3D()
        };
        vkCmdCopyBufferToImage(cmd, this->_buffStaging.bufferHandle, this->_imgVine[0].image, this->_imgVine[0].layout, 1, &copy);
    });
    return true;
}

bool
VineGenerator::initPipelines()
{
    this->_renderer.CreateComputePipeline(
        this->_computeApplyKernel, SHADER_DIRECTORY"/vine_apply_kernel.comp.spv", {
            ImageDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[0].imageView, VK_NULL_HANDLE, this->_imgVine[0].layout),
            ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[1].imageView, VK_NULL_HANDLE, this->_imgVine[1].layout),
            BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, this->_buffKernel.bufferHandle, sizeof(Kernel))
        }
    );
    return true;
}
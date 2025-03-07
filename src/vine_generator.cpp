#include "vine_generator.hpp"
#include "renderer.hpp"
#include "defines.hpp"
#include "path_config.hpp"

#include <functional>
#include <iostream>

namespace Constants {
    constexpr uint32_t MaxResolution = 2560 * 1440;
    constexpr uint32_t StagingBufferSize = MaxResolution * 4 * sizeof(float);
}

VineGenerator::VineGenerator(Renderer& renderer)
    : _renderer(renderer) {}

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
    std::cout << "update" << std::endl;
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
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_LAYOUT_GENERAL
        );
    }

    this->_renderer.CreateBuffer(
        this->_buffStaging, Constants::StagingBufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU | VMA_MEMORY_USAGE_GPU_TO_CPU
    );
    float* imgdata = (float*)this->_buffStaging.info.pMappedData;
    for(int i = 0; i < Constants::StagingBufferSize / sizeof(float); i++) {

    }
    return true;
}

bool
VineGenerator::initPipelines()
{
    this->_renderer.CreateComputePipeline(
        this->_computeApplyKernel, SHADER_DIRECTORY"/vine_apply_kernel.comp.spv", {
            ImageDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[0].imageView, VK_NULL_HANDLE, this->_imgVine[0].layout),
            ImageDescriptor(1, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[1].imageView, VK_NULL_HANDLE, this->_imgVine[1].layout),
            // ping-pong on second set
            ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[1].imageView, VK_NULL_HANDLE, this->_imgVine[1].layout),
            ImageDescriptor(1, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[0].imageView, VK_NULL_HANDLE, this->_imgVine[0].layout),
        }
    );
    return true;
}
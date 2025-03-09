#include "vine_generator.hpp"
#include "renderer.hpp"
#include "defines.hpp"
#include "path_config.hpp"

#include "imgui.h"
#include "ui_tools.hpp"

#include <functional>
#include <iostream>

#define HEX_TO_RGB(h) {((h&0xFF0000)>>16)/255.0f, ((h&0x00FF00)>>8)/255.0f, (h&0x0000FF)/255.0f, 1.0f}

namespace Constants {
    constexpr uint32_t MaxResolution = 2560 * 1440;
    constexpr uint32_t StagingBufferSize = MaxResolution * 4 * sizeof(float);

    constexpr uint32_t LocalGroupSize = 8;
}

enum InitalizerSrcTypes {
    Clear = 1,
    Random = 2
};

struct alignas(16) VineInitializerPushConstants
{
    glm::vec4 srcPos;
    uint32_t seed;
    int32_t srcType;
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
    : _renderer(renderer), _rng() {}

bool
VineGenerator::Init()
{
    return initResources() &&
           initPipelines() &&
           initPreProcess() &&
           initRendererOptions();
}

void
VineGenerator::update(VkCommandBuffer cmd, float dt)
{
    checkControls(this->_renderer.GetKeyMap(), this->_renderer.GetMouseMap(), this->_renderer.GetMouse());
    this->_imgVine[0].Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    this->_imgVine[1].Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);

    if(this->_shouldInitializeImage) {
        initializeImage(cmd);
    }

    if(this->_shouldGenerateKernel) {
        randomizeKernel(cmd);
    }

    if(this->_shouldStep)  {
        applyKernel(cmd);
    }


    this->_imgVine[0].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    this->_imgVine[1].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Image::Copy(cmd, this->_imgVine[1], this->_renderer.GetDrawImage(), false);
    Image::Copy(cmd, this->_imgVine[1], this->_imgVine[0]);
}

void
VineGenerator::applyKernel(VkCommandBuffer cmd)
{
    this->_computeApplyKernel.Bind(cmd);
    dispatchImage(cmd);
    VkImageMemoryBarrier barriers[] = {
        this->_imgVine[1].CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, ARRLEN(barriers), barriers);
}

void
VineGenerator::initializeImage(VkCommandBuffer cmd)
{
    this->_computeInitializeImage.Bind(cmd);
    VineInitializerPushConstants pc = {
        .seed = this->_rng.rand<uint32_t>(0, 1 << 24),
        .srcType = InitalizerSrcTypes::Random
    };
    vkCmdPushConstants(cmd, this->_computeInitializeImage.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    dispatchImage(cmd);
    VkImageMemoryBarrier barriers[] = { this->_imgVine[0].CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT) };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, ARRLEN(barriers), barriers);
}

void
VineGenerator::randomizeKernel(VkCommandBuffer cmd)
{
    for(int i = 0; i < this->_kernel.size.x * this->_kernel.size.y; i++) {
        this->_kernel.weights[i] = this->_rng.rand<float>(-1.0, 1.0);
    }
    vkCmdUpdateBuffer(cmd, this->_buffKernel.bufferHandle, 0, sizeof(Kernel), &this->_kernel);
}

void
VineGenerator::dispatchImage(VkCommandBuffer cmd)
{
    const VkExtent2D imgSize = this->_renderer.GetWindowExtent2D();
    vkCmdDispatch(cmd, ceil(imgSize.width / Constants::LocalGroupSize), ceil(imgSize.height / Constants::LocalGroupSize), 1);
}

void
VineGenerator::drawUI()
{
    if(this->_shouldHideUI) {
        return;
    }

	if(ImGui::Begin("controls", nullptr, ImGuiWindowFlags_NoTitleBar)) {
        ImGui::InputScalar("seed", ImGuiDataType_U32, &this->_rng.seed);
        this->_shouldInitializeImage =  ImGui::Button("reinitialize");
        this->_shouldGenerateKernel = ImGui::Button("new kernel");
    }
    ImGui::End();
}

void
VineGenerator::checkControls(KeyMap& keymap, MouseMap& mousemap, Mouse& mouse)
{
    if(keymap[SDLK_TAB]) {
        this->_shouldHideUI = !this->_shouldHideUI;
        keymap[SDLK_TAB] = false;
    }

    if(keymap[SDLK_SPACE]) {
        this->_shouldStep = !this->_shouldStep;
        keymap[SDLK_SPACE] = false;
    }
}

bool
VineGenerator::initRendererOptions()
{
    this->_renderer.RegisterUpdateCallback(std::bind(&VineGenerator::update, this, std::placeholders::_1, std::placeholders::_2));
    this->_renderer.RegisterUICallback(std::bind(&VineGenerator::drawUI, this));
    uitools::SetTheme(HEX_TO_RGB(0xcccccc), HEX_TO_RGB(0x1e1e1e), 0.5);
    std::cout << "RNGSEED: " << this->_rng.seed << std::endl;
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
    return true;
}

bool
VineGenerator::initPreProcess()
{
    this->_kernel.size = glm::uvec4(7, 7, 0, 0);
    this->_shouldGenerateKernel = true;
    this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
        randomizeKernel(cmd);
    });

    this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
        initializeImage(cmd);
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

    this->_renderer.CreateComputePipeline(
        this->_computeInitializeImage, SHADER_DIRECTORY"/vine_initialize.comp.spv", {
            ImageDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[0].imageView, VK_NULL_HANDLE, this->_imgVine[0].layout)
        }, sizeof(VineInitializerPushConstants)
    );
    return true;
}
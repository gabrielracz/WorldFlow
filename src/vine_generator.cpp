#include "vine_generator.hpp"
#include "renderer.hpp"
#include "defines.hpp"
#include "path_config.hpp"

#include <imgui.h>
#include "ui_tools.hpp"

#include <glm/gtx/string_cast.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <functional>
#include <iostream>

#define HEX_TO_RGB(h) {((h&0xFF0000)>>16)/255.0f, ((h&0x00FF00)>>8)/255.0f, (h&0x0000FF)/255.0f, 1.0f}

namespace Constants {
    constexpr uint32_t MaxResolution = 2560 * 1440;
    constexpr uint32_t StagingBufferSize = MaxResolution * 4 * sizeof(float);

    constexpr uint32_t LocalGroupSize = 8;
}

enum InitializerSrcTypes {
    None = 0,
    Clear = 1,
    Random = 2,
    Uniform = 3
};

struct alignas(16) VineInitializerPushConstants
{
    glm::ivec4 src;
    glm::vec4 srcColor;
    uint32_t seed;
};

enum ActivationFunctions {
    Identity = 0,
    Abs = 1,
    Pow2 = 2,
    ReLU = 3,
    Swish = 4,
    Tanh = 5,
    SoftPlus = 6,
    NumActivationFunctions
};

struct alignas(16) ApplyKernelPushConstants
{
    glm::vec4 colorMask;
    float kernelScale;
    float kernelContribution;
    int activationFn; 
    float activationParam;
};


void centerKernel(Kernel& kernel);

VineGenerator::VineGenerator(Renderer& renderer)
    : _renderer(renderer), _rng(), _srcType(InitializerSrcTypes::Random),
      _activationFunction(ActivationFunctions::Abs), _activationParam(2.0) {
    
    this->_kernels.resize(Constants::MaxNumKernels, Kernel{});
    this->_kernelScales.resize(Constants::MaxNumKernels, 1.0);
    this->_kernelContributions.resize(Constants::MaxNumKernels, 1.0);
    this->_activationFunctions.resize(Constants::MaxNumKernels, ActivationFunctions::Abs);
}

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


    if(this->_shouldRandomizeKernel) {
        randomizeKernel(this->_kernels[this->_selectedKernelIndex]);
    }
    
    // if(this->_shouldPushKernel) {
        // pushKernel(cmd, this->_kernels[this->_selectedKernelIndex]);
    // }

    if(this->_shouldInitializeImage) {
        addSource(cmd);
    }

    if(this->_shouldStep)  {
        dispatchKernel(cmd, 0);
    }


    int srcImgIndex = this->_numActivatedKernels % 2 == 0 && !this->_sequence ? 1 : 0;
    int dstImgIndex = (srcImgIndex + 1) % 2;
    
    this->_imgVine[dstImgIndex].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if(!this->_shouldSkipFrames || this->_renderer.GetFrameNumber() % (2) == 0 || !this->_shouldStep) {
        Image::Copy(cmd, this->_imgVine[dstImgIndex], this->_renderer.GetDrawImage(), false);
        this->_imgVine[srcImgIndex].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    } else {
        this->_imgVine[srcImgIndex].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Image::Copy(cmd, this->_imgVine[srcImgIndex], this->_renderer.GetDrawImage(), false);
        VkImageMemoryBarrier barrier = this->_imgVine[srcImgIndex].CreateBarrier(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &barrier);
        this->_imgVine[srcImgIndex].Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    Image::Copy(cmd, this->_imgVine[dstImgIndex], this->_imgVine[srcImgIndex]);
}

// void
// VineGenerator::applyKernels(VkCommandBuffer cmd)
// {
//     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computeApplyKernel.pipeline);
//     if(this->_sequence) {
//         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computeApplyKernel.layout, 0, 1, &this->_computeApplyKernel.descriptorSets[0], 0, nullptr);
//         dispatchKernel(cmd, this->_kernelSequence);
//         if((this->_renderer.GetFrameNumber() % this->_sequenceDelay) == 0 || this->_kernelSequence > 0) {
//             this->_kernelSequence = (this->_kernelSequence + 1) % this->_numActivatedKernels;
//         }
//     }
//     else {
//         for(int i = 0; i < this->_numActivatedKernels; i++) {
//             uint32_t pingPong = (i % 2);
//             vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computeApplyKernel.layout, 0, 1, &this->_computeApplyKernel.descriptorSets[pingPong], 0, nullptr);
//             dispatchKernel(cmd, i);
//         }
//     }
// }

void
VineGenerator::dispatchKernel(VkCommandBuffer cmd, int kernelIndex)
{
    this->_computeApplyKernel.Bind(cmd);
    pushKernel(cmd, this->_kernels[kernelIndex]);
    const ReactionDiffusionPushConstants& pc = this->_reactionDiffusion;
    vkCmdPushConstants(cmd, this->_computeApplyKernel.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    dispatchImage(cmd);
    VkImageMemoryBarrier barriers[] = {
        this->_imgVine[1].CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, ARRLEN(barriers), barriers);
}

void
VineGenerator::addSource(VkCommandBuffer cmd)
{
    this->_computeInitializeImage.Bind(cmd);
    VineInitializerPushConstants pc = {
        .src = glm::ivec4(this->_srcPos.x, this->_srcPos.y, this->_srcType, this->_srcRadius),
        .srcColor = this->_srcColor,
        .seed = this->_rng.rand<uint32_t>(0, 1 << 24),
    };
    vkCmdPushConstants(cmd, this->_computeInitializeImage.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    dispatchImage(cmd);
    VkImageMemoryBarrier barriers[] = { this->_imgVine[0].CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT) };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, ARRLEN(barriers), barriers);
}

void
VineGenerator::randomizeKernel(Kernel& k) {
    const uint32_t height = k.size.y;
    const uint32_t width = k.size.x;
    const size_t total_weights = static_cast<size_t>(width) * height;

    if (!this->_symmetricKernel) {
        for (size_t i = 0; i < total_weights; ++i) {
            k.weights[i] = this->_rng.rand<float>(-1.0f, 1.0f);
        }
    } else {
        // --- Octant Symmetric case ---
        // Iterate through the top-left octant defined by:
        // row `y` from 0 up to center row
        // column `x` from `y` up to center column (ensures y <= x)
        const uint32_t yLim = (height + 1) / 2; // Iterate up to center row (inclusive if odd)
        const uint32_t xLim = (width + 1) / 2;  // Iterate up to center column (inclusive if odd)
        const size_t W_s = static_cast<size_t>(width); // Use size_t width for index calculations

        for (uint32_t y = 0; y < yLim; ++y) {
            // Start x from y to ensure we are in the octant where y <= x
            for (uint32_t x = y; x < xLim; ++x) {
                // Generate one random weight for the group of up to 8 symmetric points
                const float weight = this->_rng.rand<float>(-1.0f, 1.0f);

                // Calculate reflected coordinates needed for indexing
                // (unsigned subtraction is safe here as width/height > 0 and x/y < Lim)
                const uint32_t reflected_y = height - 1 - y;
                const uint32_t reflected_x = width - 1 - x;

                // --- Assign weight to the 8 symmetric locations ---
                // Index is calculated as: row_index * width + column_index

                // Group 1: Based on (x, y)
                // P1: (x, y) -> Row y, Col x
                k.weights[static_cast<size_t>(y) * W_s + x] = weight;
                // P2: (W-1-x, y) -> Row y, Col W-1-x
                k.weights[static_cast<size_t>(y) * W_s + reflected_x] = weight;
                // P3: (x, H-1-y) -> Row H-1-y, Col x
                k.weights[static_cast<size_t>(reflected_y) * W_s + x] = weight;
                // P4: (W-1-x, H-1-y) -> Row H-1-y, Col W-1-x
                k.weights[static_cast<size_t>(reflected_y) * W_s + reflected_x] = weight;

                // Group 2: Based on diagonal reflection (y, x)
                // Avoid redundant writes if x == y (on the main diagonal)
                // These points might be identical to Group 1 points if x == y.
                // Assigning again is harmless but checking avoids redundant memory writes.
                if (x != y) {
                    // P5: (y, x) -> Row x, Col y
                    k.weights[static_cast<size_t>(x) * W_s + y] = weight;
                    // P6: (W-1-y, x) -> Row x, Col W-1-y
                    k.weights[static_cast<size_t>(x) * W_s + (width - 1 - y)] = weight;
                     // P7: (y, H-1-x) -> Row H-1-x, Col y
                    k.weights[static_cast<size_t>(height - 1 - x) * W_s + y] = weight;
                    // P8: (W-1-y, H-1-x) -> Row H-1-x, Col W-1-y
                    k.weights[static_cast<size_t>(height - 1 - x) * W_s + (width - 1 - y)] = weight;
                }
            }
        }
    }
}

void
VineGenerator::pushKernel(VkCommandBuffer cmd, Kernel& k)
{
    vkCmdUpdateBuffer(cmd, this->_buffKernel.bufferHandle, 0, sizeof(Kernel), &k);
    // VkBufferMemoryBarrier barrier = this->_buffKernel.CreateBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    VkAccessFlags allAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkBufferMemoryBarrier barrier = this->_buffKernel.CreateBarrier(allAccess, allAccess);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 1, &barrier, 0, 0);
}

void
VineGenerator::dispatchImage(VkCommandBuffer cmd)
{
    const VkExtent2D imgSize = this->_renderer.GetWindowExtent2D();
    vkCmdDispatch(cmd, ceil(imgSize.width / Constants::LocalGroupSize) + 1, ceil(imgSize.height / Constants::LocalGroupSize) + 1, 1);
}

void
VineGenerator::drawUI()
{
    if(this->_shouldHideUI) {
        return;
    }

	if(ImGui::Begin("controls", nullptr, ImGuiWindowFlags_NoTitleBar)) {
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &this->_rng.seed);
        if(ImGui::Button("Reinitialize")) {
            this->_shouldInitializeImage = true;
            this->_srcType = InitializerSrcTypes::Random;
            this->_srcRadius = 0;
        } else {
            this->_shouldInitializeImage = false;
        }
        
        float dragSpeed = 0.001;
        ImGui::DragFloat("Time", &this->_reactionDiffusion.dt, dragSpeed);
        ImGui::DragFloat("Da", &this->_reactionDiffusion.Da, dragSpeed);
        ImGui::DragFloat("Db", &this->_reactionDiffusion.Db, dragSpeed);
        ImGui::DragFloat("s", &this->_reactionDiffusion.s, dragSpeed);
        ImGui::DragFloat("Beta", &this->_reactionDiffusion.Beta, dragSpeed);
        ImGui::DragFloat("Reaction Param", &this->_reactionDiffusion.reactionParam, dragSpeed);

        ImGui::Separator();
        ImGui::ColorPicker4("Color", glm::value_ptr(this->_srcColor));
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

    if(mousemap[SDL_BUTTON_LEFT] || mousemap[SDL_BUTTON_RIGHT]) {
        this->_srcPos = glm::ivec2(mouse.prev);
        this->_srcType = mousemap[SDL_BUTTON_RIGHT] ? InitializerSrcTypes::Uniform : InitializerSrcTypes::Clear;
        this->_shouldInitializeImage = true;
        this->_srcRadius = std::max(10 + (int)std::round(mouse.scroll * 0.5), 1);
    } else if(this->_shouldHideUI){
        this->_shouldInitializeImage = false;
    }

    if(keymap[SDLK_q]) {
        this->_shouldInitializeImage = true;
        this->_srcType = InitializerSrcTypes::Clear;
        this->_srcRadius = 0;
        this->_kernelSequence = 0;
        keymap[SDLK_q] = false;
    }

    if(keymap[SDLK_w]) {
        this->_shouldInitializeImage = true;
        this->_srcType = InitializerSrcTypes::Random;
        this->_srcRadius = 0;
        keymap[SDLK_w] = false;
    }
    if(keymap[SDLK_r]) {
        this->_shouldRandomizeKernel = true;
        keymap[SDLK_r] = false;
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

    this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
        this->_imgVine[0].Clear(cmd);
        this->_imgVine[0].Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    });

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
    this->_kernels[0].size = glm::uvec4(3, 3, 0, 0);
    randomizeKernel(this->_kernels[0]);
    this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
        pushKernel(cmd, this->_kernels[0]);
    });

    this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
        addSource(cmd);
    });
    return true;
}

bool
VineGenerator::initPipelines()
{
    this->_renderer.CreateComputePipeline(
        this->_computeApplyKernel, SHADER_DIRECTORY"/vine_apply_kernel_reaction_diffusion.comp.spv", {
            ImageDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[0].imageView, VK_NULL_HANDLE, this->_imgVine[0].layout),
            ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[1].imageView, VK_NULL_HANDLE, this->_imgVine[1].layout),
        }, sizeof(ReactionDiffusionPushConstants)
    );

    this->_renderer.CreateComputePipeline(
        this->_computeInitializeImage, SHADER_DIRECTORY"/vine_initialize.comp.spv", {
            ImageDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_imgVine[0].imageView, VK_NULL_HANDLE, this->_imgVine[0].layout)
        }, sizeof(VineInitializerPushConstants)
    );
    return true;
}

void
centerKernel(Kernel& kernel)
{
    const uint32_t width  = kernel.size.x;
    const uint32_t height = kernel.size.y;
    const uint32_t maxSize = width * height;

    // Compute the centroid (center of mass)
    float sumWeights = 0.0f;
    float centroidX = 0.0f;
    float centroidY = 0.0f;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float w = kernel.weights[y * width + x];
            centroidX += x * w;
            centroidY += y * w;
            sumWeights += w;
        }
    }

    if (sumWeights == 0.0f) return; // Avoid division by zero

    centroidX /= sumWeights;
    centroidY /= sumWeights;

    // Target center position
    float targetX = (width - 1) / 2.0f;
    float targetY = (height - 1) / 2.0f;

    // Compute shift needed
    float dx = targetX - centroidX;
    float dy = targetY - centroidY;

    float newWeights[Constants::MaxKernelSize] = {0.0f};

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            // Compute source coordinates (floating point)
            float srcX = x - dx;
            float srcY = y - dy;

            // Get integer parts and fractional remainder
            int x0 = static_cast<int>(srcX);
            int y0 = static_cast<int>(srcY);
            float wx = srcX - x0;
            float wy = srcY - y0;

            // Clamp indices to valid range
            x0 = std::clamp(x0, 0, static_cast<int>(width) - 1);
            y0 = std::clamp(y0, 0, static_cast<int>(height) - 1);
            int x1 = std::clamp(x0 + 1, 0, static_cast<int>(width) - 1);
            int y1 = std::clamp(y0 + 1, 0, static_cast<int>(height) - 1);

            // Read kernel values with bilinear interpolation
            float v00 = kernel.weights[y0 * width + x0];
            float v10 = kernel.weights[y0 * width + x1];
            float v01 = kernel.weights[y1 * width + x0];
            float v11 = kernel.weights[y1 * width + x1];

            // Bilinear interpolation formula
            newWeights[y * width + x] =
                (1 - wx) * (1 - wy) * v00 +
                wx * (1 - wy) * v10 +
                (1 - wx) * wy * v01 +
                wx * wy * v11;
        }
    }

    // Copy shifted weights back to the kernel
    std::copy(newWeights, newWeights + maxSize, kernel.weights);
}
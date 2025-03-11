#ifndef VINE_GENERATOR_HPP_
#define VINE_GENERATOR_HPP_

#include "image.hpp"
#include "buffer.hpp"
#include "renderer_structs.hpp"
#include "random_generator.hpp"

namespace Constants
{
    constexpr uint32_t MaxKernelSize = 64*64;
}


struct alignas(16) Kernel
{
    glm::uvec4 size {0};
    alignas(16) float weights[Constants::MaxKernelSize] = {0.0};
};

class Renderer;

class VineGenerator
{
public:
    VineGenerator(Renderer &renderer);
    bool Init();

private:
    void update(VkCommandBuffer cmd, float dt);
    void applyKernel(VkCommandBuffer cmd);
    void addSource(VkCommandBuffer cmd);
    void randomizeKernel(Kernel& k);
    void pushKernel(VkCommandBuffer cmd, Kernel& k);

    void dispatchImage(VkCommandBuffer cmd);
    
    void checkControls(KeyMap& keymap, MouseMap& mousemap, Mouse& mouse);
    void drawUI();

    bool initRendererOptions();
    bool initResources();
    bool initPipelines();
    bool initPreProcess();

private:
    Renderer& _renderer;
    RandomGenerator _rng;

    bool _shouldHideUI {false};
    bool _shouldStep {true};
    bool _shouldInitializeImage {false};
    bool _shouldRandomizeKernel {false};
    float _kernelScale {1.0};
    bool _shouldPushKernel {false};
    bool _shouldSkipFrames {false};
    bool _symmetricKernel {false};
    glm::ivec2 _srcPos {};
    glm::vec4 _srcColor {};
    int _srcType {};
    int _srcRadius {};
    int _activationFunction {};
    float _activationParam {};
    Kernel _kernel {};

    ComputePipeline _computeApplyKernel;
    ComputePipeline _computeInitializeImage;

    Image _imgVine[2];
    Buffer _buffStaging;
    Buffer _buffKernel;
};

#endif
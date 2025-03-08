#ifndef VINE_GENERATOR_HPP_
#define VINE_GENERATOR_HPP_

#include "image.hpp"
#include "buffer.hpp"
#include "renderer_structs.hpp"
#include "random_generator.hpp"

class Renderer;

class VineGenerator
{
public:
    VineGenerator(Renderer &renderer);
    bool Init();

private:
    void update(VkCommandBuffer cmd, float dt);
    void applyKernel(VkCommandBuffer cmd);

    void checkControls(KeyMap& keymap, MouseMap& mousemap, Mouse& mouse);

    bool initRendererOptions();
    bool initResources();
    bool initPipelines();

private:
    Renderer& _renderer;
    RandomGenerator _rng;

    bool _shouldStep {true};

    ComputePipeline _computeApplyKernel;

    Image _imgVine[2];
    Buffer _buffStaging;
    Buffer _buffKernel;
};

#endif
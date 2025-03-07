#ifndef VINE_GENERATOR_HPP_
#define VINE_GENERATOR_HPP_

#include "image.hpp"
#include "buffer.hpp"
#include "renderer_structs.hpp"

class Renderer;

class VineGenerator
{
public:
    VineGenerator(Renderer &renderer);
    bool Init();

private:
    void update(VkCommandBuffer cmd, float dt);

    bool initRendererOptions();
    bool initResources();
    bool initPipelines();

private:
    Renderer& _renderer;

    ComputePipeline _computeApplyKernel;

    Image _imgVine[2];
    Buffer _buffStaging;
};

#endif
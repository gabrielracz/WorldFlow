#ifndef RENDERER_STRUCTS_HPP_
#define RENDERER_STRUCTS_HPP_

#include <SDL2/SDL_keycode.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <deque>
#include <functional>

typedef std::unordered_map<SDL_Keycode, bool> KeyMap;
typedef std::unordered_map<int, bool> MouseMap;
struct Mouse 
{
    bool first_captured = false;
    bool captured = true;
    float scroll = 0.0;
    glm::vec2 prev {};
    glm::vec2 move {};
};

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

enum class BlendMode {
    None,
    Additive,
    AlphaBlend
};

struct GraphicsPipelineOptions
{
    VkPrimitiveTopology inputTopology         = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode       polygonMode           = VK_POLYGON_MODE_FILL;
    VkCullModeFlags     cullMode              = VK_CULL_MODE_NONE;
    VkFrontFace         frontFace             = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    BlendMode           blendMode             = BlendMode::AlphaBlend;
    VkCompareOp         depthTestOp           = VK_COMPARE_OP_LESS;
    VkFormat            colorAttachmentFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkFormat            depthFormat           = VK_FORMAT_D32_SFLOAT;
    VkShaderStageFlags  pushConstantsStages   = VK_SHADER_STAGE_VERTEX_BIT;
    float               lineWidth             = 1.0;
    bool depthTestEnabled 					  = false;
};

struct BufferDescriptor
{
    unsigned int set;
    unsigned int binding;
    VkDescriptorType type;
    VkBuffer handle;
    size_t size;
    unsigned int offset;
    BufferDescriptor(unsigned int set, unsigned int binding, VkDescriptorType type, VkBuffer handle, size_t size, unsigned int offset = 0)
        : set(set), binding(binding), type(type), handle(handle), size(size), offset(offset) {}
};

struct ImageDescriptor
{
    unsigned int set;
    unsigned int binding;
    VkDescriptorType type;
    VkImageView imageView;
    VkSampler sampler;
    VkImageLayout layout;
    ImageDescriptor(unsigned int set, unsigned int binding, VkDescriptorType type, VkImageView view, VkSampler sampler, VkImageLayout layout)
        : set(set), binding(binding), type(type), imageView(view), sampler(sampler), layout(layout) {}
};


#endif
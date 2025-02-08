#ifndef RENDERER_STRUCTS_HPP_
#define RENDERER_STRUCTS_HPP_

#include <SDL2/SDL_keycode.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <iostream>
#include <deque>
#include <functional>
#include <vulkan/vulkan_core.h>

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

	void Bind(VkCommandBuffer cmd)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->layout, 0, this->descriptorSets.size(), this->descriptorSets.data(), 0, nullptr);
	}
};

struct GraphicsPipeline
{
    VkPipeline pipeline {};
    VkPipelineLayout layout {};
    std::vector<VkDescriptorSet> descriptorSets {};
    VkDescriptorSetLayout descriptorLayout {};
	
	void Bind(VkCommandBuffer cmd)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->layout, 0, this->descriptorSets.size(), this->descriptorSets.data(), 0, nullptr);
	}
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

struct TimestampQueryPool
{
    VkQueryPool queryPool;
    uint32_t queryCount;
    std::vector<uint64_t> resultsWithAvailability;
    std::vector<uint64_t> results;
    uint32_t currentFrame;
    uint32_t framesInFlight;
    float timestampPeriod = 1; //nanoseconds

    void init(VkDevice device, uint32_t maxQueries, uint32_t framesInFlight)
    {
        this->queryCount = maxQueries;
        this->results.resize(maxQueries, 0);
        this->resultsWithAvailability.resize(maxQueries * framesInFlight * 2, 1);
        this->currentFrame = 0;
        this->framesInFlight = framesInFlight;
        VkQueryPoolCreateInfo qpoolInfo = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = maxQueries * framesInFlight,
        };
        vkCreateQueryPool(device, &qpoolInfo, nullptr, &this->queryPool);
        vkResetQueryPool(device, this->queryPool, 0, maxQueries * framesInFlight);
    }

    void reset(VkDevice device) {
        uint32_t firstQuery = this->currentFrame * this->queryCount;
        vkResetQueryPool(device, this->queryPool, firstQuery, this->queryCount);
    }

    void write(VkCommandBuffer cmd, uint32_t queryIndex, VkPipelineStageFlagBits pipelineStage) {
        uint32_t actualQuery = this->currentFrame * this->queryCount + queryIndex;
        // if(resultsWithAvailability[this->currentFrame * this->queryCount + queryIndex + 1] != 0) {
            vkCmdWriteTimestamp(cmd, pipelineStage, this->queryPool, actualQuery);
        // }
    }

    void collect(VkDevice device) {
        uint32_t firstQuery = this->currentFrame * this->queryCount;
        
        // Each query result will be followed by its availability value
        
        VkResult result = vkGetQueryPoolResults(
            device, 
            this->queryPool,
            0,
            this->queryCount,
            this->resultsWithAvailability.size() * sizeof(uint64_t),
            this->resultsWithAvailability.data() + (firstQuery * 2),
            sizeof(uint64_t) * 2,  // Stride includes both timestamp and availability
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        
        for(int i = 0; i < queryCount; i++) {
            uint32_t qix = firstQuery * 2 + i * 2;
            if(resultsWithAvailability[qix + 1] != 0) {
                results[i] = resultsWithAvailability[qix];
            }
        }
    }

    void nextFrame() {
        this->currentFrame = (this->currentFrame + 1) % this->framesInFlight;
    }

    float getDelta(uint32_t startIndex, uint32_t endIndex) {
        uint32_t prevFrame = (this->currentFrame == 0) ? this->framesInFlight - 1 : this->currentFrame - 1;
        uint32_t baseQuery = prevFrame * this->queryCount * 2;

        // uint64_t startTime = this->resultsWithAvailability[baseQuery + (startIndex * 2)];
        // uint64_t endTime = this->resultsWithAvailability[baseQuery + (endIndex * 2)];

        uint64_t startTime = this->results[startIndex];
        uint64_t endTime = this->results[endIndex];

        // Convert to milliseconds for readability
        float deltaMs = (float)(endTime - startTime) / 1000000.0f;
        return deltaMs;
    }
};

#endif
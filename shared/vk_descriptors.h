﻿#pragma once

#include <vector>
#include <vk_types.h>
#include <deque>
#include <span>

//> descriptor_layout
struct DescriptorLayoutBuilder {

    std::vector<VkDescriptorSetLayoutBinding> bindings;

    static DescriptorLayoutBuilder newLayout();
    DescriptorLayoutBuilder& add_binding(uint32_t binding, VkDescriptorType type);
    DescriptorLayoutBuilder& clear();
    VkDescriptorSetLayout build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0);
};
//< descriptor_layout
// 
//> writer
struct DescriptorWriter {
    std::deque<VkDescriptorImageInfo> imageInfos;
    std::deque<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkWriteDescriptorSet> writes;

    DescriptorWriter& write_image(int binding,VkImageView imageView,VkSampler sampler , VkImageLayout layout, VkDescriptorType type);
    DescriptorWriter& write_buffer(int binding,VkBuffer buffer,size_t size, size_t offset,VkDescriptorType type); 

    DescriptorWriter& clear();
    DescriptorWriter& update_set(VkDevice device, VkDescriptorSet set);
};
//< writer
// 
//> descriptor_allocator
struct DescriptorPool {

    struct DescriptorQuantity {
		VkDescriptorType type;
		float count;
    };

    VkDescriptorPool pool;

    void init(VkDevice device, uint32_t maxSets, std::span<DescriptorQuantity> descriptors);
    void clear(VkDevice device);
    void destroy(VkDevice device);

    VkDescriptorSet allocateSet(VkDevice device, VkDescriptorSetLayout layout);
};
//< descriptor_allocator

//> descriptor_allocator_grow
struct DescriptorAllocatorGrowable {
public:
	struct PoolSizeRatio {
		VkDescriptorType type;
		float ratio;
	};

	void init(VkDevice device, uint32_t initialSets, std::span<PoolSizeRatio> poolRatios);
	void clear_pools(VkDevice device);
	void destroy_pools(VkDevice device);

    VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout, void* pNext = nullptr);
private:
	VkDescriptorPool get_pool(VkDevice device);
	VkDescriptorPool create_pool(VkDevice device, uint32_t setCount, std::span<PoolSizeRatio> poolRatios);

	std::vector<PoolSizeRatio> ratios;
	std::vector<VkDescriptorPool> fullPools;
	std::vector<VkDescriptorPool> readyPools;
	uint32_t setsPerPool;

};
//< descriptor_allocator_grow
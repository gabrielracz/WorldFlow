#ifndef IMAGE_HPP_
#define IMAGE_HPP_

#include <vulkan/vulkan_core.h>
// #include "vk_mem_alloc.h"
#include "vma.hpp"

class Image
{
public:
	void Transition(VkCommandBuffer cmd, VkImageLayout newLayout);
	void Clear(VkCommandBuffer cmd, VkClearColorValue color = {0.0, 0.0, 0.0, 0.0});

    VkImage image {};
    VkImageView imageView {};
    VmaAllocation allocation {};
    VkExtent3D imageExtent {};
    VkFormat imageFormat {};
    VmaAllocationInfo info {};
	VkImageLayout layout {};
};

#endif
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
    VkImageMemoryBarrier CreateBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess);
    VkImageMemoryBarrier2 CreateBarrier2(VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage, VkAccessFlags srcAccess, VkAccessFlags dstAccess);

    static void Copy(VkCommandBuffer cmd, Image src, Image dst, bool stretch = true);

    VkImage image {};
    VkImageView imageView {};
    VmaAllocation allocation {};
    VkExtent3D imageExtent {};
    VkFormat imageFormat {};
    VmaAllocationInfo info {};
	VkImageLayout layout {};
    VkImageAspectFlags aspectMask {VK_IMAGE_ASPECT_COLOR_BIT};
};

#endif
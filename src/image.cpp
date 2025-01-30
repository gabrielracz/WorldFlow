#include "image.hpp"
#include "vk_initializers.h"

void
Image::Transition(VkCommandBuffer cmd, VkImageLayout newLayout)
{
	if(newLayout == layout) {
		return;
	}

    VkImageMemoryBarrier2 imageBarrier {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    imageBarrier.pNext = nullptr;

    imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

    imageBarrier.oldLayout = this->layout;
    imageBarrier.newLayout = newLayout;

    VkImageAspectFlags aspectMask = (newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange = vkinit::image_subresource_range(aspectMask);
    imageBarrier.image = image;

    VkDependencyInfo depInfo {};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.pNext = nullptr;

    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &imageBarrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);
	this->layout = newLayout;
}

void 
Image::Clear(VkCommandBuffer cmd, VkClearColorValue color)
{
	VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
	Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdClearColorImage(cmd, this->image, this->layout, &color, 1, &clearRange);
}
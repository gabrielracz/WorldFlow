#include "image.hpp"
#include "vk_initializers.h"
#include <vulkan/vulkan_core.h>

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

    VkImageAspectFlags aspectMask = this->aspectMask;
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
	if(this->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
		VkImageSubresourceRange clearRange = vkinit::image_subresource_range(aspectMask);
		Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearDepthStencilValue depthVal = {0.0f, 0};
		vkCmdClearDepthStencilImage(cmd, this->image, this->layout, &depthVal, 1, &clearRange);
	} else {
		VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
		Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdClearColorImage(cmd, this->image, this->layout, &color, 1, &clearRange);
	}
}

void
Image::Copy(VkCommandBuffer cmd, Image src, Image dst, bool stretch, VkExtent3D srcExtent, VkExtent3D dstExtent)
{
	if(srcExtent.width == 0) srcExtent = src.imageExtent;
	if(dstExtent.width == 0) dstExtent = dst.imageExtent;

	VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr };
	blitRegion.srcOffsets[1].x = srcExtent.width;
	blitRegion.srcOffsets[1].y = srcExtent.height;
	blitRegion.srcOffsets[1].z = 1;
    
    if(stretch) {
        blitRegion.dstOffsets[1].x = dstExtent.width;
        blitRegion.dstOffsets[1].y = dstExtent.height;
        blitRegion.dstOffsets[1].z = 1;
    } else {
        blitRegion.dstOffsets[1].x = srcExtent.width;
        blitRegion.dstOffsets[1].y = srcExtent.height;
        blitRegion.dstOffsets[1].z = 1;
    }

	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.baseArrayLayer = 0;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcSubresource.mipLevel = 0;

	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.baseArrayLayer = 0;
	blitRegion.dstSubresource.layerCount = 1;
	blitRegion.dstSubresource.mipLevel = 0;

	VkBlitImageInfo2 blitInfo{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr };
	blitInfo.dstImage = dst.image;
	blitInfo.dstImageLayout = dst.layout;
	blitInfo.srcImage = src.image;
	blitInfo.srcImageLayout = src.layout;
	blitInfo.filter = VK_FILTER_NEAREST;
	blitInfo.regionCount = 1;
	blitInfo.pRegions = &blitRegion;

	vkCmdBlitImage2(cmd, &blitInfo);

    VkImageMemoryBarrier2 barrier = dst.CreateBarrier2(VK_PIPELINE_STAGE_2_BLIT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    VkDependencyInfo depInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier
    };
    vkCmdPipelineBarrier2(cmd, &depInfo);
    // VkImageMemoryBarrier barrier = dst.CreateBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    // vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 0, 1, &barrier);
}

VkImageMemoryBarrier
Image::CreateBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    return VkImageMemoryBarrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = this->layout,
        .newLayout = this->layout,
        .image = this->image,
        .subresourceRange = {
            .aspectMask = this->aspectMask,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
}

VkImageMemoryBarrier2
Image::CreateBarrier2(VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage, VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    return VkImageMemoryBarrier2 {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = this->layout,
        .newLayout = this->layout,
        .image = this->image,
        .subresourceRange = {
            .aspectMask = this->aspectMask,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
}
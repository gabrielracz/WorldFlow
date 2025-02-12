#include "buffer.hpp"
#include "defines.hpp"

void
Buffer::Allocate(VmaAllocator allocator, uint64_t size, VkBufferUsageFlags bufferUsage, VmaMemoryUsage memoryUsage)
{
	VkBufferCreateInfo bufferInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = bufferUsage
	};
	VmaAllocationCreateInfo vmaAllocInfo = {
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT, // ignored if not host-visible
		.usage = memoryUsage,
	};

	VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &vmaAllocInfo, &this->bufferHandle, &this->allocation, &this->info));
}

void
Buffer::Destroy(VmaAllocator allocator)
{
	vmaDestroyBuffer(allocator, this->bufferHandle, this->allocation);
}

VkBufferMemoryBarrier
Buffer::CreateBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
	return {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = srcAccess,
		.dstAccessMask = dstAccess,
		.buffer = this->bufferHandle,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
	};
}

void
Mesh::Destroy(VmaAllocator allocator)
{
	this->vertexBuffer.Destroy(allocator);
	this->indexBuffer.Destroy(allocator);
}
#ifndef BUFFER_HPP_
#define BUFFER_HPP_

#include <vulkan/vulkan.h>
// #include "vk_mem_alloc.h"
#include "vma.hpp"
#include <vulkan/vulkan_core.h>

class Buffer
{
public:
	void Allocate(VmaAllocator allocator, uint64_t size, VkBufferUsageFlags bufferUsage, VmaMemoryUsage memoryUsage);
	void Destroy(VmaAllocator allocator);
	VkBufferMemoryBarrier CreateBarrier(VkAccessFlags srcAccess = VK_ACCESS_SHADER_WRITE_BIT, VkAccessFlags dstAccess = VK_ACCESS_SHADER_READ_BIT);

	VkBuffer bufferHandle {};
	VmaAllocation allocation {};
	VmaAllocationInfo info {};
};

struct GPUMesh
{
    Buffer indexBuffer;
    Buffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress;
    VkDeviceAddress indexBufferAddress {};
    size_t numIndices;
    size_t numVertices;

	void Destroy(VmaAllocator allocator);
};

#endif
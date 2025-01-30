#include "fluid_engine.hpp"
#include "fluid_engine_structs.hpp"
#include "renderer.hpp"
#include "path_config.hpp"
#include "vk_loader.h"

// #define VMA_IMPLEMENTATION
// #include "vk_mem_alloc.h"
#include "vma.hpp"
#include <functional>

namespace Constants
{
constexpr uint64_t StagingBufferSize = 1024ul * 1024ul * 8ul;

constexpr size_t VoxelGridResolution = 256;
constexpr size_t VoxelGridSize = VoxelGridResolution * VoxelGridResolution * VoxelGridResolution * sizeof(float);
constexpr float VoxelGridScale = 2.0f;

constexpr uint32_t MeshIdx = 2;
// constexpr float MeshScale = 0.01;
// constexpr float MeshScale = 0.0007;
constexpr float MeshScale = 0.60;
constexpr glm::vec3 MeshTranslation = glm::vec3(0.0, 0.0, 0.0);
const glm::mat4 MeshTransform = glm::translate(MeshTranslation) * glm::scale(glm::vec3(MeshScale));
const std::string MeshFile = ASSETS_DIRECTORY"/basicmesh.glb";

constexpr glm::vec3 LightPosition = glm::vec4(10.0, 10.0, 10.0, 1.0);

constexpr uint32_t NumAllocatedVoxelFragments = 1024 * 1024;
constexpr uint32_t NumAllocatedNodes = 256 * 256 * 256 * 2;
constexpr uint32_t MaxTreeDepth = 2;
constexpr uint32_t NodeDivisions = 2;
constexpr uint32_t NodeChildren = NodeDivisions * NodeDivisions * NodeDivisions;
}

FluidEngine::FluidEngine(Renderer& renderer)
	: _renderer(renderer), _testMeshes(10) {}

FluidEngine::~FluidEngine() {
}

bool FluidEngine::Init()
{
	const bool initSuccess = initResources() &&
							 initPipelines();
	if(!initSuccess) {
		return false;
	}

	this->_renderer.RegisterUpdateCallback(std::bind(&FluidEngine::update, this, std::placeholders::_1, std::placeholders::_2));
	return true;
}

void FluidEngine::update(VkCommandBuffer cmd, float dt)
{
	// updateVoxelVolume(cmd); // fill voxels with sample noise
	// // voxelRasterizeGeometry(cmd);

	// generateTreeIndirectCommands(cmd);
	// generateTreeGeometry(cmd);
	// drawLines(cmd);
	// if(this->_shouldSubdivide) {
	// 	flagNodesForSubdivision(cmd);
	// 	subdivideTree(cmd);
	// 	this->_shouldSubdivide = false;
	// }

	// if(this->_shouldRenderGeometry) {
	// 	drawGeometry(cmd, dt);
	// }

	// if(this->_shouldRenderVoxels) {
	// 	rayCastVoxelVolume(cmd);
	// }
	std::cout << "update" << std::endl;
}

bool FluidEngine::initResources()
{
	this->_renderer.CreateImage(this->_voxelImage,
		VkExtent3D{Constants::VoxelGridResolution, Constants::VoxelGridResolution, 1},
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_LAYOUT_GENERAL
	);
	this->_renderer.ImmediateSubmit([&](VkCommandBuffer cmd) {
		this->_voxelImage.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	});

	// BUFFERS
	this->_renderer.CreateBuffer(this->_stagingBuffer, Constants::StagingBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	this->_renderer.CreateBuffer(this->_voxelVolume, Constants::VoxelGridSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	this->_renderer.ImmediateSubmit([&](VkCommandBuffer cmd) {
		vkCmdFillBuffer(cmd, this->_voxelVolume.bufferHandle, 0, VK_WHOLE_SIZE, 0);
	});
	this->_renderer.CreateBuffer(this->_voxelInfoBuffer, sizeof(VoxelInfo), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	VoxelInfo voxInfo = {
		.gridDimensions = glm::vec3(Constants::VoxelGridResolution),
		.gridScale = Constants::VoxelGridScale
	};
	void* data = this->_stagingBuffer.info.pMappedData;
	std::memcpy(data, &voxInfo, sizeof(voxInfo));
	this->_renderer.ImmediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy = {
			.srcOffset = 0,
			.dstOffset = 0,
			.size = sizeof(VoxelInfo)
		};
		vkCmdCopyBuffer(cmd, this->_stagingBuffer.bufferHandle, this->_voxelInfoBuffer.bufferHandle, 1, &copy);
	});
	std::cout << "VOXBUFFMEM: " << this->_voxelVolume.info.size << std::endl;

	this->_renderer.CreateBuffer(
		this->_voxelFragmentCounter,
		sizeof(VoxelFragmentCounter),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_CPU_ONLY
	);

	this->_renderer.CreateBuffer(
		this->_voxelFragmentList,
		Constants::NumAllocatedVoxelFragments * sizeof(VoxelFragment),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	this->_renderer.CreateBuffer(
		this->_treeVertices,
		Constants::NumAllocatedNodes * 8 * sizeof(glm::vec3), // 8 vertices per cube
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	VkBufferDeviceAddressInfo deviceAddressInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = this->_treeVertices.bufferHandle
	};
	this->_treeMesh.vertexBufferAddress = vkGetBufferDeviceAddress(this->_renderer.GetDevice(), &deviceAddressInfo);

	this->_renderer.CreateBuffer(
		this->_treeIndices,
		Constants::NumAllocatedNodes * 12 * 2 * sizeof(uint32_t), // 12 lines per cube, 2 indices per line
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	deviceAddressInfo.buffer = this->_treeIndices.bufferHandle;
	this->_treeMesh.indexBufferAddress = vkGetBufferDeviceAddress(this->_renderer.GetDevice(), &deviceAddressInfo);

	this->_renderer.CreateBuffer(
		this->_treeIndirectDrawBuffer,
		sizeof(VkDrawIndexedIndirectCommand),
		VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	this->_renderer.CreateBuffer(this->_treeIndirectDispatchBuffer,
		sizeof(VkDispatchIndirectCommand),
		VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	this->_renderer.CreateBuffer(this->_treeFlaggerIndirectDispatchBuffer,
		sizeof(VkDispatchIndirectCommand),
		VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	this->_renderer.CreateBuffer(this->_treeInfoBuffer,
		sizeof(TreeInfo),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
		VMA_MEMORY_USAGE_CPU_ONLY
	);
	TreeInfo treeInfo = {.nodeCounter = Constants::NodeChildren}; // init base level
	std::memcpy(this->_stagingBuffer.info.pMappedData, &treeInfo, sizeof(TreeInfo));
	this->_renderer.ImmediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy = {.size = sizeof(TreeInfo)};
		vkCmdCopyBuffer(cmd, this->_stagingBuffer.bufferHandle, this->_treeInfoBuffer.bufferHandle, 1, &copy);
	});

	// voxel nodes
	this->_renderer.CreateBuffer(this->_voxelNodes,
		Constants::NumAllocatedNodes * sizeof(VoxelNode),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	const uint32_t width = Constants::NodeDivisions;
	const float s = 1.0 / (Constants::NodeDivisions);
	const float m = -0.5 + s/2.0;
	VoxelNode nodes[Constants::NodeChildren];
	for(int z = 0; z < Constants::NodeDivisions; z++) {
		for(int y = 0; y < Constants::NodeDivisions; y++) {
			for(int x = 0; x < Constants::NodeDivisions; x++) {
				uint32_t index = x + width*y + width*width*z;
				VoxelNode* data = (VoxelNode*)this->_stagingBuffer.info.pMappedData;
				data[index] = VoxelNode{
					.pos = glm::vec4(
						m + s*(x),
						m + s*(y),
						m + s*(z),
						1.0
					),
					.childPtr = 0,
				};
			}
		}
	}
	this->_renderer.ImmediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy = {.srcOffset = 0, .dstOffset = 0, .size = sizeof(VoxelNode) * Constants::NodeChildren};
		vkCmdCopyBuffer(cmd, this->_stagingBuffer.bufferHandle, this->_voxelNodes.bufferHandle, 1, &copy);
	});

	// MESHES
	std::vector<std::vector<Vertex>> vertexBuffers;
	std::vector<std::vector<uint32_t>> indexBuffers;
	if(!loadGltfMeshes(Constants::MeshFile, vertexBuffers, indexBuffers)) {
		std::cout << "[ERROR] Failed to load meshes" << std::endl;
	}
	for(int m = 0; m < vertexBuffers.size(); m++) {
		this->_renderer.UploadMesh(this->_testMeshes[m], vertexBuffers[m], indexBuffers[m]);
		std::cout << "Mesh Triangles: " << indexBuffers[m].size() / 3 << std::endl;
	}
	return true;
return true;
}

bool FluidEngine::initPipelines()
{
    this->_renderer.CreateGraphicsPipeline(this->_meshPipeline, SHADER_DIRECTORY"/mesh.vert.spv", SHADER_DIRECTORY"/mesh.frag.spv", "", {}, 0, 
                                           sizeof(GraphicsPushConstants), {.blendMode = BlendMode::Additive});

    this->_renderer.CreateGraphicsPipeline(this->_linePipeline, SHADER_DIRECTORY"/line.vert.spv", SHADER_DIRECTORY"/line.frag.spv", "", {}, 0, 
                                           sizeof(GraphicsPushConstants), {.inputTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST});

    this->_renderer.CreateGraphicsPipeline(this->_voxelRasterPipeline, SHADER_DIRECTORY"/voxelRaster.vert.spv", SHADER_DIRECTORY"/voxelRaster.frag.spv", SHADER_DIRECTORY"/voxelRaster.geom.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelVolume.bufferHandle, this->_voxelVolume.info.size),
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, this->_voxelInfoBuffer.bufferHandle, sizeof(VoxelInfo)),
        BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentCounter.bufferHandle, sizeof(VoxelFragmentCounter)),
        BufferDescriptor(0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentList.bufferHandle, this->_voxelFragmentList.info.size)
    },
    VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(GraphicsPushConstants));

    this->_renderer.CreateComputePipeline(this->_voxelizerPipeline, SHADER_DIRECTORY"/voxelizer.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelVolume.bufferHandle, Constants::VoxelGridSize)
    },
    sizeof(VoxelizerPushConstants));

    this->_renderer.CreateComputePipeline(this->_raytracerPipeline, SHADER_DIRECTORY"/voxelTracer.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelVolume.bufferHandle, Constants::VoxelGridSize),
        ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_renderer.GetDrawImage().imageView, VK_NULL_HANDLE, this->_renderer.GetDrawImage().layout)
    },
    sizeof(RayTracerPushConstants));

    this->_renderer.CreateComputePipeline(this->_treeLineGenerator, SHADER_DIRECTORY"/treeRenderer.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelNodes.bufferHandle, this->_voxelNodes.info.size),
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_treeIndirectDrawBuffer.bufferHandle, sizeof(VkDrawIndexedIndirectCommand))
    },
    sizeof(TreeRendererPushConstants));

    this->_renderer.CreateComputePipeline(this->_treeIndirectCmdGenerator, SHADER_DIRECTORY"/treePopulateIndirectCmds.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_treeIndirectDispatchBuffer.bufferHandle, sizeof(VkDispatchIndirectCommand)),
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_treeFlaggerIndirectDispatchBuffer.bufferHandle, sizeof(VkDispatchIndirectCommand)),
        BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_treeIndirectDrawBuffer.bufferHandle, sizeof(VkDrawIndexedIndirectCommand)),
        BufferDescriptor(0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_treeInfoBuffer.bufferHandle, sizeof(TreeInfo)),
        BufferDescriptor(0, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentCounter.bufferHandle, sizeof(VoxelFragmentCounter))
    });

    this->_renderer.CreateComputePipeline(this->_treeSubdivider, SHADER_DIRECTORY"/treeSubdivider.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_treeInfoBuffer.bufferHandle, sizeof(TreeInfo)),
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelNodes.bufferHandle, this->_voxelNodes.info.size)
    });

    this->_renderer.CreateComputePipeline(this->_treeFlagger, SHADER_DIRECTORY"/treeFlagger.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentList.bufferHandle, this->_voxelFragmentList.info.size),
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentCounter.bufferHandle, sizeof(VoxelFragmentCounter)),
        BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelNodes.bufferHandle, this->_voxelNodes.info.size)
    });

    return true;
}
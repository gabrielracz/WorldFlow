#include "fluid_engine.hpp"
#include "fluid_engine_structs.hpp"
#include "renderer.hpp"
#include "path_config.hpp"
#include "defines.hpp"

#include "vma.hpp"
#include "vk_loader.h"
#include "vk_initializers.h"

#include <functional>

namespace Constants
{
constexpr uint64_t StagingBufferSize = 1024ul * 1024ul * 8ul;

constexpr size_t VoxelGridResolution = 1000;
constexpr size_t VoxelGridSize = VoxelGridResolution * VoxelGridResolution * VoxelGridResolution * sizeof(float);
constexpr float VoxelGridScale = 2.0f;

constexpr uint32_t MeshIdx = 0;
constexpr float MeshScale = 0.01;
// constexpr float MeshScale = 0.0007;
// constexpr float MeshScale = 0.60;
constexpr glm::vec3 MeshTranslation = glm::vec3(0.0, 0.0, 0.0);
const glm::mat4 MeshTransform = glm::translate(MeshTranslation) * glm::scale(glm::vec3(MeshScale));
const std::string MeshFile = ASSETS_DIRECTORY"/xyz.glb";

constexpr glm::vec3 LightPosition = glm::vec4(10.0, 10.0, 10.0, 1.0);

constexpr uint32_t NumAllocatedVoxelFragments = 1024 * 1000;
constexpr uint32_t VoxelFragmentListSize = NumAllocatedVoxelFragments * sizeof(VoxelFragment);
constexpr uint32_t NumAllocatedVoxelNodes = 128 * 128 * 128 * 2;
constexpr uint32_t VoxelNodesSize = NumAllocatedVoxelNodes * sizeof(VoxelNode);
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
	checkInput(this->_renderer.GetKeyMap(), this->_renderer.GetMouseMap(), this->_renderer.GetMouse());

	// zeroVoxelBuffers(cmd);

	if(this->_shouldAddVoxelNoise) {
		updateVoxelVolume(cmd); // fill voxels with sample noise
	}

	voxelRasterizeGeometry(cmd);


	if(this->_shouldSubdivide) {
		// for(int i = 0; i < 5; i++) {
			generateTreeIndirectCommands(cmd);
			flagNodesForSubdivision(cmd);
			subdivideTree(cmd);
		// }
		this->_shouldSubdivide = false;
	}

	if(this->_shouldRenderLines) {
		// generateTreeIndirectCommands(cmd);
		generateTreeGeometry(cmd);
		drawLines(cmd);
	}

	if(this->_shouldRenderGeometry) {
		drawGeometry(cmd, dt);
	}

	if(this->_shouldRenderVoxels) {
		rayCastVoxelVolume(cmd);
	}
}

void FluidEngine::zeroVoxelBuffers(VkCommandBuffer cmd)
{
	vkCmdFillBuffer(cmd, this->_voxelNodes.bufferHandle, 8 * sizeof(VoxelNode), VK_WHOLE_SIZE, 0);
	const uint32_t width = Constants::NodeDivisions;
	const float s = 1.0 / (Constants::NodeDivisions);
	const float m = -0.5 + s/2.0;
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
	VkBufferCopy copy = {.srcOffset = 0, .dstOffset = 0, .size = sizeof(VoxelNode) * Constants::NodeChildren};
	vkCmdCopyBuffer(cmd, this->_stagingBuffer.bufferHandle, this->_voxelNodes.bufferHandle, 1, &copy);

	vkCmdFillBuffer(cmd, this->_voxelGrid.bufferHandle, 0, VK_WHOLE_SIZE, 0);
	vkCmdFillBuffer(cmd, this->_treeInfoBuffer.bufferHandle, 0, VK_WHOLE_SIZE, 8);
	VkBufferMemoryBarrier bufferBarriers[] = {
			this->_voxelNodes.CreateBarrier(),
			this->_voxelFragmentCounter.CreateBarrier(),
			this->_voxelFragmentList.CreateBarrier(),
			this->_voxelGrid.CreateBarrier()
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(bufferBarriers), bufferBarriers, 0, nullptr);
}

void
FluidEngine::drawLines(VkCommandBuffer cmd)
{
	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_renderer.GetDrawImage().imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(this->_renderer.GetWindowExtent2D(), &colorAttachmentInfo, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_linePipeline.pipeline);
	const VkViewport viewport = this->_renderer.GetWindowViewport();
	const VkRect2D scissor = this->_renderer.GetWindowScissor();
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	GraphicsPushConstants pc = {
		.worldMatrix = this->_renderer.GetCamera().GetProjectionMatrix() * this->_renderer.GetCamera().GetViewMatrix() * glm::scale(glm::vec3(Constants::VoxelGridScale)),
		.vertexBuffer = this->_treeMesh.vertexBufferAddress
	};

	vkCmdPushConstants(cmd, this->_linePipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pc);
	vkCmdBindIndexBuffer(cmd, this->_treeIndices.bufferHandle, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, this->_treeIndirectDrawBuffer.bufferHandle, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
	vkCmdEndRendering(cmd);
}

void
FluidEngine::drawGeometry(VkCommandBuffer cmd, float dt)
{
	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_renderer.GetDrawImage().imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(this->_renderer.GetWindowExtent2D(), &colorAttachmentInfo, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_meshPipeline.pipeline);
	const VkViewport viewport = this->_renderer.GetWindowViewport();
	const VkRect2D scissor = this->_renderer.GetWindowScissor();
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	GraphicsPushConstants pc = {
		.worldMatrix = this->_renderer.GetCamera().GetProjectionMatrix() * this->_renderer.GetCamera().GetViewMatrix() * Constants::MeshTransform,
		.vertexBuffer = this->_testMeshes[Constants::MeshIdx].vertexBufferAddress
	};

	vkCmdPushConstants(cmd, this->_meshPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pc);
	vkCmdBindIndexBuffer(cmd, this->_testMeshes[Constants::MeshIdx].indexBuffer.bufferHandle, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, this->_testMeshes[Constants::MeshIdx].numIndices, 1, 0, 0, 0);
	vkCmdEndRendering(cmd);
}

void
FluidEngine::voxelRasterizeGeometry(VkCommandBuffer cmd)
{
	this->_voxelImage.Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	// Zero the counter
	vkCmdFillBuffer(cmd, this->_voxelFragmentCounter.bufferHandle, 0, VK_WHOLE_SIZE, 0);
	VkMemoryBarrier2 memBarrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
	};

	VkDependencyInfo dependencyInfo = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &memBarrier
	};

	vkCmdPipelineBarrier2(cmd, &dependencyInfo);
	VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_voxelImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(VkExtent2D{Constants::VoxelGridResolution, Constants::VoxelGridResolution}, &colorAttachmentInfo, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_voxelRasterPipeline.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_voxelRasterPipeline.layout, 0, 1, this->_voxelRasterPipeline.descriptorSets.data(), 0, nullptr);

	VkViewport viewport = {
		.x = 0,
		.y = 0,
		.width = (float)Constants::VoxelGridResolution,
		.height = (float)Constants::VoxelGridResolution,
		.minDepth = 0.0,
		.maxDepth = 1.0
	};
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {
		.offset = { .x = 0, .y = 0 },
		.extent = { .width = Constants::VoxelGridResolution, .height = Constants::VoxelGridResolution}
	};
	vkCmdSetScissor(cmd, 0, 1, &scissor);


	GraphicsPushConstants pc = {
		.worldMatrix = glm::mat4(1.0) * Constants::MeshTransform,
		.vertexBuffer = this->_testMeshes[Constants::MeshIdx].vertexBufferAddress
	};

	vkCmdPushConstants(cmd, this->_voxelRasterPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pc);
	vkCmdBindIndexBuffer(cmd, this->_testMeshes[Constants::MeshIdx].indexBuffer.bufferHandle, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, this->_testMeshes[Constants::MeshIdx].numIndices, 1, 0, 0, 0);
	vkCmdEndRendering(cmd);
	VkBufferMemoryBarrier bufferBarriers[] = {
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.buffer = this->_voxelGrid.bufferHandle,
			.offset = 0,
			.size = VK_WHOLE_SIZE,
		},
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.buffer = this->_voxelFragmentCounter.bufferHandle,
			.offset = 0,
			.size = VK_WHOLE_SIZE
		}
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, bufferBarriers, 0, nullptr);
}

void
FluidEngine::updateVoxelVolume(VkCommandBuffer cmd)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_voxelizerPipeline.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_voxelizerPipeline.layout, 0, 1, this->_voxelizerPipeline.descriptorSets.data(), 0, nullptr);
	VoxelizerPushConstants pc = {
		.gridSize = glm::vec3(Constants::VoxelGridResolution),
		.gridScale = 1.0f/Constants::VoxelGridResolution,
		.time = static_cast<float>(this->_renderer.GetElapsedTime())
	};
	vkCmdPushConstants(cmd, this->_voxelizerPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelizerPushConstants), &pc);

	constexpr uint32_t localWorkgroupSize = 8;
	constexpr uint32_t groupCount = Constants::VoxelGridResolution / localWorkgroupSize;
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
}

void
FluidEngine::rayCastVoxelVolume(VkCommandBuffer cmd)
{
	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_raytracerPipeline.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_raytracerPipeline.layout, 0, 1, this->_raytracerPipeline.descriptorSets.data(), 0, nullptr);

	glm::mat4 view = this->_renderer.GetCamera().GetViewMatrix();
	glm::mat4 invView = glm::inverse(view);
	glm::mat4 projection = this->_renderer.GetCamera().GetProjectionMatrix();
	projection[1][1] *= -1;
	RayTracerPushConstants rtpc = {
		.inverseProjection = glm::inverse(projection),
		.inverseView = invView,
		.cameraPos = invView * glm::vec4(0.0, 0.0, 0.0, 1.0),
		.nearPlane = 0.1f,
		.screenSize = glm::vec2(this->_renderer.GetWindowExtent2D().width, this->_renderer.GetWindowExtent2D().height),
		.maxDistance = 2000.0f,
		.stepSize = 0.1,
		.gridSize = glm::vec3(Constants::VoxelGridResolution),
		.gridScale = Constants::VoxelGridScale,
		.lightSource = glm::vec4(30.0, 50.0, 20.0, 1.0),
		// .baseColor = glm::vec4(HEXCOLOR(0xFFBF00))
		// .baseColor = glm::vec4(HEXCOLOR(0x675CFF))
		.baseColor = glm::vec4(0.8, 0.8, 0.8, 1.0)
	};
	vkCmdPushConstants(cmd, this->_raytracerPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
	VkExtent3D groupCounts = this->_renderer.GetWorkgroupCounts(8);
	vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
}

void
FluidEngine::generateTreeGeometry(VkCommandBuffer cmd)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeLineGenerator.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeLineGenerator.layout, 0, 1, this->_treeLineGenerator.descriptorSets.data(), 0, nullptr);
	TreeRendererPushConstants pc = {
		.vertexBuffer = this->_treeMesh.vertexBufferAddress,
		.indexBuffer = this->_treeMesh.indexBufferAddress
	};
	vkCmdPushConstants(cmd, this->_treeLineGenerator.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TreeRendererPushConstants), &pc);
	// vkCmdDispatch(cmd, Constants::NodeChildren, 1, 1);
	vkCmdDispatchIndirect(cmd, this->_treeIndirectDispatchBuffer.bufferHandle, 0);
	VkBufferMemoryBarrier bufferBarriers[] = {
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.buffer = this->_treeIndices.bufferHandle,
			.offset = 0,
			.size = VK_WHOLE_SIZE,
		},
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.buffer = this->_treeVertices.bufferHandle,
			.offset = 0,
			.size = VK_WHOLE_SIZE,
		},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(bufferBarriers), bufferBarriers, 0, nullptr);
}

void
FluidEngine::generateTreeIndirectCommands(VkCommandBuffer cmd)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeIndirectCmdGenerator.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeIndirectCmdGenerator.layout, 0, 1, this->_treeIndirectCmdGenerator.descriptorSets.data(), 0, nullptr);
	vkCmdDispatch(cmd, 1, 1, 1);
	VkBufferMemoryBarrier bufferBarriers[] = {
		this->_treeIndirectDispatchBuffer.CreateBarrier(),
		this->_treeFlaggerIndirectDispatchBuffer.CreateBarrier(),
		this->_treeIndirectDrawBuffer.CreateBarrier()
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, ARRLEN(bufferBarriers), bufferBarriers, 0, nullptr);
}

void
FluidEngine::subdivideTree(VkCommandBuffer cmd)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeSubdivider.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeSubdivider.layout, 0, 1, this->_treeSubdivider.descriptorSets.data(), 0, nullptr);
	vkCmdDispatchIndirect(cmd, this->_treeIndirectDispatchBuffer.bufferHandle, 0); // TODO: rethink this indirect buffer
	VkBufferMemoryBarrier bufferBarriers[] = {
			this->_treeInfoBuffer.CreateBarrier(),
			this->_voxelNodes.CreateBarrier()
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(bufferBarriers), bufferBarriers, 0, nullptr);
}

void
FluidEngine::flagNodesForSubdivision(VkCommandBuffer cmd)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeFlagger.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_treeFlagger.layout, 0, 1, this->_treeFlagger.descriptorSets.data(), 0, nullptr);
	vkCmdDispatchIndirect(cmd, this->_treeFlaggerIndirectDispatchBuffer.bufferHandle, 0); // TODO: rethink this indirect buffer
	VkBufferMemoryBarrier bufferBarriers[] = {
			this->_voxelNodes.CreateBarrier()
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(bufferBarriers), bufferBarriers, 0, nullptr);
}


void
FluidEngine::checkInput(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse)
{

	if(keyMap[SDLK_q]) {
		this->_shouldRenderGeometry = !this->_shouldRenderGeometry;
		keyMap[SDLK_q] = false;
	}
	if(keyMap[SDLK_w]) {
		this->_shouldRenderVoxels = !this->_shouldRenderVoxels;
		keyMap[SDLK_w] = false;
	}
	if(keyMap[SDLK_e]) {
		this->_shouldRenderLines = !this->_shouldRenderLines;
		keyMap[SDLK_e] = false;
	}
	if(keyMap[SDLK_r]) {
		this->_shouldSubdivide = !this->_shouldSubdivide;
		if(!keyMap[SDLK_LSHIFT]) {
			keyMap[SDLK_r] = false;
		}
	}
	if(keyMap[SDLK_a]) {
		this->_shouldAddVoxelNoise = !this->_shouldAddVoxelNoise;
		keyMap[SDLK_a] = false;
	}

	if(mouseMap[SDL_BUTTON_LEFT]) {
		float mouse_sens = -0.003f;
		glm::vec2 look = mouse.move * mouse_sens;
		this->_renderer.GetCamera().OrbitYaw(-look.x);
		this->_renderer.GetCamera().OrbitPitch(-look.y);
		mouse.move = {0.0, 0.0};
	}

	if(mouse.scroll != 0.0f) {
		float delta = -mouse.scroll * 0.1f;
		this->_renderer.GetCamera().distance += delta;
		mouse.scroll = 0.0;
	}
}

bool FluidEngine::initResources()
{
	// IMAGES
	this->_renderer.CreateImage(this->_voxelImage,
		VkExtent3D{Constants::VoxelGridResolution, Constants::VoxelGridResolution, 1},
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	);
	// BUFFERS
	this->_renderer.CreateBuffer(this->_stagingBuffer, Constants::StagingBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	this->_renderer.CreateBuffer(this->_voxelGrid, Constants::VoxelGridSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	this->_renderer.ImmediateSubmit([&](VkCommandBuffer cmd) {
		vkCmdFillBuffer(cmd, this->_voxelGrid.bufferHandle, 0, VK_WHOLE_SIZE, 0);
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
	std::cout << "VOXBUFFMEM: " << this->_voxelGrid.info.size << std::endl;

	this->_renderer.CreateBuffer(
		this->_voxelFragmentCounter,
		sizeof(VoxelFragmentCounter),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_CPU_ONLY
	);

	this->_renderer.CreateBuffer(
		this->_voxelFragmentList,
		Constants::NumAllocatedVoxelFragments * sizeof(VoxelFragment),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	this->_renderer.CreateBuffer(
		this->_treeVertices,
		Constants::NumAllocatedVoxelNodes * 8 * sizeof(glm::vec3), // 8 vertices per cube
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
		Constants::NumAllocatedVoxelNodes * 12 * 2 * sizeof(uint32_t), // 12 lines per cube, 2 indices per line
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
		Constants::VoxelNodesSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	const uint32_t width = Constants::NodeDivisions;
	const float s = 1.0 / (Constants::NodeDivisions);
	const float m = -0.5 + s/2.0;
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
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelGrid.bufferHandle, Constants::VoxelGridSize),
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, this->_voxelInfoBuffer.bufferHandle, sizeof(VoxelInfo)),
        BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentCounter.bufferHandle, sizeof(VoxelFragmentCounter)),
        BufferDescriptor(0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentList.bufferHandle, Constants::VoxelFragmentListSize)
    },
    VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(GraphicsPushConstants));

    this->_renderer.CreateComputePipeline(this->_voxelizerPipeline, SHADER_DIRECTORY"/voxelizer.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelGrid.bufferHandle, Constants::VoxelGridSize)
    },
    sizeof(VoxelizerPushConstants));

    this->_renderer.CreateComputePipeline(this->_raytracerPipeline, SHADER_DIRECTORY"/voxelTracer.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelGrid.bufferHandle, Constants::VoxelGridSize),
        ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_renderer.GetDrawImage().imageView, VK_NULL_HANDLE, this->_renderer.GetDrawImage().layout)
    },
    sizeof(RayTracerPushConstants));

    this->_renderer.CreateComputePipeline(this->_treeLineGenerator, SHADER_DIRECTORY"/treeRenderer.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelNodes.bufferHandle, Constants::VoxelNodesSize),
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
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelNodes.bufferHandle, Constants::VoxelNodesSize)
    });

    this->_renderer.CreateComputePipeline(this->_treeFlagger, SHADER_DIRECTORY"/treeFlagger.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentList.bufferHandle, this->_voxelFragmentList.info.size),
        BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelFragmentCounter.bufferHandle, sizeof(VoxelFragmentCounter)),
        BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_voxelNodes.bufferHandle, Constants::VoxelNodesSize)
    });

    return true;
}
#include "uniform_fluid_engine.hpp"
#include "fluid_engine_structs.hpp"
#include "renderer.hpp"
#include "path_config.hpp"
#include "defines.hpp"

#include "renderer_structs.hpp"
#include "vma.hpp"
#include "vk_initializers.h"

#include <functional>
#include <vulkan/vulkan_core.h>

/* STRUCTS */
struct alignas(16) VoxelGridCell
{
	float value;
};

struct alignas(16) AddVoxelsPushConstants
{
    glm::uvec3 gridSize;
    float gridScale;
    float time;
};

// struct alignas(16) RayTracerPushConstants
// {
//     glm::mat4 inverseProjection;  
//     glm::mat4 inverseView;        
//     glm::vec3 cameraPos;         
//     float nearPlane;        
//     glm::vec2 screenSize;        
//     float maxDistance;      
//     float stepSize;         
//     glm::vec3 gridSize;          
//     float gridScale;          
//     glm::vec4 lightSource;
//     glm::vec4 baseColor;
// };

namespace Constants
{
constexpr size_t VoxelGridResolution = 128;
constexpr size_t VoxelGridSize = VoxelGridResolution * VoxelGridResolution * VoxelGridResolution * sizeof(VoxelGridCell);
constexpr float VoxelGridScale = 2.0f;

constexpr glm::vec3 LightPosition = glm::vec4(10.0, 10.0, 10.0, 1.0);
}

UniformFluidEngine::UniformFluidEngine(Renderer& renderer)
	: _renderer(renderer) {}

UniformFluidEngine::~UniformFluidEngine() {}

bool UniformFluidEngine::Init()
{
	const bool initSuccess = initResources() &&
							 initPipelines();
	if(!initSuccess) {
		return false;
	}

	this->_renderer.RegisterUpdateCallback(std::bind(&UniformFluidEngine::update, this, std::placeholders::_1, std::placeholders::_2));
	return true;
}

void UniformFluidEngine::update(VkCommandBuffer cmd, float dt)
{
	checkInput(this->_renderer.GetKeyMap(), this->_renderer.GetMouseMap(), this->_renderer.GetMouse());

	addVoxels(cmd);
	renderVoxelVolume(cmd);
}

void
UniformFluidEngine::addVoxels(VkCommandBuffer cmd)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computeAddVoxels.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computeAddVoxels.layout, 0, 1, this->_computeAddVoxels.descriptorSets.data(), 0, nullptr);
	VoxelizerPushConstants pc {
		.gridSize = glm::uvec3(Constants::VoxelGridResolution),
		.gridScale = 1.0/Constants::VoxelGridResolution,
		.time = this->_renderer.GetElapsedTime()
	};
	vkCmdPushConstants(cmd, this->_computeAddVoxels.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelizerPushConstants), &pc);
	constexpr uint32_t localWorkgroupSize = 8;
	constexpr uint32_t groupCount = Constants::VoxelGridResolution / localWorkgroupSize;
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);

	VkBufferMemoryBarrier barriers[] = {
		this->_buffVoxelGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::renderVoxelVolume(VkCommandBuffer cmd)
{
	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computeRaycastVoxelGrid.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_computeRaycastVoxelGrid.layout, 0, 1, this->_computeRaycastVoxelGrid.descriptorSets.data(), 0, nullptr);

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
	vkCmdPushConstants(cmd, this->_computeRaycastVoxelGrid.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
	VkExtent3D groupCounts = this->_renderer.GetWorkgroupCounts(8);
	vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
}

// void
// UniformFluidEngine::addVoxels(VkCommandBuffer cmd)
// {
// 	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_voxelizerPipeline.pipeline);
// 	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_voxelizerPipeline.layout, 0, 1, this->_voxelizerPipeline.descriptorSets.data(), 0, nullptr);
// 	VoxelizerPushConstants pc = {
// 		.gridSize = glm::vec3(Constants::VoxelGridResolution),
// 		.gridScale = 1.0f/Constants::VoxelGridResolution,
// 		.time = this->_renderer.GetElapsedTime()
// 	};
// 	vkCmdPushConstants(cmd, this->_voxelizerPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VoxelizerPushConstants), &pc);

// 	constexpr uint32_t localWorkgroupSize = 8;
// 	constexpr uint32_t groupCount = Constants::VoxelGridResolution / localWorkgroupSize;
// 	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
// }

// void
// UniformFluidEngine::rayCastVoxelVolume(VkCommandBuffer cmd)
// {
// 	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
// 	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_raytracerPipeline.pipeline);
// 	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->_raytracerPipeline.layout, 0, 1, this->_raytracerPipeline.descriptorSets.data(), 0, nullptr);

// 	glm::mat4 view = this->_renderer.GetCamera().GetViewMatrix();
// 	glm::mat4 invView = glm::inverse(view);
// 	glm::mat4 projection = this->_renderer.GetCamera().GetProjectionMatrix();
// 	projection[1][1] *= -1;
// 	RayTracerPushConstants rtpc = {
// 		.inverseProjection = glm::inverse(projection),
// 		.inverseView = invView,
// 		.cameraPos = invView * glm::vec4(0.0, 0.0, 0.0, 1.0),
// 		.nearPlane = 0.1f,
// 		.screenSize = glm::vec2(this->_renderer.GetWindowExtent2D().width, this->_renderer.GetWindowExtent2D().height),
// 		.maxDistance = 2000.0f,
// 		.stepSize = 0.1,
// 		.gridSize = glm::vec3(Constants::VoxelGridResolution),
// 		.gridScale = Constants::VoxelGridScale,
// 		.lightSource = glm::vec4(30.0, 50.0, 20.0, 1.0),
// 		// .baseColor = glm::vec4(HEXCOLOR(0xFFBF00))
// 		// .baseColor = glm::vec4(HEXCOLOR(0x675CFF))
// 		.baseColor = glm::vec4(0.8, 0.8, 0.8, 1.0)
// 	};
// 	vkCmdPushConstants(cmd, this->_raytracerPipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
// 	VkExtent3D groupCounts = this->_renderer.GetWorkgroupCounts(8);
// 	vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
// }

void
UniformFluidEngine::checkInput(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse)
{
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

bool
UniformFluidEngine::initResources()
{

	this->_renderer.CreateBuffer(
		this->_buffVoxelGrid,
		Constants::VoxelGridSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	return true;
}

bool
UniformFluidEngine::initPipelines()
{
    this->_renderer.CreateComputePipeline(this->_computeRaycastVoxelGrid, SHADER_DIRECTORY"/voxelTracer.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffVoxelGrid.bufferHandle, Constants::VoxelGridSize),
        ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_renderer.GetDrawImage().imageView, VK_NULL_HANDLE, this->_renderer.GetDrawImage().layout)
    },
    sizeof(RayTracerPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAddVoxels, SHADER_DIRECTORY"/voxelizer.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffVoxelGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(VoxelizerPushConstants));
	return true;	
}
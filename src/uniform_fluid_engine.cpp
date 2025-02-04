#include "uniform_fluid_engine.hpp"
#include "fluid_engine_structs.hpp"
#include "renderer.hpp"
#include "path_config.hpp"
#include "defines.hpp"

#include "renderer_structs.hpp"
#include "vma.hpp"

#include <functional>
#include <vulkan/vulkan_core.h>

/* STRUCTS */
struct alignas(16) FluidGridCell
{
	glm::vec3 velocity;
	float density;
};

struct alignas(16) FluidGridInfo
{
	glm::uvec3 resolution;
	float scale;
};

struct alignas(16) FluidPushConstants
{
	float time;
	float dt;
	uint redBlack;
};

/* CONSTANTS */
namespace Constants
{
constexpr size_t VoxelGridResolution = 128;
constexpr size_t VoxelGridSize = VoxelGridResolution * VoxelGridResolution * VoxelGridResolution * sizeof(FluidGridCell);
constexpr float VoxelGridScale = 2.0f;

constexpr uint32_t NumDiffusionIterations = 10;

constexpr glm::vec3 LightPosition = glm::vec4(10.0, 10.0, 10.0, 1.0);
}

/* FUNCTIONS */
static uint32_t
getFluidDispatchGroupCount(uint32_t localGroupSize)
{
	return Constants::VoxelGridResolution / localGroupSize;
}

/* CLASS */
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
	checkControls(this->_renderer.GetKeyMap(), this->_renderer.GetMouseMap(), this->_renderer.GetMouse(), dt);

	if(this->_shouldAddSources) {
		addSources(cmd);
		// this->_shouldAddSources = false;
	}

	// diffuseVelocity(cmd, dt);
	// advectVelocity(cmd, dt);
	// projectIncompressible(cmd, dt);

	if(this->_shouldDiffuseDensity)
		diffuseDensity(cmd, dt);

	// advectDensity(cmd, dt);

	renderVoxelVolume(cmd);
}

void
UniformFluidEngine::addSources(VkCommandBuffer cmd)
{
	this->_computeAddSources.Bind(cmd);

	FluidPushConstants pc = {.time = this->_renderer.GetElapsedTime()};
	vkCmdPushConstants(cmd, this->_computeAddSources.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

	const uint32_t groupCount = getFluidDispatchGroupCount(8);
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::diffuseDensity(VkCommandBuffer cmd, float dt)
{
	this->_computeDiffuseDensity.Bind(cmd);


	for(uint32_t i = 0; i < Constants::NumDiffusionIterations; i++) {
		FluidPushConstants pc = {
			.time = this->_renderer.GetElapsedTime(),
			.dt = dt/Constants::NumDiffusionIterations,
			.redBlack = (i % 2)
		};
		vkCmdPushConstants(cmd, this->_computeDiffuseDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		const uint32_t groupCount = getFluidDispatchGroupCount(8);
		vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
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
		.baseColor = glm::vec4(0.8, 0.8, 0.8, 1.0)
	};
	vkCmdPushConstants(cmd, this->_computeRaycastVoxelGrid.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
	VkExtent3D groupCounts = this->_renderer.GetWorkgroupCounts(8);
	vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
}

void
UniformFluidEngine::checkControls(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse, float dt)
{
	if(mouseMap[SDL_BUTTON_LEFT]) {
		const float mouse_sens = -0.1875f;
		glm::vec2 look = mouse.move * mouse_sens * dt;
		this->_renderer.GetCamera().OrbitYaw(-look.x);
		this->_renderer.GetCamera().OrbitPitch(-look.y);
		mouse.move = {0.0, 0.0};
	}

	if(mouse.scroll != 0.0f) {
		float delta = -mouse.scroll * 6.25f * dt;
		this->_renderer.GetCamera().distance += delta;
		mouse.scroll = 0.0;
	}

	if(keyMap[SDLK_q]) {
		this->_shouldDiffuseDensity = !this->_shouldDiffuseDensity;
		keyMap[SDLK_q] = false;
	}
}

bool
UniformFluidEngine::initResources()
{

	this->_renderer.CreateBuffer(
		this->_buffFluidGrid,
		Constants::VoxelGridSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
		vkCmdFillBuffer(cmd, this->_buffFluidGrid.bufferHandle, 0, VK_WHOLE_SIZE, 0);
	});

	this->_renderer.CreateBuffer(
		this->_buffFluidInfo,
		sizeof(FluidGridInfo),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
		FluidGridInfo fluidInfo = {
			.resolution = glm::uvec3(Constants::VoxelGridResolution),
			.scale = 1.0
		};
		VkBufferCopy copy = {
			.size = sizeof(FluidGridInfo),
		};
		vkCmdUpdateBuffer(cmd, this->_buffFluidInfo.bufferHandle, 0, sizeof(FluidGridInfo), &fluidInfo);
	});

	return true;
}

bool
UniformFluidEngine::initPipelines()
{
    this->_renderer.CreateComputePipeline(this->_computeRaycastVoxelGrid, SHADER_DIRECTORY"/voxelTracerAccum.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize),
        ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_renderer.GetDrawImage().imageView, VK_NULL_HANDLE, this->_renderer.GetDrawImage().layout)
    },
    sizeof(RayTracerPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAddSources, SHADER_DIRECTORY"/fluid_add_sources.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeDiffuseDensity, SHADER_DIRECTORY"/fluid_diffuse_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));


	return true;	
}
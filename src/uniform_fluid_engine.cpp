#include "uniform_fluid_engine.hpp"
#include "fluid_engine_structs.hpp"
#include "renderer.hpp"
#include "path_config.hpp"
#include "defines.hpp"

#include <glm/gtx/string_cast.hpp>

#include "renderer_structs.hpp"
#include "vma.hpp"
#include "imgui.h"

#include <functional>
#include <vulkan/vulkan_core.h>

/* STRUCTS */
struct alignas(16) FluidGridCell
{
	glm::vec3 velocity;
	float density;
	float pressure;
	float divergence;
	int occupied;
	float padding;
};

struct alignas(16) FluidGridInfo
{
	glm::uvec3 resolution;
	float cellSize;
};

struct alignas(16) FluidPushConstants
{
	float time;
	float dt;
	uint32_t redBlack;
};

struct alignas(16) AddFluidPropertiesPushConstants
{
	glm::vec4 sourcePosition;
	glm::vec4 velocity;
	glm::vec4 objectPosition;
	float elapsed;
	float dt;
	float sourceRadius;
	int addVelocity;
	int addDensity;
    float density;
	uint32_t objectType;
	float objectRadius;
	float decayRate;
};
const auto s = sizeof(AddFluidPropertiesPushConstants);


enum Timestamps : uint32_t
{
	StartFrame = 0,
	AddFluidProperties,
	VelocityUpdate,
	PressureSolve,
	DensityUpdate,
	FluidRender,
	NumTimestamps
};

/* CONSTANTS */
namespace Constants
{
constexpr size_t VoxelGridResolution = 32;
constexpr size_t VoxelGridSize = VoxelGridResolution * VoxelGridResolution * VoxelGridResolution * sizeof(FluidGridCell);
constexpr float VoxelGridScale = 2.0f;
const uint32_t VoxelDiagonal = std::sqrt(VoxelGridResolution*VoxelGridResolution * 3);
constexpr glm::vec3 VoxelGridCenter = glm::vec3(Constants::VoxelGridResolution/2, Constants::VoxelGridResolution/2, Constants::VoxelGridResolution/2);

constexpr uint32_t LocalGroupSize = 8;

constexpr uint32_t NumDiffusionIterations = 10;
constexpr uint32_t NumPressureIterations = NumDiffusionIterations * 4;

constexpr glm::vec3 LightPosition = glm::vec4(10.0, 10.0, 10.0, 1.0);
}

/* FUNCTIONS */
static uint32_t
getFluidDispatchGroupCount(uint32_t localGroupSize = Constants::LocalGroupSize)
{
	return Constants::VoxelGridResolution / localGroupSize;
}

static void
rollingAverage(float& currentValue, float newValue) {
	const float alpha = 0.15;
	currentValue = alpha * newValue + (1.0f - alpha) * currentValue;
}

/* CLASS */
UniformFluidEngine::UniformFluidEngine(Renderer& renderer)
	: _renderer(renderer) {}

UniformFluidEngine::~UniformFluidEngine() {}

bool
UniformFluidEngine::Init()
{
	const bool initSuccess = initRendererOptions() &&
							 initResources() &&
							 initPipelines();
	if(!initSuccess) {
		return false;
	}

	this->_renderer.RegisterPreFrameCallback(std::bind(&UniformFluidEngine::preFrame, this));
	this->_renderer.RegisterUpdateCallback(std::bind(&UniformFluidEngine::update, this, std::placeholders::_1, std::placeholders::_2));
	this->_renderer.RegisterUICallback(std::bind(&UniformFluidEngine::ui, this));
	return true;
}

void
UniformFluidEngine::update(VkCommandBuffer cmd, float dt)
{
	checkControls(this->_renderer.GetKeyMap(), this->_renderer.GetMouseMap(), this->_renderer.GetMouse(), dt);

	dt = 0.08;
	// this->_timestamps.reset(this->_renderer.GetDevice());
	this->_timestamps.reset(this->_renderer.GetDevice());
	this->_timestamps.write(cmd, Timestamps::StartFrame, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	addSources(cmd);
	this->_timestamps.write(cmd, Timestamps::AddFluidProperties, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	diffuseVelocity(cmd, dt);
	advectVelocity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::VelocityUpdate, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	computeDivergence(cmd);
	solvePressure(cmd);
	projectIncompressible(cmd);
	if(this->_toggle)
		computeDivergence(cmd);
	this->_timestamps.write(cmd, Timestamps::PressureSolve, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	diffuseDensity(cmd, dt);
	advectDensity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::DensityUpdate, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	renderVoxelVolume(cmd);
	this->_timestamps.write(cmd, Timestamps::FluidRender, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void
UniformFluidEngine::addSources(VkCommandBuffer cmd)
{
	this->_computeAddSources.Bind(cmd);

	const AddFluidPropertiesPushConstants pc = {
		.sourcePosition = glm::vec4(this->_sourcePosition, 1.0),
		.velocity = glm::vec4(this->_velocitySourceAmount, 1.0),
		.objectPosition = glm::vec4(this->_objectPosition, 1.0),
		.elapsed = this->_renderer.GetElapsedTime(),
		.sourceRadius = Constants::VoxelGridResolution / 5.0,
		.addVelocity = this->_shouldAddSources,
		.addDensity = this->_shouldAddSources,
		.density = this->_densityAmount,
		.objectType = (uint32_t) this->_shouldAddObstacle > 0,
		.objectRadius = Constants::VoxelGridResolution/5.0,
		.decayRate = 0.99
	};
	vkCmdPushConstants(cmd, this->_computeAddSources.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

	const uint32_t groupCount = getFluidDispatchGroupCount();
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::diffuseVelocity(VkCommandBuffer cmd, float dt)
{
	this->_computeDiffuseVelocity.Bind(cmd);
	for(uint32_t i = 0; i < Constants::NumDiffusionIterations; i++) {
		FluidPushConstants pc = {
			.time = this->_renderer.GetElapsedTime(),
			.dt = dt,
			.redBlack = (i % 2)
		};
		vkCmdPushConstants(cmd, this->_computeDiffuseVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		const uint32_t groupCount = getFluidDispatchGroupCount();
		vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
UniformFluidEngine::diffuseDensity(VkCommandBuffer cmd, float dt)
{
	this->_computeDiffuseDensity.Bind(cmd);

	for(uint32_t i = 0; i < Constants::NumDiffusionIterations; i++) {
		FluidPushConstants pc = {
			.time = this->_renderer.GetElapsedTime(),
			.dt = dt,
			.redBlack = (i % 2)
		};
		vkCmdPushConstants(cmd, this->_computeDiffuseDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		const uint32_t groupCount = getFluidDispatchGroupCount();
		vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
UniformFluidEngine::advectVelocity(VkCommandBuffer cmd, float dt)
{
	this->_computeAdvectVelocity.Bind(cmd);

	FluidPushConstants pc = {.dt = dt};
	vkCmdPushConstants(cmd, this->_computeAdvectVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FluidPushConstants), &pc);

	const uint32_t groupCount = getFluidDispatchGroupCount();
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);

	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::advectDensity(VkCommandBuffer cmd, float dt)
{
	this->_computeAdvectDensity.Bind(cmd);

	FluidPushConstants pc = {.dt = dt};
	vkCmdPushConstants(cmd, this->_computeAdvectDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FluidPushConstants), &pc);

	const uint32_t groupCount = getFluidDispatchGroupCount();
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);

	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::computeDivergence(VkCommandBuffer cmd)
{
	this->_computeDivergence.Bind(cmd);
	FluidPushConstants pc{};
	vkCmdPushConstants(cmd, this->_computeDivergence.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	const uint32_t groupCount = getFluidDispatchGroupCount();
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::solvePressure(VkCommandBuffer cmd)
{
	this->_computeSolvePressure.Bind(cmd);

	for(uint32_t i = 0; i < Constants::NumPressureIterations; i++) {
		FluidPushConstants pc = {
			.redBlack = (i % 2)
		};
		vkCmdPushConstants(cmd, this->_computeSolvePressure.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		const uint32_t groupCount = getFluidDispatchGroupCount();
		vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
UniformFluidEngine::projectIncompressible(VkCommandBuffer cmd)
{
	this->_computeProjectIncompressible.Bind(cmd);
	FluidPushConstants pc{};
	vkCmdPushConstants(cmd, this->_computeProjectIncompressible.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	const uint32_t groupCount = getFluidDispatchGroupCount();
	vkCmdDispatch(cmd, groupCount, groupCount, groupCount);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
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
		.maxDistance = Constants::VoxelDiagonal,
		.stepSize = 0.1,
		.gridSize = glm::vec3(Constants::VoxelGridResolution),
		.gridScale = Constants::VoxelGridScale,
		.lightSource = glm::vec4(30.0, 50.0, 20.0, 1.0),
		.baseColor = glm::vec4(0.8, 0.8, 0.8, 1.0),
		.renderType = this->_renderType
	};
	vkCmdPushConstants(cmd, this->_computeRaycastVoxelGrid.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
	VkExtent3D groupCounts = this->_renderer.GetWorkgroupCounts(8);
	vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
}

void
UniformFluidEngine::preFrame()
{
	this->_timestamps.collect(this->_renderer.GetDevice());
	this->_timestamps.nextFrame();

	const uint32_t statReportPeriod = 7;
	this->_timestampAverages[Timestamps::StartFrame] = this->_timestamps.getDelta(Timestamps::StartFrame, Timestamps::FluidRender);
	this->_timestampAverages[Timestamps::AddFluidProperties] = this->_timestamps.getDelta(Timestamps::StartFrame, Timestamps::AddFluidProperties);
	this->_timestampAverages[Timestamps::VelocityUpdate] =  this->_timestamps.getDelta(Timestamps::AddFluidProperties, Timestamps::VelocityUpdate);
	this->_timestampAverages[Timestamps::PressureSolve] =  this->_timestamps.getDelta(Timestamps::VelocityUpdate, Timestamps::PressureSolve);
	this->_timestampAverages[Timestamps::DensityUpdate] =  this->_timestamps.getDelta(Timestamps::PressureSolve, Timestamps::DensityUpdate);
	this->_timestampAverages[Timestamps::FluidRender] =  this->_timestamps.getDelta(Timestamps::DensityUpdate, Timestamps::FluidRender);
}

void
UniformFluidEngine::ui()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* viewport = ImGui::GetMainViewport(); // Use GetMainViewport for multi-viewport support
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(0, 0)); // Auto-sizing
	if(ImGui::Begin("stats", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
		ImGui::Text("Total:    %3.2f ms", this->_timestampAverages[Timestamps::StartFrame]);
		ImGui::Text("Add:      %3.2f ms", this->_timestampAverages[Timestamps::AddFluidProperties]);
		ImGui::Text("Velocity: %3.2f ms", this->_timestampAverages[Timestamps::VelocityUpdate]);
		ImGui::Text("Pressure: %3.2f ms", this->_timestampAverages[Timestamps::PressureSolve]);
		ImGui::Text("Density:  %3.2f ms", this->_timestampAverages[Timestamps::DensityUpdate]);
		ImGui::Text("Render:   %3.2f ms", this->_timestampAverages[Timestamps::FluidRender]);
	}
	ImGui::End();
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

	float v = 10.0;
	constexpr glm::vec3 c = Constants::VoxelGridCenter;
	this->_objectPosition = Constants::VoxelGridCenter;
	this->_shouldAddSources = false;
	if(keyMap[SDLK_q]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(v, 0, 0);
		this->_sourcePosition = glm::vec3(0, c.y, c.z);
	}
	if(keyMap[SDLK_w]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(0, v, 0);
		this->_sourcePosition = glm::vec3(c.x, 0, c.z);
	}
	if(keyMap[SDLK_e]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(-v, 0, 0);
		this->_sourcePosition = glm::vec3(c.x*2 - 1, c.y, c.z);
	}
	if(keyMap[SDLK_r]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(0, -v, 0);
		this->_sourcePosition = glm::vec3(c.x, c.y*2 - 1, c.z);
	}

	if(keyMap[SDLK_f]) {
		this->_shouldAddObstacle = !this->_shouldAddObstacle;
		keyMap[SDLK_f] = false;
	}


	for(int i = 1; i <= 4; i++) {
		if(keyMap[SDLK_0 + i]) {
			this->_renderType = i;
			keyMap[SDLK_0 + i] = false;
		}
	}
}

bool
UniformFluidEngine::initRendererOptions()
{
	this->_timestamps.init(this->_renderer.GetDevice(), Timestamps::NumTimestamps, Constants::FrameOverlap);
	this->_timestampAverages.resize(Timestamps::NumTimestamps, 0);
	return true;
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
			.cellSize = 1.0 / Constants::VoxelGridResolution
		};
		VkBufferCopy copy = {
			.size = sizeof(FluidGridInfo),
		};
		std::cout << glm::to_string(fluidInfo.resolution) << std::endl;
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
	sizeof(AddFluidPropertiesPushConstants));

	// TODO: Refactor these to return Descriptor IDS and use those.
	this->_renderer.CreateComputePipeline(this->_computeDiffuseVelocity, SHADER_DIRECTORY"/fluid_diffuse_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeDiffuseDensity, SHADER_DIRECTORY"/fluid_diffuse_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));
	this->_renderer.CreateComputePipeline(this->_computeAdvectVelocity, SHADER_DIRECTORY"/fluid_advect_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAdvectDensity, SHADER_DIRECTORY"/fluid_advect_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeDivergence, SHADER_DIRECTORY"/fluid_compute_divergence.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeSolvePressure, SHADER_DIRECTORY"/fluid_solve_pressure.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeProjectIncompressible, SHADER_DIRECTORY"/fluid_project_incompressible.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize)
	},
	sizeof(FluidPushConstants));
	return true;	
}
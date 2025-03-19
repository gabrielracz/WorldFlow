#include "uniform_fluid_engine.hpp"
#include "fluid_engine_structs.hpp"
#include "renderer.hpp"
#include "path_config.hpp"
#include "defines.hpp"

#include <cstdlib>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "renderer_structs.hpp"
#include "vma.hpp"
#include "imgui.h"
#include "ui_tools.hpp"

#include <functional>
#include <vulkan/vulkan_core.h>

#include "vk_initializers.h"

/* STRUCTS */
struct alignas(16) FluidGridCell
{
	glm::vec4 velocity;
	float density;
	float pressure;
	float divergence;
	int occupied;
	glm::vec4 padding;
};

// Property Buffer Types
typedef glm::vec4  FluidVelocity;
typedef float      FluidDensity;
typedef float      FluidPressure;
typedef float      FluidDivergence;
typedef uint32_t   FluidFlags;
typedef glm::vec4  FluidDebug;
typedef uint32_t   FluidBrickOffsets;
struct alignas(16) FluidGridReferences
{
	uint64_t velocityBufferReference;
	uint64_t densityBufferReference;
	uint64_t pressureBufferReference;
	uint64_t divergenceBufferReference;
	uint64_t flagsBufferReference;
	uint64_t debugBufferReference;
	uint64_t brickOffsetsBufferReference;
};

struct alignas(16) FluidGridInfo
{
	glm::uvec4 resolution;
	glm::vec4 position;
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
	int clear;
};
const auto s = sizeof(AddFluidPropertiesPushConstants);

struct alignas(16) ParticlesPushConstants
{
    glm::mat4 cameraMatrix;
	float dt;
	float elapsed;
	float maxLifetime;
};

struct alignas(16) GenerateLinesPushConstants
{
	uint64_t vertexBufferAddress;
};

struct alignas(16) DrawLinesPushConstants
{
	glm::mat4 renderMatrix;
	uint64_t vertexBufferAddress;
};

enum Timestamps : uint32_t
{
	StartFrame = 0,
	AddFluidProperties,
	VelocityDiffusion,
	VelocityAdvect,
	PressureSolve,
	DensityDiffusion,
	DensityAdvect,
	FluidRender,
	NumTimestamps
};

struct alignas(16) Particle
{
	glm::vec4 position {};
	float mass {};
	float lifetime {}; 
};

/* CONSTANTS */
namespace Constants
{
constexpr size_t VoxelGridResolution = 16;
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(VoxelGridResolution, VoxelGridResolution, VoxelGridResolution, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(64, 64, 64, 1);
constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(128, 32, 128, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(16,16,16,1);

const uint32_t NumVoxelGridCells = VoxelGridDimensions.x * VoxelGridDimensions.y * VoxelGridDimensions.z;
const size_t VoxelGridSize = NumVoxelGridCells * sizeof(FluidGridCell);
const float VoxelGridScale = 2.0f;
const uint32_t VoxelDiagonal = VoxelGridDimensions.x + VoxelGridDimensions.y + VoxelGridDimensions.z;
constexpr glm::vec3 VoxelGridCenter = glm::vec3(VoxelGridDimensions) * 0.5f + glm::vec3(1.0);
constexpr float VoxelCellSize = 1.0 / VoxelGridResolution;

constexpr uint32_t NumGridLines = VoxelGridDimensions.y*VoxelGridDimensions.z + VoxelGridDimensions.x*VoxelGridDimensions.y + VoxelGridDimensions.x*VoxelGridDimensions.z;
const glm::uvec3 GridGroups = glm::uvec3(VoxelGridDimensions.y*VoxelGridDimensions.z, VoxelGridDimensions.x*VoxelGridDimensions.y, VoxelGridDimensions.x*VoxelGridDimensions.z);

// constexpr uint32_t LocalGroupSize = 8;
constexpr glm::uvec3 LocalGroupSize = glm::uvec3(8, 8, 8);

constexpr uint32_t NumAdvectionIterations = 1;
constexpr uint32_t NumDiffusionIterations = 4;
constexpr uint32_t NumPressureIterations = 6;

constexpr glm::vec4 LightPosition = glm::vec4(500.0, 500.0, 200, 1.0);

constexpr uint32_t NumParticles = 65536;
constexpr float MaxParticleLifetime = 240.0;
}

/* FUNCTIONS */
static void
rollingAverage(float& currentValue, float newValue) {
	const float alpha = 0.15f;
	currentValue = alpha * newValue + (1.0f - alpha) * currentValue;
}

/* CLASS */
UniformFluidEngine::UniformFluidEngine(Renderer& renderer)
	: _renderer(renderer), _diffusionIterations(Constants::NumDiffusionIterations), _pressureIterations(Constants::NumPressureIterations), _advectionIterations(Constants::NumAdvectionIterations) {}

UniformFluidEngine::~UniformFluidEngine() {}

bool
UniformFluidEngine::Init()
{
	const bool initSuccess = initRendererOptions() &&
							 initResources() &&
							 initPipelines() &&
							 initPreProcess();
	if(!initSuccess) {
		return false;
	}

	return true;
}

void
UniformFluidEngine::update(VkCommandBuffer cmd, float dt)
{
	checkControls(this->_renderer.GetKeyMap(), this->_renderer.GetMouseMap(), this->_renderer.GetMouse(), dt);

	dt = (this->_useTickRate) ? this->_tickRate : dt * this->_tickRate;
	this->_timestamps.reset(this->_renderer.GetDevice());
	this->_timestamps.write(cmd, Timestamps::StartFrame, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	addSources(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::AddFluidProperties, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	diffuseVelocity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::VelocityDiffusion, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	advectVelocity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::VelocityAdvect, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	computeDivergence(cmd);
	solvePressure(cmd);
	if(this->_shouldProjectIncompressible)
		projectIncompressible(cmd);
		// computeDivergence(cmd);
	this->_timestamps.write(cmd, Timestamps::PressureSolve, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	diffuseDensity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::DensityDiffusion, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	advectDensity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::DensityAdvect, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	// drawGrid(cmd);
	if(this->_shouldRenderFluid)
		renderVoxelVolume(cmd);
	// renderParticles(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::FluidRender, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	
}

void
UniformFluidEngine::addSources(VkCommandBuffer cmd, float dt)
{
	this->_computeAddSources.Bind(cmd);

	const AddFluidPropertiesPushConstants pc = {
		.sourcePosition = glm::vec4(this->_sourcePosition, 1.0),
		.velocity = glm::vec4(this->_velocitySourceAmount, 1.0),
		.objectPosition = glm::vec4(this->_objectPosition, 1.0),
		.elapsed = this->_renderer.GetElapsedTime(),
		.dt = dt,
		.sourceRadius = this->_sourceRadius * Constants::VoxelGridResolution,
		.addVelocity = this->_shouldAddSources,
		.addDensity = this->_shouldAddSources,
		.density = this->_densityAmount,
		.objectType = (uint32_t) this->_shouldAddObstacle > 0,
		.objectRadius = this->_objectRadius * Constants::VoxelGridResolution,
		.decayRate = this->_decayRate,
		.clear = this->_shouldClear
	};
	vkCmdPushConstants(cmd, this->_computeAddSources.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	dispatchFluid(cmd);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
		this->_buffFluidDensity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
		this->_buffFluidPressure.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
		this->_buffFluidDivergence.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
		this->_buffFluidFlags.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
		this->_buffFluidDebug.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::diffuseVelocity(VkCommandBuffer cmd, float dt)
{
	this->_computeDiffuseVelocity.Bind(cmd);
	for(uint32_t i = 0; i < this->_diffusionIterations; i++) {
		FluidPushConstants pc = {
			.time = this->_renderer.GetElapsedTime(),
			.dt = dt,
			.redBlack = ((i+1) % 2)
		};
		vkCmdPushConstants(cmd, this->_computeDiffuseVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		dispatchFluid(cmd);

		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
UniformFluidEngine::diffuseDensity(VkCommandBuffer cmd, float dt)
{
	this->_computeDiffuseDensity.Bind(cmd);

	for(uint32_t i = 0; i < this->_diffusionIterations; i++) {
		FluidPushConstants pc = {
			.time = this->_renderer.GetElapsedTime(),
			.dt = dt,
			.redBlack = ((i+1) % 2)
		};
		vkCmdPushConstants(cmd, this->_computeDiffuseDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		dispatchFluid(cmd);

		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidDensity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
UniformFluidEngine::advectVelocity(VkCommandBuffer cmd, float dt)
{
	this->_computeAdvectVelocity.Bind(cmd);
	
	for(uint32_t i = 0; i < this->_advectionIterations; i++) {
		FluidPushConstants pc = {.dt = dt/(float)this->_advectionIterations};
		vkCmdPushConstants(cmd, this->_computeAdvectVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FluidPushConstants), &pc);
		dispatchFluid(cmd);
		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
UniformFluidEngine::advectDensity(VkCommandBuffer cmd, float dt)
{
	this->_computeAdvectDensity.Bind(cmd);

	for(uint32_t i = 0; i < this->_advectionIterations; i++) {
		FluidPushConstants pc = {.dt = dt/(float)this->_advectionIterations};
		vkCmdPushConstants(cmd, this->_computeAdvectDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FluidPushConstants), &pc);
		dispatchFluid(cmd);
		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidDensity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
UniformFluidEngine::computeDivergence(VkCommandBuffer cmd)
{
	this->_computeDivergence.Bind(cmd);
	FluidPushConstants pc{};
	vkCmdPushConstants(cmd, this->_computeDivergence.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	dispatchFluid(cmd);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidDivergence.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::solvePressure(VkCommandBuffer cmd)
{
	this->_computeSolvePressure.Bind(cmd);

	for(uint32_t i = 0; i < this->_pressureIterations; i++) {
		FluidPushConstants pc = {
			.redBlack = ((i+1) % 2)
		};
		vkCmdPushConstants(cmd, this->_computeSolvePressure.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		dispatchFluid(cmd, glm::uvec3(1, 1, 1));

		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidPressure.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
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
	dispatchFluid(cmd);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
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
		.stepSize = 0.1f,
		.gridSize = glm::vec3(Constants::VoxelGridDimensions),
		.gridScale = Constants::VoxelGridScale,
		.lightSource = Constants::LightPosition,
		.baseColor = glm::vec4(0.8, 0.8, 0.8, 1.0),
		.renderType = this->_renderType
	};
	vkCmdPushConstants(cmd, this->_computeRaycastVoxelGrid.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
	VkExtent3D groupCounts = this->_renderer.GetWorkgroupCounts(8);
	vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
}

void
UniformFluidEngine::renderMesh(VkCommandBuffer cmd, Mesh& mesh, const glm::mat4& transform)
{
	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_renderer.GetDrawImage().imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(this->_renderer.GetWindowExtent2D(), &colorAttachmentInfo, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_graphicsRenderMesh.pipeline);
	const VkViewport viewport = this->_renderer.GetWindowViewport();
	const VkRect2D scissor = this->_renderer.GetWindowScissor();
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	GraphicsPushConstants pc = {
		.worldMatrix = this->_renderer.GetCamera().GetProjectionMatrix() * this->_renderer.GetCamera().GetViewMatrix() * transform,
		.vertexBuffer = mesh.vertexBufferAddress
	};

	vkCmdPushConstants(cmd, this->_graphicsRenderMesh.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pc);
	vkCmdBindIndexBuffer(cmd, mesh.indexBuffer.bufferHandle, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, mesh.numIndices, 1, 0, 0, 0);
	vkCmdEndRendering(cmd);
}

void
UniformFluidEngine::renderParticles(VkCommandBuffer cmd, float dt)
{

	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_renderer.GetDrawImage().imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(this->_renderer.GetWindowExtent2D(), &colorAttachmentInfo, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);

	this->_graphicsParticles.Bind(cmd);
	const VkViewport viewport = this->_renderer.GetWindowViewport();
	const VkRect2D scissor = this->_renderer.GetWindowScissor();
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	ParticlesPushConstants pc = {
		.cameraMatrix = this->_renderer.GetCamera().GetProjectionMatrix() * this->_renderer.GetCamera().GetViewMatrix(),
		.dt = dt,
		.elapsed = this->_renderer.GetElapsedTime(),
		.maxLifetime = Constants::MaxParticleLifetime
	};
	vkCmdPushConstants(cmd, this->_graphicsParticles.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

	vkCmdDraw(cmd, Constants::NumParticles, 1, 0, 0);
	vkCmdEndRendering(cmd);

	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::generateGridLines(VkCommandBuffer cmd)
{
	this->_computeGenerateGridLines.Bind(cmd);

	GenerateLinesPushConstants pc = {
		.vertexBufferAddress = this->_gridMesh.vertexBufferAddress
	};
	vkCmdPushConstants(cmd, this->_computeGenerateGridLines.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	
	const glm::uvec3 groups = Constants::GridGroups / Constants::LocalGroupSize;
	vkCmdDispatch(cmd, groups.x, groups.y, groups.z);
	VkBufferMemoryBarrier barrier = this->_buffGridLines.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, 0, 1, &barrier, 0, 0);
}

void
UniformFluidEngine::drawGrid(VkCommandBuffer cmd)
{
	this->_renderer.GetDrawImage().Transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_renderer.GetDrawImage().imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(this->_renderer.GetWindowExtent2D(), &colorAttachmentInfo, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);

	this->_graphicsGridLines.Bind(cmd);
	const VkViewport viewport = this->_renderer.GetWindowViewport();
	const VkRect2D scissor = this->_renderer.GetWindowScissor();
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	DrawLinesPushConstants pc = {
		.renderMatrix = this->_renderer.GetCamera().GetProjectionMatrix() * this->_renderer.GetCamera().GetViewMatrix() * glm::scale(glm::vec3(Constants::VoxelGridScale)),
		.vertexBufferAddress = this->_gridMesh.vertexBufferAddress
	};
	vkCmdPushConstants(cmd, this->_graphicsGridLines.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cmd, Constants::NumGridLines*2, 1, 0, 0);
	vkCmdEndRendering(cmd);
}

void
UniformFluidEngine::dispatchFluid(VkCommandBuffer cmd, const glm::uvec3& factor)
{
	// const glm::uvec4 groups = (Constants::VoxelGridDimensions / Constants::LocalGroupSize) / glm::uvec4(factor, 1.0);
	const glm::uvec3 groups = (glm::uvec3(Constants::VoxelGridDimensions) / Constants::LocalGroupSize) / factor;
	vkCmdDispatch(cmd, groups.x, groups.y, groups.z);
}

void
UniformFluidEngine::preFrame()
{
	this->_timestamps.collect(this->_renderer.GetDevice());
	this->_timestamps.nextFrame();

	const uint32_t statReportPeriod = 7;
	this->_timestampAverages[Timestamps::StartFrame] = this->_timestamps.getDelta(Timestamps::StartFrame, Timestamps::FluidRender);
	this->_timestampAverages[Timestamps::AddFluidProperties] = this->_timestamps.getDelta(Timestamps::StartFrame, Timestamps::AddFluidProperties);
	this->_timestampAverages[Timestamps::VelocityDiffusion] =  this->_timestamps.getDelta(Timestamps::AddFluidProperties, Timestamps::VelocityDiffusion);
	this->_timestampAverages[Timestamps::VelocityAdvect] =  this->_timestamps.getDelta(Timestamps::VelocityDiffusion, Timestamps::VelocityAdvect);
	this->_timestampAverages[Timestamps::PressureSolve] =  this->_timestamps.getDelta(Timestamps::VelocityAdvect, Timestamps::PressureSolve);
	this->_timestampAverages[Timestamps::DensityDiffusion] =  this->_timestamps.getDelta(Timestamps::PressureSolve, Timestamps::DensityDiffusion);
	this->_timestampAverages[Timestamps::DensityAdvect] =  this->_timestamps.getDelta(Timestamps::DensityDiffusion, Timestamps::DensityAdvect);
	this->_timestampAverages[Timestamps::FluidRender] =  this->_timestamps.getDelta(Timestamps::DensityAdvect, Timestamps::FluidRender);
}

void
UniformFluidEngine::drawUI()
{
	if(this->_shouldHideUI) {
		return;
	}

    ImGuiIO& io = ImGui::GetIO();

    ImGuiViewport* viewport = ImGui::GetMainViewport(); // Use GetMainViewport for multi-viewport support

	float pad = 10.0;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(0, 0)); // Auto-sizing

	ImVec2 windowSize;

	if(ImGui::Begin("stats", nullptr, ImGuiWindowFlags_NoTitleBar)) {
		const float frameTime = this->_timestampAverages[Timestamps::StartFrame];
		ImGui::Text("FPS:       %3.2f", 1.0/(frameTime/1000.0));
		ImGui::Text("Total:     %3.2f ms", frameTime);
		ImGui::Text("Add:       %3.2f ms", this->_timestampAverages[Timestamps::AddFluidProperties]);
		ImGui::Text("VelDiff:   %3.2f ms", this->_timestampAverages[Timestamps::VelocityDiffusion]);
		ImGui::Text("VelAdvect: %3.2f ms", this->_timestampAverages[Timestamps::VelocityAdvect]);
		ImGui::Text("Pressure:  %3.2f ms", this->_timestampAverages[Timestamps::PressureSolve]);
		ImGui::Text("DnsDiff:   %3.2f ms", this->_timestampAverages[Timestamps::DensityDiffusion]);
		ImGui::Text("DnsAdvect: %3.2f ms", this->_timestampAverages[Timestamps::DensityAdvect]);
		ImGui::Text("Render:    %3.2f ms", this->_timestampAverages[Timestamps::FluidRender]);
		ImGui::SameLine();
		ImGui::PushID("ShouldRender");
		ImGui::Checkbox("", &this->_shouldRenderFluid);
		ImGui::PopID();
		ImGui::Separator();
		ImGui::DragFloat("Tick", &this->_tickRate, 0.0025f);
		ImGui::SameLine();
		ImGui::PushID("Use Tick Rate");
		ImGui::Checkbox("", &this->_useTickRate);
		ImGui::PopID();
		windowSize = ImGui::GetWindowSize();
	}
	ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(pad, windowSize.y + pad), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(0, 0)); // Auto-sizing
	if(ImGui::Begin("controls", nullptr, ImGuiWindowFlags_NoTitleBar)) {
		ImGui::DragFloat("obj", &this->_objectRadius, 0.01f, 0.5f);
		ImGui::DragFloat("src", &this->_sourceRadius, 0.01f, 0.5f);
		ImGui::DragFloat("dns", &this->_densityAmount, 0.01f, 1.0f);
		ImGui::DragFloat("decay", &this->_decayRate, 0.01f);
		ImGui::DragFloat("vel", &this->_velocitySpeed, 0.025f);
		const uint32_t step = 1;
		ImGui::InputScalar("diffiter", ImGuiDataType_U32, &this->_diffusionIterations, &step);
		ImGui::InputScalar("presiter", ImGuiDataType_U32, &this->_pressureIterations, &step);
		ImGui::InputScalar("advectiter", ImGuiDataType_U32, &this->_advectionIterations, &step);
		ImGui::InputFloat3("sourcePos", glm::value_ptr(this->_sourcePosition));
		ImGui::DragFloat("objOff", &this->_objectOffset, 0.25f);
		this->_shouldClear = ImGui::Button("clear");
	}

	ImGui::End();
}

void
UniformFluidEngine::checkControls(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse, float dt)
{
	if(mouseMap[SDL_BUTTON_RIGHT]) {
		const float mouse_sens = -1.2f;
		glm::vec2 look = mouse.move * mouse_sens * dt;
		this->_renderer.GetCamera().OrbitYaw(-look.x);
		this->_renderer.GetCamera().OrbitPitch(-look.y);
		mouse.move = {0.0, 0.0};
	}

	if(mouse.scroll != 0.0f) {
		float delta = -mouse.scroll * 6.25f * dt;
		this->_renderer.GetCamera().distance += delta;
		mouse.scroll = 0.0f;
	}

	float v = this->_velocitySpeed;
	constexpr glm::vec3 c = Constants::VoxelGridCenter;
	this->_objectPosition = Constants::VoxelGridCenter;
	this->_shouldAddSources = false;
	int offset = (int)((this->_objectRadius * Constants::VoxelGridResolution / 2.0f) * this->_objectOffset);
	if(keyMap[SDLK_q]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(v, 0.f, 0.f);
		this->_sourcePosition = glm::vec3(offset, c.y, c.z);
	}
	if(keyMap[SDLK_e]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(-v, 0.f, 0.f);
		this->_sourcePosition = glm::vec3(c.x*2 - 1 - offset, c.y, c.z);
	}
	if(keyMap[SDLK_w]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(0.f, 0.f, v);
		this->_sourcePosition = glm::vec3(c.x, c.y, offset);
	}
	if(keyMap[SDLK_r]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(0.f, 0.f, -v);
		this->_sourcePosition = glm::vec3(c.x, c.y, c.z*2 - 1 - offset);
	}
	if(keyMap[SDLK_t]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(0.f, v, 0.f);
		this->_sourcePosition = glm::vec3(c.x, offset, c.z);
	}
	if(keyMap[SDLK_y]) {
		this->_shouldAddSources = true;
		this->_velocitySourceAmount = glm::vec3(0.f, -v, 0.f);
		this->_sourcePosition = glm::vec3(c.x, c.y*2 - 1 - offset, c.z);
	}

	if(keyMap[SDLK_z]) {
		this->_shouldProjectIncompressible = !this->_shouldProjectIncompressible;
		keyMap[SDLK_z] = false;
	}

	if(keyMap[SDLK_f]) {
		this->_shouldAddObstacle = !this->_shouldAddObstacle;
		keyMap[SDLK_f] = false;
	}
	
	if(keyMap[SDLK_TAB]) {
		this->_shouldHideUI = !this->_shouldHideUI;
		keyMap[SDLK_TAB] = false;
	}


	for(int i = 1; i <= 5; i++) {
		if(keyMap[SDLK_0 + i] && keyMap[SDLK_LSHIFT]) {
			this->_renderType = i;
			keyMap[SDLK_0 + i] = false;
		}
	}
}

bool
UniformFluidEngine::initRendererOptions()
{
	this->_renderer.RegisterPreFrameCallback(std::bind(&UniformFluidEngine::preFrame, this));
	this->_renderer.RegisterUpdateCallback(std::bind(&UniformFluidEngine::update, this, std::placeholders::_1, std::placeholders::_2));
	this->_renderer.RegisterUICallback(std::bind(&UniformFluidEngine::drawUI, this));

	this->_renderer.CreateTimestampQueryPool(this->_timestamps, Timestamps::NumTimestamps);
	this->_timestampAverages.resize(Timestamps::NumTimestamps, 0);

	this->_objectRadius = 0.2f;
	this->_sourceRadius = 0.15f;

	// uitools::SetAmberRedTheme();
	// uitools::SetDarkRedTheme();
	uitools::SetTheme(HEX_TO_RGB(0xcccccc), HEX_TO_RGB(0x1e1e1e), 0.5);
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
			.resolution = Constants::VoxelGridDimensions,
			.position = glm::uvec4(0),
			.cellSize = Constants::VoxelCellSize
		};
		VkBufferCopy copy = {
			.size = sizeof(FluidGridInfo),
		};
		std::cout << glm::to_string(fluidInfo.resolution) << std::endl;
		vkCmdUpdateBuffer(cmd, this->_buffFluidInfo.bufferHandle, 0, sizeof(FluidGridInfo), &fluidInfo);
	});

	this->_renderer.CreateBuffer(
		this->_buffParticles,
		Constants::NumParticles * sizeof(Particle),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);

	Particle* particles = new Particle[Constants::NumParticles];
	for(int i = 0; i < Constants::NumParticles; i++) {
		particles[i].position = glm::vec4((glm::vec3(rand(), rand(), rand()) / (float)RAND_MAX - 0.5f) * glm::vec3(Constants::VoxelGridDimensions) * (float)Constants::VoxelCellSize, 1.0);
		particles[i].mass = 0.01f;
		particles[i].lifetime = (float)rand() / (float)RAND_MAX * Constants::MaxParticleLifetime;
	}
	this->_renderer.ImmediateSubmit([&particles, this](VkCommandBuffer cmd) {
		// vkCmdUpdateBuffer(cmd, this->_buffParticles.bufferHandle, 0, sizeof(particles), &particles);
	});

	this->_renderer.CreateBuffer(
		this->_buffGridLines, Constants::NumGridLines*2*sizeof(glm::vec4),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	VkBufferDeviceAddressInfo deviceAddressInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = this->_buffGridLines.bufferHandle
	};
	this->_gridMesh.vertexBuffer = this->_buffGridLines;
	this->_gridMesh.vertexBufferAddress = vkGetBufferDeviceAddress(this->_renderer.GetDevice(), &deviceAddressInfo);

	this->_renderer.CreateBuffer(
		this->_buffStaging, Constants::NumGridLines * 2 * sizeof(glm::vec4),
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_CPU_ONLY
	);

	// FLUID PROPERTY BUFFERS
	VkBufferUsageFlags fluidBufferUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaMemoryUsage fluidMemoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
	this->_renderer.CreateBuffer(this->_buffFluidVelocity, Constants::NumVoxelGridCells * sizeof(FluidVelocity), fluidBufferUsage, fluidMemoryUsage);
	this->_renderer.CreateBuffer(this->_buffFluidDensity, Constants::NumVoxelGridCells * sizeof(FluidDensity), fluidBufferUsage, fluidMemoryUsage);
	this->_renderer.CreateBuffer(this->_buffFluidPressure, Constants::NumVoxelGridCells * sizeof(FluidPressure), fluidBufferUsage, fluidMemoryUsage);
	this->_renderer.CreateBuffer(this->_buffFluidDivergence, Constants::NumVoxelGridCells * sizeof(FluidDivergence), fluidBufferUsage, fluidMemoryUsage);
	this->_renderer.CreateBuffer(this->_buffFluidFlags, Constants::NumVoxelGridCells * sizeof(FluidFlags), fluidBufferUsage, fluidMemoryUsage);
	this->_renderer.CreateBuffer(this->_buffFluidDebug, Constants::NumVoxelGridCells * sizeof(FluidDebug), fluidBufferUsage, fluidMemoryUsage);
	this->_renderer.CreateBuffer(this->_buffFluidBrickOffsets, Constants::NumVoxelGridCells * sizeof(FluidBrickOffsets), fluidBufferUsage, fluidMemoryUsage);
	this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
		vkCmdFillBuffer(cmd, this->_buffFluidVelocity.bufferHandle, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, this->_buffFluidDensity.bufferHandle, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, this->_buffFluidPressure.bufferHandle, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, this->_buffFluidDivergence.bufferHandle, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, this->_buffFluidFlags.bufferHandle, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, this->_buffFluidDebug.bufferHandle, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, this->_buffFluidBrickOffsets.bufferHandle, 0, VK_WHOLE_SIZE, 0);
	});

	this->_renderer.CreateBuffer(
		this->_buffFluidGridReferences, sizeof(FluidGridReferences),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY
	);
	this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd){
		FluidGridReferences refs = {
			.velocityBufferReference = this->_buffFluidVelocity.deviceAddress,
			.densityBufferReference = this->_buffFluidDensity.deviceAddress,
			.pressureBufferReference = this->_buffFluidPressure.deviceAddress,
			.divergenceBufferReference = this->_buffFluidDivergence.deviceAddress,
			.flagsBufferReference = this->_buffFluidFlags.deviceAddress,
			.debugBufferReference = this->_buffFluidDebug.deviceAddress,
		};
		vkCmdUpdateBuffer(cmd, this->_buffFluidGridReferences.bufferHandle, 0, sizeof(refs), &refs);
	});
	return true;
}

bool
UniformFluidEngine::initPipelines()
{
	// GRAPHICS
	this->_renderer.CreateGraphicsPipeline(this->_graphicsRenderMesh, SHADER_DIRECTORY"/mesh.vert.spv", SHADER_DIRECTORY"/mesh.frag.spv", "", {}, 0, 
                                           sizeof(GraphicsPushConstants), {.blendMode = BlendMode::Additive});

    this->_renderer.CreateGraphicsPipeline(this->_graphicsParticles, SHADER_DIRECTORY"/particles.vert.spv", SHADER_DIRECTORY"/particles.frag.spv", "", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences)),
		BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffParticles.bufferHandle, Constants::NumParticles * sizeof(Particle))
	}, VK_SHADER_STAGE_VERTEX_BIT, sizeof(ParticlesPushConstants),
	{.inputTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST, .blendMode = BlendMode::Additive, .pushConstantsStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT});

	this->_renderer.CreateGraphicsPipeline(this->_graphicsGridLines, SHADER_DIRECTORY"/line.vert.spv", SHADER_DIRECTORY"/line.frag.spv", "", {}, 0,
											sizeof(DrawLinesPushConstants), {.inputTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST});
	
	// COMPUTE
    this->_renderer.CreateComputePipeline(this->_computeRaycastVoxelGrid, SHADER_DIRECTORY"/grid_tracer.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences)),
        ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_renderer.GetDrawImage().imageView, VK_NULL_HANDLE, this->_renderer.GetDrawImage().layout),
		BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
    },
    sizeof(RayTracerPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAddSources, SHADER_DIRECTORY"/fluid_add_sources.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(AddFluidPropertiesPushConstants));

	// TODO: Refactor these to return Descriptor IDS and use those.
	this->_renderer.CreateComputePipeline(this->_computeDiffuseVelocity, SHADER_DIRECTORY"/fluid_diffuse_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeDiffuseDensity, SHADER_DIRECTORY"/fluid_diffuse_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(FluidPushConstants));
	this->_renderer.CreateComputePipeline(this->_computeAdvectVelocity, SHADER_DIRECTORY"/fluid_advect_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAdvectDensity, SHADER_DIRECTORY"/fluid_advect_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeDivergence, SHADER_DIRECTORY"/fluid_compute_divergence.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeSolvePressure, SHADER_DIRECTORY"/fluid_solve_pressure.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeProjectIncompressible, SHADER_DIRECTORY"/fluid_project_incompressible.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGridReferences.bufferHandle, sizeof(FluidGridReferences))
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeGenerateGridLines, SHADER_DIRECTORY"/grid_generate_lines.comp.spv", {
	}, sizeof(GenerateLinesPushConstants));
	return true;	
}

bool
UniformFluidEngine::initPreProcess()
{
	return true;
	// y*z, x*y, x*z
	constexpr glm::uvec4 dim = Constants::VoxelGridDimensions;
	constexpr glm::vec3 center = Constants::VoxelGridCenter;
	constexpr float scale = Constants::VoxelCellSize;
	glm::vec4* data = new glm::vec4[Constants::NumGridLines * 2];
	// glm::vec4* data = (glm::vec4*)this->_buffGridLines.info.pMappedData;
	int i = 0;
	// for(int z = 0; z < Constants::VoxelGridDimensions.z; z++) {
	// 	for(int y = 0; y < Constants::VoxelGridDimensions.y; y++) {
	// 		data[i++] = glm::vec4((glm::vec3(0.0, z, y) - center) * scale, 1.0);
	// 		data[i++] = glm::vec4((glm::vec3(dim.x, z, y) - center) * scale, 1.0);
	// 	}
	// }
	for(uint32_t y = 0; y < dim.y; y++) {
		for(uint32_t x = 0; x < dim.x; x+=4) {
			data[i++] = glm::vec4((glm::vec3(x, y, 0.0) - center) * scale/2.0f, 1.0);
			data[i++] = glm::vec4((glm::vec3(x, y, dim.z) - center) * scale/2.0f, 1.0);
		}
	}
	// for(int z = 0; z < Constants::VoxelGridDimensions.z; z++) {
	// 	for(int x = 0; x < Constants::VoxelGridDimensions.x; x++) {
	// 		data[i++] = glm::vec4((glm::vec3(x, 0.0, z) - center) * scale, 1.0);
	// 		data[i++] = glm::vec4((glm::vec3(x, dim.y, z) - center) * scale, 1.0);
	// 	}
	// }
	
	std::cout << i << std::endl;
	std::memcpy(this->_buffStaging.info.pMappedData, data, i * sizeof(glm::vec4));

	this->_renderer.ImmediateSubmit([this, i](VkCommandBuffer cmd) {
		VkBufferCopy copy = {
			.size = i * sizeof(glm::vec4)
			// .size = VK_WHOLE_SIZE
		};
		vkCmdCopyBuffer(cmd, this->_buffStaging.bufferHandle, this->_gridMesh.vertexBuffer.bufferHandle, 1, &copy);
	});
	return true;
}
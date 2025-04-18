// #include "uniform_fluid_engine.hpp"
#include "wf_fluid_engine.hpp"
#include "wf_structs.hpp"
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
#include <bit>
#include <vulkan/vulkan_core.h>

#include "vk_initializers.h"
#include "vk_loader.h"

namespace wf {
namespace Constants
{

// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(16, 8, 16, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(32, 16, 32, 1);
constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(64, 32, 64, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(128, 64, 128, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(160, 80, 160, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(256, 128, 256, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(512, 128, 512, 1);

// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(128*2, 32*2, 64*2, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(96, 32, 96, 1.0);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(256, 96, 256, 1) ;
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(128, 64, 128, 1);
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(64, 256, 64, 1);

constexpr size_t VoxelGridResolution = VoxelGridDimensions.x/4;

const uint32_t NumVoxelGridCells = VoxelGridDimensions.x * VoxelGridDimensions.y * VoxelGridDimensions.z;
const float VoxelGridScale = 2.0f;
const uint32_t VoxelDiagonal = VoxelGridDimensions.x + VoxelGridDimensions.y + VoxelGridDimensions.z;
constexpr glm::vec3 VoxelGridCenter = glm::vec3(VoxelGridDimensions) * 0.5f + glm::vec3(1.0);
constexpr float VoxelCellSize = 1.0 / VoxelGridResolution;

constexpr uint32_t NumGridLines = VoxelGridDimensions.y*VoxelGridDimensions.z + VoxelGridDimensions.x*VoxelGridDimensions.y + VoxelGridDimensions.x*VoxelGridDimensions.z;
const glm::uvec3 GridGroups = glm::uvec3(VoxelGridDimensions.y*VoxelGridDimensions.z, VoxelGridDimensions.x*VoxelGridDimensions.y, VoxelGridDimensions.x*VoxelGridDimensions.z);

constexpr glm::uvec3 LocalGroupSize = glm::uvec3(4, 4, 4);

constexpr uint32_t NumAdvectionIterations = 1;
constexpr uint32_t NumDiffusionIterations = 4;
constexpr uint32_t NumPressureIterations = 6;

constexpr glm::vec4 LightPosition = glm::vec4(500.0, 500.0, 400, 1.0);

constexpr uint32_t NumParticles = 65536;
constexpr float MaxParticleLifetime = 240.0;

const std::string MeshFile = ASSETS_DIRECTORY"/meshes/filledwing.glb";
}

/* FUNCTIONS */
static void
rollingAverage(float& currentValue, float newValue) {
	const float alpha = 0.15f;
	currentValue = alpha * newValue + (1.0f - alpha) * currentValue;
}

/* CLASS */
WorldFlow::WorldFlow(Renderer& renderer, Settings settings)
	: _renderer(renderer), _settings(settings), _diffusionIterations(Constants::NumDiffusionIterations), _pressureIterations(Constants::NumPressureIterations),
	  _advectionIterations(Constants::NumAdvectionIterations), _rendererSubgridLimit(settings.numGridLevels-1), _grid{.numSubgrids = settings.numGridLevels},
	  _objMeshes(10) 
	  {}

WorldFlow::~WorldFlow() {}

bool
WorldFlow::Init()
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
WorldFlow::update(VkCommandBuffer cmd, float dt)
{
	checkControls(this->_renderer.GetKeyMap(), this->_renderer.GetMouseMap(), this->_renderer.GetMouse(), dt);

	dt = (this->_useTickRate) ? this->_tickRate : dt * this->_tickRate;
	this->_timestamps.reset(this->_renderer.GetDevice());
	this->_timestamps.write(cmd, Timestamps::StartFrame, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		
	if(!this->_shouldPause) {
		generateSubgridOffsets(cmd);
		generateIndirectCommands(cmd);
		this->_timestamps.write(cmd, Timestamps::GenerateCommands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		addSources(cmd, dt);
		voxelRasterizeGeometry(cmd, this->_objMeshes[0]);
		this->_timestamps.write(cmd, Timestamps::AddFluidProperties, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		diffuseVelocity(cmd, dt);
		this->_timestamps.write(cmd, Timestamps::VelocityDiffusion, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		advectVelocity(cmd, dt);
		this->_timestamps.write(cmd, Timestamps::VelocityAdvect, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);


		computeDivergence(cmd);
		solvePressure(cmd);
		if(this->_shouldProjectIncompressible)
			projectIncompressible(cmd, dt);
		this->_timestamps.write(cmd, Timestamps::PressureSolve, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		restrictVelocities(cmd);

		diffuseDensity(cmd, dt);
		this->_timestamps.write(cmd, Timestamps::DensityDiffusion, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		advectDensity(cmd, dt);
		this->_timestamps.write(cmd, Timestamps::DensityAdvect, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	}

	if(this->_shouldRenderFluid)
		renderVoxelVolume(cmd);

	// renderMesh(cmd, this->_objMeshes[0]);

	this->_timestamps.write(cmd, Timestamps::FluidRender, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void
WorldFlow::generateSubgridOffsets(VkCommandBuffer cmd)
{
	// generate subgrid access offsets at finer resolutions for active cell chunks
	this->_computeGenerateSubgridOffsets.Bind(cmd);
	for(uint32_t i = 1; i < this->_grid.numSubgrids; i++) {
		SubGrid& sg = this->_grid.subgrids[i];
		uint32_t clearIndexCount = 0;
		vkCmdUpdateBuffer(cmd, sg.buffGpuReferences.bufferHandle, offsetof(SubGridGpuReferences, indexCount), sizeof(uint32_t), &clearIndexCount);
		VkBufferMemoryBarrier clearBarrier = sg.buffGpuReferences.CreateBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 1, &clearBarrier, 0, 0);

		const SubgridLevelPushConstants pc = {
			.subgridLevel = i-1
		};
		vkCmdPushConstants(cmd, this->_computeGenerateSubgridOffsets.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		dispatchFluid(cmd, this->_grid.subgrids[i-1]);

		VkBufferMemoryBarrier barrier = this->_grid.subgrids[i-1].buffGpuReferences.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 1, &barrier, 0, 0);
	}
}

void
WorldFlow::generateIndirectCommands(VkCommandBuffer cmd)
{
	this->_computeGenerateIndirectCommands.Bind(cmd);
	for(uint32_t i = 1; i < this->_grid.numSubgrids; i++) {
		GenerateIndirectCommandPushConstants pc = {
			.subgridLevel = i,
			.groupDimensionLimit = 65535
		};
		vkCmdPushConstants(cmd, this->_computeGenerateIndirectCommands.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cmd, 1, 1, 1);
	}
	
	VkBufferMemoryBarrier barriers[Constants::MAX_SUBGRID_LEVELS];
	for(uint32_t i = 1; i < this->_grid.numSubgrids; i++) {
		barriers[i-1] = this->_grid.subgrids[i].buffDispatchCommand.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 0, 0, this->_grid.numSubgrids-1, barriers, 0, 0);
}

void
WorldFlow::addSources(VkCommandBuffer cmd, float dt)
{
	this->_computeAddSources.Bind(cmd);
	// for(uint32_t i = 0; i < this->_grid.numSubgrids; i++) {
	for(int32_t i = this->_grid.numSubgrids-1; i >= 0; i--) {
		SubGrid& sg = this->_grid.subgrids[i];
		const AddFluidPropertiesPushConstants pc = {
			.sourcePosition = glm::vec4(this->_sourcePosition, 1.0) * (float)sg.resolution.w,
			.velocity = glm::vec4(this->_velocitySourceAmount, 1.0),
			.objectPosition = this->_objectPosition,
			.activationWeights = this->_activationWeights,
			.elapsed = this->_renderer.GetElapsedTime(),
			.dt = dt,
			.sourceRadius = this->_sourceRadius / sg.cellSize,
			.addVelocity = this->_shouldAddSources,
			.addDensity = this->_shouldAddSources,
			.density = this->_densityAmount,
			.objectType = (uint32_t)this->_shouldAddObstacle > 0,
			.objectRadius = this->_objectRadius,
			.decayRate = this->_decayRate,
			.clear = this->_shouldClear,
			.subgridLevel = (uint32_t)i,
			.activationThreshold = this->_activationThreshold
		};
		vkCmdPushConstants(cmd, this->_computeAddSources.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		dispatchFluid(cmd, sg);
		VkBufferMemoryBarrier barriers[] = {
			sg.buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
			sg.buffFluidDensity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
			sg.buffFluidPressure.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
			sg.buffFluidDivergence.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
			sg.buffFluidFlags.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
			sg.buffFluidDebug.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
	this->_shouldClear = false;
}

void
WorldFlow::diffuseVelocity(VkCommandBuffer cmd, float dt)
{
	for(uint32_t s = 0; s < this->_grid.numSubgrids; s++) {
		SubGrid& sg = this->_grid.subgrids[s];
		this->_computeDiffuseVelocity.Bind(cmd);
		for(uint32_t i = 0; i < this->_diffusionIterations; i++) {
			if(s != 0) break;
			DiffusionPushConstants pc = {
				.dt = dt,
				.redBlack = ((i+1) % 2),
				.subgridLevel = s,
				.diffusionRate = this->_diffusionRate * 10e-5f
			};
			vkCmdPushConstants(cmd, this->_computeDiffuseVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			
			dispatchFluid(cmd, sg);

			VkBufferMemoryBarrier barriers[] = {
				this->_grid.subgrids[s].buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
		}
		if(s < this->_grid.numSubgrids-1) {
			// prolongVelocity(cmd, s);
		}
	}
}

void
WorldFlow::diffuseDensity(VkCommandBuffer cmd, float dt)
{
	for(uint32_t s = 0; s < this->_grid.numSubgrids; s++) {
		SubGrid& sg = this->_grid.subgrids[s];
		this->_computeDiffuseDensity.Bind(cmd);
		for(uint32_t i = 0; i < this->_diffusionIterations; i++) {
			DiffusionPushConstants pc = {
				.dt = dt,
				.redBlack = ((i+1) % 2),
				.subgridLevel = s,
				.diffusionRate = this->_diffusionRate * 10e-5f
			};
			vkCmdPushConstants(cmd, this->_computeDiffuseDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

			dispatchFluid(cmd, sg);

			VkBufferMemoryBarrier barriers[] = {
				this->_grid.subgrids[s].buffFluidDensity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
		}
	}
}

void
WorldFlow::advectVelocity(VkCommandBuffer cmd, float dt)
{
	for(uint32_t s = 0; s < this->_grid.numSubgrids; s++) {
		SubGrid& sg = this->_grid.subgrids[s];
		this->_computeAdvectVelocity.Bind(cmd);
		
		for(uint32_t i = 0; i < this->_advectionIterations; i++) {
			FluidPushConstants pc = {
				.dt = dt/(float)this->_advectionIterations,
				.subgridLevel = s
			};
			vkCmdPushConstants(cmd, this->_computeAdvectVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FluidPushConstants), &pc);
			dispatchFluid(cmd, sg);
			VkBufferMemoryBarrier barriers[] = {
				this->_grid.subgrids[s].buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
		}
		if(s < this->_grid.numSubgrids-1) {
			prolongDensity(cmd, s); // prolong diffusion results onto finer grid level
		}
	}
}

void
WorldFlow::advectDensity(VkCommandBuffer cmd, float dt)
{
	for(uint32_t s = 0; s < this->_grid.numSubgrids; s++) {
		SubGrid& sg = this->_grid.subgrids[s];
		this->_computeAdvectDensity.Bind(cmd);

		for(uint32_t i = 0; i < this->_advectionIterations; i++) {
			FluidPushConstants pc = {
				.dt = dt/(float)this->_advectionIterations,
				.subgridLevel = s
			};
			vkCmdPushConstants(cmd, this->_computeAdvectDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FluidPushConstants), &pc);
			dispatchFluid(cmd, sg);
			VkBufferMemoryBarrier barriers[] = {
				this->_grid.subgrids[s].buffFluidDensity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
		}
	}
}

void
WorldFlow::computeDivergence(VkCommandBuffer cmd)
{
	this->_computeDivergence.Bind(cmd);
	for(uint32_t s = 0; s < this->_grid.numSubgrids; s++) {
		SubGrid& sg = this->_grid.subgrids[s];
		FluidPushConstants pc {
			.subgridLevel = s
		};
		vkCmdPushConstants(cmd, this->_computeDivergence.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		dispatchFluid(cmd, sg);
		VkBufferMemoryBarrier barriers[] = {
			this->_grid.subgrids[s].buffFluidDivergence.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
			this->_grid.subgrids[s].buffFluidVorticity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
WorldFlow::solvePressure(VkCommandBuffer cmd)
{
	for(uint32_t s = 0; s < this->_grid.numSubgrids; s++) {
		SubGrid& sg = this->_grid.subgrids[s];
		this->_computeSolvePressure.Bind(cmd);

		uint32_t iters = this->_pressureIterations + (this->_pressureIterations*(s*this->_iterationSubgridFactor));
		for(uint32_t i = 0; i < iters; i++) {
			FluidPushConstants pc = {
				.redBlack = ((i+1) % 2),
				.subgridLevel = s
			};
			vkCmdPushConstants(cmd, this->_computeSolvePressure.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

			dispatchFluid(cmd, sg);

			VkBufferMemoryBarrier barriers[] = {
				this->_grid.subgrids[s].buffFluidPressure.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
		}
	}
}

void
WorldFlow::projectIncompressible(VkCommandBuffer cmd, float dt)
{
	for(uint32_t s = 0; s < this->_grid.numSubgrids; s++) {
		SubGrid& sg = this->_grid.subgrids[s];
		this->_computeProjectIncompressible.Bind(cmd);
		ProjectIncompressiblePushConstants pc {
			.dt = dt,
			.subgridLevel = s,
			.fluidDensity = this->_fluidDensity
		};
		vkCmdPushConstants(cmd, this->_computeProjectIncompressible.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		dispatchFluid(cmd, sg);
		VkBufferMemoryBarrier barriers[] = {
			this->_grid.subgrids[s].buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
	}
}

void
WorldFlow::prolongDensity(VkCommandBuffer cmd, uint32_t coarseGridLevel)
{
	const uint32_t fineGridSublevel = coarseGridLevel + 1;
	this->_computeProlongDensity.Bind(cmd);
	SubgridTransferPushConstants pc = {
		.subgridLevel = fineGridSublevel,
		.alpha = this->_transferAlpha
	};
	vkCmdPushConstants(cmd, this->_computeProlongDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	dispatchFluid(cmd, this->_grid.subgrids[fineGridSublevel]);
	VkBufferMemoryBarrier barrier = this->_grid.subgrids[fineGridSublevel].buffFluidDensity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 1, &barrier, 0, 0);
}

void
WorldFlow::prolongVelocity(VkCommandBuffer cmd, uint32_t coarseGridLevel)
{
	const uint32_t fineGridSublevel = coarseGridLevel + 1;
	this->_computeProlongVelocity.Bind(cmd);
	SubgridTransferPushConstants pc = {
		.subgridLevel = fineGridSublevel,
		.alpha = this->_transferAlpha
	};
	vkCmdPushConstants(cmd, this->_computeProlongVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	dispatchFluid(cmd, this->_grid.subgrids[fineGridSublevel]);
	VkBufferMemoryBarrier barrier = this->_grid.subgrids[fineGridSublevel].buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 1, &barrier, 0, 0);
}

void
WorldFlow::restrictVelocities(VkCommandBuffer cmd)
{
	this->_computeRestrictVelocity.Bind(cmd);
	for(uint32_t s = this->_grid.numSubgrids-1; s > 0; s--) {
		wf::SubGrid sg = this->_grid.subgrids[s];
		SubgridTransferPushConstants pc = {
			.subgridLevel = s,
			.alpha = this->_restrictionAlpha
		};
		vkCmdPushConstants(cmd, this->_computeRestrictVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		dispatchFluid(cmd, sg);
		VkBufferMemoryBarrier barriers[] = {
			this->_grid.subgrids[s-1].buffFluidVelocity.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, ARRLEN(barriers), barriers, 0, 0);
	}
}

void
WorldFlow::renderVoxelVolume(VkCommandBuffer cmd)
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
		.maxDistance = Constants::VoxelDiagonal*2,
		.stepSize = 0.1f,
		.gridSize = glm::vec3(Constants::VoxelGridDimensions),
		.gridScale = Constants::VoxelGridScale,
		.lightSource = Constants::LightPosition,
		.baseColor = glm::vec4(0.8, 0.8, 0.8, 1.0),
		.renderType = this->_renderType,
		.rootGridLevel = this->_rendererSubgridBegin,
		.subgridLimit = this->_rendererSubgridLimit
	};
	vkCmdPushConstants(cmd, this->_computeRaycastVoxelGrid.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RayTracerPushConstants), &rtpc);
	VkExtent3D groupCounts = this->_renderer.GetWorkgroupCounts(8);
	vkCmdDispatch(cmd, groupCounts.width, groupCounts.height, groupCounts.depth);
}

void
WorldFlow::renderMesh(VkCommandBuffer cmd, Mesh& mesh, const glm::mat4& transform)
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
WorldFlow::voxelRasterizeGeometry(VkCommandBuffer cmd, Mesh& mesh)
{
	VkExtent3D voxExtent = this->_voxelImage.imageExtent;
	VkRenderingAttachmentInfo colorAttachmentInfo = vkinit::attachment_info(this->_voxelImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(VkExtent2D{voxExtent.width, voxExtent.height}, &colorAttachmentInfo, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_graphicsVoxelRaster.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->_graphicsVoxelRaster.layout, 0, 1, this->_graphicsVoxelRaster.descriptorSets.data(), 0, nullptr);

	VkViewport viewport = {
		.x = 0,
		.y = 0,
		.width = (float)voxExtent.width,
		.height = (float)voxExtent.height,
		.minDepth = 0.0,
		.maxDepth = 1.0
	};
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {
		.offset = { .x = 0, .y = 0 },
		.extent = { .width = voxExtent.width, .height = voxExtent.height}
	};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	glm::mat4 transf = glm::translate(glm::vec3(this->_objectPosition)) * glm::scale(glm::vec3(this->_objectPosition.w));
	GraphicsPushConstants pc = {
		.worldMatrix = transf,
		.vertexBuffer = mesh.vertexBufferAddress
	};
	vkCmdPushConstants(cmd, this->_graphicsVoxelRaster.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	vkCmdBindIndexBuffer(cmd, mesh.indexBuffer.bufferHandle, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, mesh.numIndices, 1, 0, 0, 0);
	vkCmdEndRendering(cmd);
	
	// VkImageMemoryBarrier imgBarrier = this->_voxelImage.CreateBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
	// vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &imgBarrier);
}

void
WorldFlow::renderParticles(VkCommandBuffer cmd, float dt)
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
}

void
WorldFlow::generateGridLines(VkCommandBuffer cmd)
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
WorldFlow::drawGrid(VkCommandBuffer cmd)
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
WorldFlow::dispatchFluid(VkCommandBuffer cmd, const SubGrid& sg, const glm::uvec3& factor)
{
	if(sg.level == 0) {
		const glm::uvec3 groups = (glm::uvec3(sg.resolution) / Constants::LocalGroupSize) / factor;
		vkCmdDispatch(cmd, groups.x, groups.y, groups.z);
	} else {
		vkCmdDispatchIndirect(cmd, sg.buffDispatchCommand.bufferHandle, 0);
	}
}

void
WorldFlow::preFrame()
{
	this->_timestamps.collect(this->_renderer.GetDevice());
	this->_timestamps.nextFrame();

	const uint32_t statReportPeriod = 7;
	this->_timestampAverages[Timestamps::StartFrame] = this->_timestamps.getDelta(Timestamps::StartFrame, Timestamps::FluidRender);
	this->_timestampAverages[Timestamps::GenerateCommands] = this->_timestamps.getDelta(Timestamps::StartFrame, Timestamps::GenerateCommands);
	this->_timestampAverages[Timestamps::AddFluidProperties] = this->_timestamps.getDelta(Timestamps::GenerateCommands, Timestamps::AddFluidProperties);
	this->_timestampAverages[Timestamps::VelocityDiffusion] =  this->_timestamps.getDelta(Timestamps::AddFluidProperties, Timestamps::VelocityDiffusion);
	this->_timestampAverages[Timestamps::VelocityAdvect] =  this->_timestamps.getDelta(Timestamps::VelocityDiffusion, Timestamps::VelocityAdvect);
	this->_timestampAverages[Timestamps::PressureSolve] =  this->_timestamps.getDelta(Timestamps::VelocityAdvect, Timestamps::PressureSolve);
	this->_timestampAverages[Timestamps::DensityDiffusion] =  this->_timestamps.getDelta(Timestamps::PressureSolve, Timestamps::DensityDiffusion);
	this->_timestampAverages[Timestamps::DensityAdvect] =  this->_timestamps.getDelta(Timestamps::DensityDiffusion, Timestamps::DensityAdvect);
	this->_timestampAverages[Timestamps::FluidRender] =  this->_timestamps.getDelta(Timestamps::DensityAdvect, Timestamps::FluidRender);
}

void
WorldFlow::drawUI()
{
	if(this->_shouldHideUI) {
		return;
	}

    ImGuiIO& io = ImGui::GetIO();

    ImGuiViewport* viewport = ImGui::GetMainViewport(); // Use GetMainViewport for multi-viewport support

	float pad = 10.0;
    // ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    // ImGui::SetNextWindowSize(ImVec2(0, 0)); // Auto-sizing

	ImVec2 windowSize;

	if(ImGui::Begin("stats", nullptr)) {
		const float frameTime = this->_timestampAverages[Timestamps::StartFrame];
		ImGui::Text("FPS:       %3.2f", 1.0/(frameTime/1000.0));
		ImGui::Text("Total:     %3.2f ms", frameTime);
		ImGui::Text("GenCmd:    %3.2f ms", this->_timestampAverages[Timestamps::GenerateCommands]);
		ImGui::Text("AddSrc:    %3.2f ms", this->_timestampAverages[Timestamps::AddFluidProperties]);
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

    // ImGui::SetNextWindowPos(ImVec2(pad, windowSize.y + pad), ImGuiCond_Always);
    // ImGui::SetNextWindowSize(ImVec2(0, 0)); // Auto-sizing
	if(ImGui::Begin("controls", nullptr)) {
		ImGui::DragFloat("obj", &this->_objectRadius, 0.01f);
		ImGui::DragFloat("src", &this->_sourceRadius, 0.01f);
		ImGui::DragFloat("dns", &this->_densityAmount, 0.1f);
		ImGui::DragFloat("decay", &this->_decayRate, 0.01f);
		ImGui::DragFloat("vel", &this->_velocitySpeed, 0.25f);
		ImGui::InputFloat3("sourcePos", glm::value_ptr(this->_sourcePosition));
		// ImGui::DragFloat("objOff", &this->_objectOffset, 0.0025f);
		ImGui::DragFloat4("objPos", glm::value_ptr(this->_objectPosition), 0.0025f);
		this->_shouldClear = ImGui::Button("clear");
	}
	ImGui::End();

	if(ImGui::Begin("parameters", nullptr)) {
		const uint32_t step = 1;
		ImGui::InputScalar("rndrlvl start", ImGuiDataType_U32, &this->_rendererSubgridBegin, &step);
		ImGui::InputScalar("rndrlvl end", ImGuiDataType_U32, &this->_rendererSubgridLimit, &step);
		ImGui::DragFloat("activation", &this->_activationThreshold, 0.01f);
		ImGui::DragFloat4("vrt/vel/dns/prs", glm::value_ptr(this->_activationWeights), 0.01f);
		ImGui::InputScalar("diffiter", ImGuiDataType_U32, &this->_diffusionIterations, &step);
		ImGui::InputScalar("presiter", ImGuiDataType_U32, &this->_pressureIterations, &step);
		ImGui::InputScalar("iterfactor", ImGuiDataType_U32, &this->_iterationSubgridFactor, &step);
		ImGui::InputScalar("advectiter", ImGuiDataType_U32, &this->_advectionIterations, &step);
		ImGui::DragFloat("transferAlpha", &this->_transferAlpha, 0.01f);
		ImGui::DragFloat("arestrictionAlpha", &this->_restrictionAlpha, 0.01f);
		ImGui::DragFloat("diffusionRate", &this->_diffusionRate, 0.01f);
		ImGui::DragFloat("pressureDensity", &this->_fluidDensity, 0.0001f);
	}
	ImGui::End();
}

void
WorldFlow::checkControls(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse, float dt)
{
	if(keyMap[SDLK_F4]) {
		this->_renderer.Close();
	}

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
	// this->_objectPosition = glm::vec3(this->_objectOffset, 0.0, 0.0);
	this->_shouldAddSources = false;
	// int offset = (int)((this->_sourceRadius * Constants::VoxelGridResolution / 2.0f));
	int offset = (int)(this->_sourceRadius);
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

	if(keyMap[SDLK_d]) {
		this->_shouldClear = true;
		keyMap[SDLK_d] = false;
	}
	
	if(keyMap[SDLK_TAB]) {
		this->_shouldHideUI = !this->_shouldHideUI;
		keyMap[SDLK_TAB] = false;
	}

	if(keyMap[SDLK_SPACE]) {
		this->_shouldPause = !this->_shouldPause;
		keyMap[SDLK_SPACE] = false;
	}


	for(int i = 1; i <= 9; i++) {
		if(keyMap[SDLK_0 + i] && keyMap[SDLK_LSHIFT]) {
			this->_renderType = i;
			keyMap[SDLK_0 + i] = false;
		}
	}
}

bool
WorldFlow::initRendererOptions()
{
	this->_renderer.RegisterPreFrameCallback(std::bind(&WorldFlow::preFrame, this));
	this->_renderer.RegisterUpdateCallback(std::bind(&WorldFlow::update, this, std::placeholders::_1, std::placeholders::_2));
	this->_renderer.RegisterUICallback(std::bind(&WorldFlow::drawUI, this));

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
WorldFlow::initResources()
{
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
	
	// VOXEL RASTERIZATION ALLOCATION
	std::vector<std::vector<Vertex>> vertexBuffers;
	std::vector<std::vector<uint32_t>> indexBuffers;
	if(!loadGltfMeshes(Constants::MeshFile, vertexBuffers, indexBuffers)) {
		std::cout << "[ERROR] Failed to load meshes" << std::endl;
	}
	for(int m = 0; m < vertexBuffers.size(); m++) {
		this->_renderer.UploadMesh(this->_objMeshes[m], vertexBuffers[m], indexBuffers[m]);
		std::cout << "Mesh Triangles: " << indexBuffers[m].size() / 3 << std::endl;
	}

	glm::uvec3 finestDimension = Constants::VoxelGridDimensions * (uint32_t)std::pow(this->_settings.gridSubdivision, this->_settings.numGridLevels-1);
	uint32_t maxDimension = std::max(std::max(finestDimension.x, finestDimension.y), finestDimension.z);
	this->_renderer.CreateImage(this->_voxelImage,
		VkExtent3D{maxDimension, maxDimension, 1},
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	);
	this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
		this->_voxelImage.Clear(cmd, {1.0, 0.0, 0.0, 1.0});
	});

	// WORLDFLOW GRID ALLOCATION //
	VkBufferUsageFlags fluidBufferUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaMemoryUsage fluidMemoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
	// MASTER GRID
	this->_renderer.CreateBuffer(this->_grid.buffWorldFlowGridGpu, sizeof(WorldFlowGridGpu), fluidBufferUsage, fluidMemoryUsage);

	// FLUID PROPERTY BUFFERS
	for(uint32_t i = 0; i < this->_grid.numSubgrids; i++) {
		wf::SubGrid& sg = this->_grid.subgrids[i];
		// uint32_t subdivision = (i > 0) ? this->_settings.gridSubdivision*i : 1;
		uint32_t subdivision = (uint32_t)std::pow(this->_settings.gridSubdivision, i);

		sg.level = i;
		sg.resolution = glm::uvec4(glm::uvec3(Constants::VoxelGridDimensions) * subdivision, subdivision);
		sg.center = glm::vec4(0.0);
		sg.cellSize = Constants::VoxelCellSize / subdivision;
		uint32_t numCells = sg.resolution.x * sg.resolution.y * sg.resolution.z;

		this->_renderer.CreateBuffer(sg.buffGpuReferences, sizeof(SubGridGpuReferences), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffDispatchCommand, sizeof(DispatchIndirectCommand), fluidBufferUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidVelocity, numCells * sizeof(FluidVelocity), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidDensity, numCells * sizeof(FluidDensity), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidPressure, numCells * sizeof(FluidPressure), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidDivergence, numCells * sizeof(FluidDivergence), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidFlags, numCells * sizeof(FluidFlags), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidDebug, numCells * sizeof(FluidDebug), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidIndexOffsets, numCells * sizeof(FluidIndexOffsets), fluidBufferUsage, fluidMemoryUsage);
		this->_renderer.CreateBuffer(sg.buffFluidVorticity, numCells * sizeof(FluidVorticity), fluidBufferUsage, fluidMemoryUsage);
		
		this->_grid.gpuRefs.subgridReferences[i] = sg.buffGpuReferences.deviceAddress;
		this->_grid.gpuRefs.subgridCount = this->_grid.numSubgrids;
		std::cout << "Subgrid: " << glm::to_string(sg.resolution) << " " << sg.cellSize << std::endl;
	}

	this->_renderer.ImmediateSubmit([this](VkCommandBuffer cmd) {
		for(uint32_t i = 0; i < this->_grid.numSubgrids; i++) {
			wf::SubGrid& sg = this->_grid.subgrids[i];
			vkCmdFillBuffer(cmd, sg.buffFluidVelocity.bufferHandle, 0, VK_WHOLE_SIZE, 0);
			vkCmdFillBuffer(cmd, sg.buffFluidDensity.bufferHandle, 0, VK_WHOLE_SIZE, 0);
			vkCmdFillBuffer(cmd, sg.buffFluidPressure.bufferHandle, 0, VK_WHOLE_SIZE, 0);
			vkCmdFillBuffer(cmd, sg.buffFluidDivergence.bufferHandle, 0, VK_WHOLE_SIZE, 0);
			vkCmdFillBuffer(cmd, sg.buffFluidFlags.bufferHandle, 0, VK_WHOLE_SIZE, 0);
			vkCmdFillBuffer(cmd, sg.buffFluidDebug.bufferHandle, 0, VK_WHOLE_SIZE, 0);
			vkCmdFillBuffer(cmd, sg.buffFluidIndexOffsets.bufferHandle, 0, VK_WHOLE_SIZE, 0);
			vkCmdFillBuffer(cmd, sg.buffDispatchCommand.bufferHandle, 0, VK_WHOLE_SIZE, 1);

			SubGridGpuReferences refs = {
				.velocityBufferReference = sg.buffFluidVelocity.deviceAddress,
				.densityBufferReference = sg.buffFluidDensity.deviceAddress,
				.pressureBufferReference = sg.buffFluidPressure.deviceAddress,
				.divergenceBufferReference = sg.buffFluidDivergence.deviceAddress,
				.flagsBufferReference = sg.buffFluidFlags.deviceAddress,
				.debugBufferReference = sg.buffFluidDebug.deviceAddress,
				.indexOffsetsBufferReference = sg.buffFluidIndexOffsets.deviceAddress,
				.dispatchCommandReference = sg.buffDispatchCommand.deviceAddress,
				.vorticityBufferReference = sg.buffFluidVorticity.deviceAddress,

				.resolution = sg.resolution,
				.center = sg.center,
				.cellSize = sg.cellSize,
				.indexCount = 0
			};
			vkCmdUpdateBuffer(cmd, sg.buffGpuReferences.bufferHandle, 0, sizeof(refs), &refs);
		}
		vkCmdUpdateBuffer(cmd, this->_grid.buffWorldFlowGridGpu.bufferHandle, 0, sizeof(WorldFlowGridGpu), &this->_grid.gpuRefs);
	});
	return true;
}

bool
WorldFlow::initPipelines()
{
	// GRAPHICS
	this->_renderer.CreateGraphicsPipeline(this->_graphicsRenderMesh, SHADER_DIRECTORY"/mesh.vert.spv", SHADER_DIRECTORY"/mesh.frag.spv", "", {}, 0, 
                                           sizeof(GraphicsPushConstants), {.blendMode = BlendMode::Additive});

	this->_renderer.CreateGraphicsPipeline(this->_graphicsVoxelRaster, SHADER_DIRECTORY"/voxel_raster.vert.spv", SHADER_DIRECTORY"/voxel_raster.frag.spv", SHADER_DIRECTORY"/voxel_raster.geom.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	VK_SHADER_STAGE_ALL_GRAPHICS, sizeof(GraphicsPushConstants),
	{.conservativeRasterization = 0.0f});

    this->_renderer.CreateGraphicsPipeline(this->_graphicsParticles, SHADER_DIRECTORY"/particles.vert.spv", SHADER_DIRECTORY"/particles.frag.spv", "", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffParticles.bufferHandle, Constants::NumParticles * sizeof(Particle))
	}, VK_SHADER_STAGE_VERTEX_BIT, sizeof(ParticlesPushConstants),
	{.inputTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST, .blendMode = BlendMode::Additive, .pushConstantsStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT});

	this->_renderer.CreateGraphicsPipeline(this->_graphicsGridLines, SHADER_DIRECTORY"/line.vert.spv", SHADER_DIRECTORY"/line.frag.spv", "", {}, 0,
											sizeof(DrawLinesPushConstants), {.inputTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST});
											
	// COMPUTE
    this->_renderer.CreateComputePipeline(this->_computeRaycastVoxelGrid, SHADER_DIRECTORY"/grid_tracer.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
        ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_renderer.GetDrawImage().imageView, VK_NULL_HANDLE, this->_renderer.GetDrawImage().layout),
    },
    sizeof(RayTracerPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeGenerateSubgridOffsets, SHADER_DIRECTORY"/fluid_generate_subgrid_offsets.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(SubgridLevelPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeGenerateIndirectCommands, SHADER_DIRECTORY"/fluid_generate_indirect_commands.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(GenerateIndirectCommandPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAddSources, SHADER_DIRECTORY"/fluid_add_sources.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(AddFluidPropertiesPushConstants));

	// TODO: Refactor these to return Descriptor IDS and use those.
	this->_renderer.CreateComputePipeline(this->_computeDiffuseVelocity, SHADER_DIRECTORY"/fluid_diffuse_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(DiffusionPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeDiffuseDensity, SHADER_DIRECTORY"/fluid_diffuse_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(DiffusionPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAdvectVelocity, SHADER_DIRECTORY"/fluid_advect_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeAdvectDensity, SHADER_DIRECTORY"/fluid_advect_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeDivergence, SHADER_DIRECTORY"/fluid_compute_divergence.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeSolvePressure, SHADER_DIRECTORY"/fluid_solve_pressure.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(FluidPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeProjectIncompressible, SHADER_DIRECTORY"/fluid_project_incompressible.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(ProjectIncompressiblePushConstants));

	this->_renderer.CreateComputePipeline(this->_computeProlongDensity, SHADER_DIRECTORY"/fluid_prolong_density.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(SubgridTransferPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeProlongVelocity, SHADER_DIRECTORY"/fluid_prolong_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(SubgridTransferPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeRestrictVelocity, SHADER_DIRECTORY"/fluid_restrict_velocity.comp.spv", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_grid.buffWorldFlowGridGpu.bufferHandle, sizeof(WorldFlowGridGpu)),
	},
	sizeof(SubgridTransferPushConstants));

	this->_renderer.CreateComputePipeline(this->_computeGenerateGridLines, SHADER_DIRECTORY"/grid_generate_lines.comp.spv", {
	}, sizeof(GenerateLinesPushConstants));
	return true;	
}

bool
WorldFlow::initPreProcess()
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
}
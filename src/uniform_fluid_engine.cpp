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
	glm::vec3 velocity;
	float density;
	float pressure;
	float divergence;
	int occupied;
	float padding;
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
};
const auto s = sizeof(AddFluidPropertiesPushConstants);

struct alignas(16) ParticlesPushConstants
{
    glm::mat4 cameraMatrix;
	float dt;
	float elapsed;
	float maxLifetime;
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
constexpr size_t VoxelGridResolution = 64;
// constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(VoxelGridResolution, VoxelGridResolution, VoxelGridResolution, 1);
constexpr glm::uvec4 VoxelGridDimensions = glm::uvec4(256, 32, 16, 1);

const size_t VoxelGridSize = VoxelGridDimensions.x * VoxelGridDimensions.y * VoxelGridDimensions.z * sizeof(FluidGridCell);
const float VoxelGridScale = 2.0f;
const uint32_t VoxelDiagonal = VoxelGridDimensions.x + VoxelGridDimensions.y + VoxelGridDimensions.z;
constexpr glm::vec3 VoxelGridCenter = glm::vec3(VoxelGridDimensions) * 0.5f;
constexpr float VoxelCellSize = 1.0 / VoxelGridResolution;

constexpr uint32_t LocalGroupSize = 8;

constexpr uint32_t NumDiffusionIterations = 4;
constexpr uint32_t NumPressureIterations = 6;

constexpr glm::vec3 LightPosition = glm::vec4(10.0, 10.0, 10.0, 1.0);

constexpr uint32_t NumParticles = 2024;
constexpr float MaxParticleLifetime = 120.0;
}

/* FUNCTIONS */
static void
rollingAverage(float& currentValue, float newValue) {
	const float alpha = 0.15;
	currentValue = alpha * newValue + (1.0f - alpha) * currentValue;
}

/* CLASS */
UniformFluidEngine::UniformFluidEngine(Renderer& renderer)
	: _renderer(renderer), _diffusionIterations(Constants::NumDiffusionIterations), _pressureIterations(Constants::NumPressureIterations) {}

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
	addSources(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::AddFluidProperties, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	diffuseVelocity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::VelocityDiffusion, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	advectVelocity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::VelocityAdvect, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	computeDivergence(cmd);
	solvePressure(cmd);
	projectIncompressible(cmd);
	if(this->_toggle)
		computeDivergence(cmd);
	this->_timestamps.write(cmd, Timestamps::PressureSolve, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	diffuseDensity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::DensityDiffusion, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	advectDensity(cmd, dt);
	this->_timestamps.write(cmd, Timestamps::DensityAdvect, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	// renderVoxelVolume(cmd);
	renderParticles(cmd, dt);
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
		.decayRate = this->_decayRate
	};
	vkCmdPushConstants(cmd, this->_computeAddSources.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	dispatchFluid(cmd);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
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
			.redBlack = (i % 2)
		};
		vkCmdPushConstants(cmd, this->_computeDiffuseVelocity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		dispatchFluid(cmd);

		VkBufferMemoryBarrier barriers[] = {
			this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
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
			.redBlack = (i % 2)
		};
		vkCmdPushConstants(cmd, this->_computeDiffuseDensity.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		dispatchFluid(cmd);

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

	dispatchFluid(cmd);

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

	dispatchFluid(cmd);

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
	dispatchFluid(cmd);
	VkBufferMemoryBarrier barriers[] = {
		this->_buffFluidGrid.CreateBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, ARRLEN(barriers), barriers, 0, nullptr);
}

void
UniformFluidEngine::solvePressure(VkCommandBuffer cmd)
{
	this->_computeSolvePressure.Bind(cmd);

	for(uint32_t i = 0; i < this->_pressureIterations; i++) {
		FluidPushConstants pc = {
			.redBlack = (i % 2)
		};
		vkCmdPushConstants(cmd, this->_computeSolvePressure.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

		dispatchFluid(cmd);

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
	dispatchFluid(cmd);
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
		.gridSize = glm::vec3(Constants::VoxelGridDimensions),
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
UniformFluidEngine::dispatchFluid(VkCommandBuffer cmd, const glm::uvec3& factor)
{
	const glm::uvec4 groups = (Constants::VoxelGridDimensions / Constants::LocalGroupSize) / glm::uvec4(factor, 1.0);
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
UniformFluidEngine::ui()
{
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
		windowSize = ImGui::GetWindowSize();
	}
	ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(pad, windowSize.y + pad), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(0, 0)); // Auto-sizing
	if(ImGui::Begin("controls", nullptr, ImGuiWindowFlags_NoTitleBar)) {
		ImGui::SliderFloat("obj", &this->_objectRadius, 0.01, 0.5);
		ImGui::SliderFloat("src", &this->_sourceRadius, 0.01, 0.5);
		ImGui::SliderFloat("dns", &this->_densityAmount, 0.01, 1.0);
		ImGui::SliderFloat("decay", &this->_decayRate, 0.00, 0.5);
		ImGui::InputFloat("vel", &this->_velocitySpeed, 1.0, 150.0);
		const uint32_t step = 1;
		ImGui::InputScalar("diffiter", ImGuiDataType_U32, &this->_diffusionIterations, &step);
		ImGui::InputScalar("presiter", ImGuiDataType_U32, &this->_pressureIterations, &step);
		ImGui::InputFloat3("sourcePos", glm::value_ptr(this->_sourcePosition));
	}

	if(this->_shouldCollapseUI) {
		uitools::CollapseAllWindows();
		this->_shouldCollapseUI = false;
	}

	ImGui::End();
}

void
UniformFluidEngine::checkControls(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse, float dt)
{
	if(mouseMap[SDL_BUTTON_RIGHT]) {
		const float mouse_sens = -1.2;
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

	float v = this->_velocitySpeed;
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
	
	if(keyMap[SDLK_TAB]) {
		this->_shouldCollapseUI = true;
		keyMap[SDLK_TAB] = false;
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
	this->_renderer.CreateTimestampQueryPool(this->_timestamps, Timestamps::NumTimestamps);
	this->_timestampAverages.resize(Timestamps::NumTimestamps, 0);

	this->_objectRadius = 0.2;
	this->_sourceRadius = 0.15;

	// uitools::SetAmberRedTheme();
	// uitools::SetDarkRedTheme();
	uitools::SetTheme();
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

	Particle particles[Constants::NumParticles];
	for(int i = 0; i < Constants::NumParticles; i++) {
		particles[i].position = glm::vec4((glm::vec3(rand(), rand(), rand()) / (float)RAND_MAX - 0.5f) * glm::vec3(Constants::VoxelGridDimensions) * (float)Constants::VoxelCellSize, 1.0);
		particles[i].mass = 0.01;
		particles[i].lifetime = (float)rand() / (float)RAND_MAX * Constants::MaxParticleLifetime;
	}
	this->_renderer.ImmediateSubmit([&particles, this](VkCommandBuffer cmd) {
		vkCmdUpdateBuffer(cmd, this->_buffParticles.bufferHandle, 0, sizeof(particles), &particles);
	});

	return true;
}

bool
UniformFluidEngine::initPipelines()
{
    this->_renderer.CreateGraphicsPipeline(this->_graphicsRenderMesh, SHADER_DIRECTORY"/mesh.vert.spv", SHADER_DIRECTORY"/mesh.frag.spv", "", {}, 0, 
                                           sizeof(GraphicsPushConstants), {.blendMode = BlendMode::Additive});
    this->_renderer.CreateGraphicsPipeline(this->_graphicsParticles, SHADER_DIRECTORY"/particles.vert.spv", SHADER_DIRECTORY"/particles.frag.spv", "", {
		BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
		BufferDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize),
		BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffParticles.bufferHandle, Constants::NumParticles * sizeof(Particle))
	}, VK_SHADER_STAGE_VERTEX_BIT, sizeof(ParticlesPushConstants),
	{.inputTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST, .blendMode = BlendMode::Additive, .pushConstantsStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT});

    this->_renderer.CreateComputePipeline(this->_computeRaycastVoxelGrid, SHADER_DIRECTORY"/voxelTracerAccum.comp.spv", {
        BufferDescriptor(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidGrid.bufferHandle, Constants::VoxelGridSize),
        ImageDescriptor(0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->_renderer.GetDrawImage().imageView, VK_NULL_HANDLE, this->_renderer.GetDrawImage().layout),
		BufferDescriptor(0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, this->_buffFluidInfo.bufferHandle, sizeof(FluidGridInfo)),
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
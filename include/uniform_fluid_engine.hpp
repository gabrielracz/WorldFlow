#ifndef UNIFORM_FLUID_ENGINE_HPP_
#define UNIFORM_FLUID_ENGINE_HPP_

#include "renderer_structs.hpp"
#include "vma.hpp"
#include "image.hpp"
#include "buffer.hpp"

#include <atomic>

class Renderer;
class UniformFluidEngine
{
public:
	UniformFluidEngine(Renderer& renderer);
	~UniformFluidEngine();

	bool Init();

private:
	void drawUI();
	void preFrame();
	void update(VkCommandBuffer cmd, float dt);
	void addSources(VkCommandBuffer cmd, float dt);
	void diffuseVelocity(VkCommandBuffer cmd, float dt);
	void advectVelocity(VkCommandBuffer cmd, float dt);
	void computeDivergence(VkCommandBuffer cmd);
	void solvePressure(VkCommandBuffer cmd);
	void projectIncompressible(VkCommandBuffer cmd);
	void diffuseDensity(VkCommandBuffer cmd, float dt);
	void advectDensity(VkCommandBuffer cmd, float dt);
	void renderVoxelVolume(VkCommandBuffer cmd);
	void renderMesh(VkCommandBuffer cmd, Mesh& mesh, const glm::mat4& transform = glm::mat4());
	void renderParticles(VkCommandBuffer cmd, float dt);
	void generateGridLines(VkCommandBuffer cmd);
	void drawGrid(VkCommandBuffer cmd);

	void dispatchFluid(VkCommandBuffer cmd, const glm::uvec3& factor = {1, 1, 1});
	// void getTimestampQueries();

	void checkControls(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse, float dt);

	bool initRendererOptions();
	bool initResources();
	bool initPipelines();
	bool initPreProcess();

private:
	Renderer& _renderer;

	float _tickRate {0.1f};
	bool _useTickRate {true};
	bool _shouldAddSources {false};
	bool _shouldDiffuseDensity {false};
	bool _shouldProjectIncompressible {true};
	bool _shouldRenderFluid {true};
	bool _shouldClear {false};
	bool _toggle {false};
	bool _shouldAddObstacle {false};
	bool _shouldHideUI {false};
	glm::vec3 _objectPosition{};
	float _objectRadius;
	float _objectOffset = 1.0;
	int _renderType = 1;
	glm::vec3 _sourcePosition {};
	float _sourceRadius {};
	glm::vec3 _velocitySourceAmount {};
	float _densityAmount = 0.25f;
	float _velocitySpeed = 10.0f;
	float _decayRate = 0.1f;

	uint32_t _diffusionIterations;
	uint32_t _pressureIterations;
	uint32_t _advectionIterations;
	// std::vector<uint64_t> _timestamps;
	TimestampQueryPool _timestamps;
	std::vector<float> _timestampAverages;

	GraphicsPipeline _graphicsRenderMesh;
	GraphicsPipeline _graphicsParticles;
	GraphicsPipeline _graphicsGridLines;
	ComputePipeline _computeRaycastVoxelGrid;
	ComputePipeline _computeAddSources;
	ComputePipeline _computeDiffuseVelocity;
	ComputePipeline _computeAdvectVelocity;
	ComputePipeline _computeDivergence;
	ComputePipeline _computeSolvePressure;
	ComputePipeline _computeProjectIncompressible;
	ComputePipeline _computeDiffuseDensity;
	ComputePipeline _computeAdvectDensity;
	ComputePipeline _computeGenerateGridLines;

	Buffer _buffFluidGrid;
	Buffer _buffFluidInfo;
	Buffer _buffParticles;
	Buffer _buffStaging;
	Buffer _buffGridLines;
	
	Buffer _buffFluidGridReferences;
	Buffer _buffFluidVelocity;
	Buffer _buffFluidDensity;
	Buffer _buffFluidPressure;
	Buffer _buffFluidDivergence;
	Buffer _buffFluidFlags;
	Buffer _buffFluidDebug;
	Buffer _buffFluidBrickOffsets;

	Mesh _gridMesh;
};




#endif
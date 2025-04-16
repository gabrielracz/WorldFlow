#ifndef WORLDFLOW_ENGINE_HPP_
#define WORLDFLOW_ENGINE_HPP_

#include "wf_structs.hpp"
#include "renderer_structs.hpp"
#include "vma.hpp"
#include "buffer.hpp"
#include "image.hpp"

#include <atomic>


class Renderer;

namespace wf {

struct Settings
{
	glm::uvec4 resolution;
	unsigned int numGridLevels {1};
	uint32_t gridSubdivision {4};
};

struct SubGrid
{
	uint32_t level;
	glm::uvec4 resolution;
	glm::vec4 center;
	float cellSize;

	Buffer buffGpuReferences;
	Buffer buffDispatchCommand;
	Buffer buffFluidVelocity;
	Buffer buffFluidDensity;
	Buffer buffFluidPressure;
	Buffer buffFluidDivergence;
	Buffer buffFluidFlags;
	Buffer buffFluidDebug;
	Buffer buffFluidIndexOffsets;
	Buffer buffFluidVorticity;
};


struct Grid
{
	SubGrid subgrids[Constants::MAX_SUBGRID_LEVELS];
	uint32_t numSubgrids;

	WorldFlowGridGpu gpuRefs;
	Buffer buffWorldFlowGridGpu;
};

class WorldFlow
{
public:
	WorldFlow(Renderer& renderer, Settings settings = {});
	~WorldFlow();

	bool Init();

private:
	void drawUI();
	void preFrame();
	void update(VkCommandBuffer cmd, float dt);
	void generateSubgridOffsets(VkCommandBuffer cmd);
	void generateIndirectCommands(VkCommandBuffer cmd);
	void addSources(VkCommandBuffer cmd, float dt);
	void diffuseVelocity(VkCommandBuffer cmd, float dt);
	void advectVelocity(VkCommandBuffer cmd, float dt);
	void computeDivergence(VkCommandBuffer cmd);
	void solvePressure(VkCommandBuffer cmd);
	void projectIncompressible(VkCommandBuffer cmd, float dt);
	void diffuseDensity(VkCommandBuffer cmd, float dt);
	void advectDensity(VkCommandBuffer cmd, float dt);
	void prolongDensity(VkCommandBuffer cmd, uint32_t coarseGridLevel);
	void prolongVelocity(VkCommandBuffer cmd, uint32_t coarseGridLevel);
	void restrictVelocities(VkCommandBuffer cmd);
	void renderVoxelVolume(VkCommandBuffer cmd);
	void renderMesh(VkCommandBuffer cmd, Mesh& mesh, const glm::mat4& transform = glm::mat4(1.0f));
	void voxelRasterizeGeometry(VkCommandBuffer cmd, Mesh& mesh);
	void renderParticles(VkCommandBuffer cmd, float dt);
	void generateGridLines(VkCommandBuffer cmd);
	void drawGrid(VkCommandBuffer cmd);

	void dispatchFluid(VkCommandBuffer cmd, const SubGrid& sg, const glm::uvec3& factor = {1, 1, 1});
	// void getTimestampQueries();

	void checkControls(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse, float dt);

	bool initRendererOptions();
	bool initResources();
	bool initPipelines();
	bool initPreProcess();

private:
	Renderer& _renderer;
	Settings _settings;

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
	bool _shouldPause {false};
	glm::vec3 _objectPosition{0.0};
	float _objectRadius;
	float _objectOffset = 0.0;
	int _renderType = 1;
	glm::vec3 _sourcePosition {};
	float _sourceRadius {};
	glm::vec3 _velocitySourceAmount {};
	float _densityAmount = 2.5f;
	float _velocitySpeed = 3.25f;
	float _decayRate = 0.1f;
	float _transferAlpha = 0.4f;
	float _restrictionAlpha = 0.4f;
	float _diffusionRate = 1.5f;
	float _activationThreshold = 1.5f;
	float _fluidDensity = 0.1f;
	uint32_t _rendererSubgridBegin = 0;
	uint32_t _rendererSubgridLimit = Constants::MAX_SUBGRID_LEVELS;

	uint32_t _diffusionIterations;
	uint32_t _pressureIterations;
	uint32_t _iterationSubgridFactor = 1;
	uint32_t _advectionIterations;
	// std::vector<uint64_t> _timestamps;
	TimestampQueryPool _timestamps;
	std::vector<float> _timestampAverages;

	GraphicsPipeline _graphicsRenderMesh;
	GraphicsPipeline _graphicsVoxelRaster;
	GraphicsPipeline _graphicsParticles;
	GraphicsPipeline _graphicsGridLines;

	ComputePipeline _computeGenerateIndirectCommands;
	ComputePipeline _computeGenerateSubgridOffsets;
	ComputePipeline _computeAddSources;
	ComputePipeline _computeDiffuseVelocity;
	ComputePipeline _computeAdvectVelocity;
	ComputePipeline _computeDivergence;
	ComputePipeline _computeSolvePressure;
	ComputePipeline _computeProjectIncompressible;
	ComputePipeline _computeDiffuseDensity;
	ComputePipeline _computeAdvectDensity;
	ComputePipeline _computeGenerateGridLines;
	ComputePipeline _computeRaycastVoxelGrid;
	ComputePipeline _computeProlongDensity;
	ComputePipeline _computeProlongVelocity;
	ComputePipeline _computeRestrictVelocity;

	Buffer _buffParticles;
	Buffer _buffStaging;
	Buffer _buffGridLines;
	Image _voxelImage;
	
	Grid _grid {};

	Mesh _gridMesh;
	std::vector<Mesh> _objMeshes;
};
}
#endif
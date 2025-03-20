#ifndef WORLDFLOW_ENGINE_HPP_
#define WORLDFLOW_ENGINE_HPP_

#include "wf_structs.hpp"
#include "renderer_structs.hpp"
#include "vma.hpp"
#include "buffer.hpp"

#include <atomic>


class Renderer;

namespace wf {

struct WFSettings
{
	glm::uvec4 resolution;
	unsigned int numGridLevels {2};
};

struct SubGrid
{
	glm::uvec4 resolution;
	glm::vec4 center;
	float cellSize;

	Buffer buffGpuReferences;
	Buffer buffFluidVelocity;
	Buffer buffFluidDensity;
	Buffer buffFluidPressure;
	Buffer buffFluidDivergence;
	Buffer buffFluidFlags;
	Buffer buffFluidDebug;
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
	WorldFlow(Renderer& renderer);
	~WorldFlow();

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

	// Buffer _buffFluidGrid;
	// Buffer _buffFluidInfo;
	Buffer _buffParticles;
	Buffer _buffStaging;
	Buffer _buffGridLines;
	
	Grid _grid {};

	Mesh _gridMesh;
};
}
#endif
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
	void ui();
	void preFrame();
	void update(VkCommandBuffer cmd, float dt);
	void addSources(VkCommandBuffer cmd);
	void diffuseVelocity(VkCommandBuffer cmd, float dt);
	void advectVelocity(VkCommandBuffer cmd, float dt);
	void computeDivergence(VkCommandBuffer cmd);
	void solvePressure(VkCommandBuffer cmd);
	void projectIncompressible(VkCommandBuffer cmd);
	void diffuseDensity(VkCommandBuffer cmd, float dt);
	void advectDensity(VkCommandBuffer cmd, float dt);
	void renderVoxelVolume(VkCommandBuffer cmd);
	// void getTimestampQueries();

	void checkControls(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse, float dt);

	bool initRendererOptions();
	bool initResources();
	bool initPipelines();

private:
	Renderer& _renderer;

	bool _shouldAddSources {false};
	bool _shouldDiffuseDensity {false};
	bool _toggle {false};
	bool _shouldAddObstacle {false};
	glm::vec3 _objectPosition{};
	int _renderType = 1;
	glm::vec3 _sourcePosition {};
	glm::vec3 _velocitySourceAmount {};
	float _densityAmount = 0.25;
	// std::vector<uint64_t> _timestamps;
	TimestampQueryPool _timestamps;
	std::vector<float> _timestampAverages;

	ComputePipeline _computeRaycastVoxelGrid;
	ComputePipeline _computeAddSources;
	ComputePipeline _computeDiffuseVelocity;
	ComputePipeline _computeAdvectVelocity;
	ComputePipeline _computeDivergence;
	ComputePipeline _computeSolvePressure;
	ComputePipeline _computeProjectIncompressible;
	ComputePipeline _computeDiffuseDensity;
	ComputePipeline _computeAdvectDensity;

	Buffer _buffFluidGrid;
	Buffer _buffFluidInfo;
};




#endif
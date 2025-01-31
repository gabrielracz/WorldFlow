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
	void update(VkCommandBuffer cmd, float dt);
	void addVoxels(VkCommandBuffer cmd);
	void renderVoxelVolume(VkCommandBuffer cmd);

	void checkInput(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse);

	bool initResources();
	bool initPipelines();

private:
	Renderer& _renderer;

	ComputePipeline _computeRaycastVoxelGrid;
	ComputePipeline _computeAddVoxels;

	Buffer _buffVoxelGrid;
};




#endif
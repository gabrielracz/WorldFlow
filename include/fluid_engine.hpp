#ifndef FLUID_ENGINE_HPP_
#define FLUID_ENGINE_HPP_

// #include "renderer.hpp"
#include "renderer_structs.hpp"
// #include "vk_mem_alloc.h"
#include "vma.hpp"
#include "image.hpp"
#include "buffer.hpp"

#include <atomic>

class Renderer;

class FluidEngine
{
public:
	FluidEngine(Renderer& renderer);
	~FluidEngine();

	bool Init();

private:
	void update(VkCommandBuffer cmd, float dt);

	void zeroVoxelBuffers(VkCommandBuffer cmd);
	void updateVoxelVolume(VkCommandBuffer cmd);
	void voxelRasterizeGeometry(VkCommandBuffer cmd);
	void generateTreeIndirectCommands(VkCommandBuffer cmd);
	void generateTreeGeometry(VkCommandBuffer cmd);
	void drawLines(VkCommandBuffer cmd);
	void flagNodesForSubdivision(VkCommandBuffer cmd);
	void subdivideTree(VkCommandBuffer cmd);
	void drawGeometry(VkCommandBuffer cmd, float dt);
	void rayCastVoxelVolume(VkCommandBuffer cmd);

	void checkInput(KeyMap& keyMap, MouseMap& mouseMap, Mouse& mouse);

	bool initResources();
	bool initPipelines();

private:
	Renderer& _renderer;

	/* CONFIG */
	std::atomic<bool> _shouldSubdivide {true};
	std::atomic<bool> _shouldRenderVoxels {true};
	std::atomic<bool> _shouldRenderGeometry {false};
	std::atomic<bool> _shouldRenderLines {true};
	std::atomic<bool> _shouldAddVoxelNoise {false};

	/* PIPELINES */
    ComputePipeline _voxelizerPipeline;
    ComputePipeline _raytracerPipeline;
    ComputePipeline _treeFragmentProcessor;
    ComputePipeline _treeLineGenerator;
    ComputePipeline _treeIndirectCmdGenerator;
    ComputePipeline _treeSubdivider;
    ComputePipeline _treeFlagger;

    GraphicsPipeline _meshPipeline;
    GraphicsPipeline _voxelRasterPipeline;
    GraphicsPipeline _linePipeline;

	/* RESOURCES */
    Image _voxelImage;
    Buffer _stagingBuffer;
    Buffer _voxelGrid;
    Buffer _voxelInfoBuffer;
    Buffer _voxelFragmentCounter;
    Buffer _voxelFragmentList;
    Buffer _voxelNodes;
    Buffer _treeIndirectDrawBuffer;
    Buffer _treeIndirectDispatchBuffer;
    Buffer _treeFlaggerIndirectDispatchBuffer;
    Buffer _treeInfoBuffer;

	std::vector<GPUMesh> _testMeshes;
	GPUMesh _treeMesh;
	Buffer _treeVertices;
	Buffer _treeIndices;
};

#endif
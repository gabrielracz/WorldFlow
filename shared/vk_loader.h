// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>
#include <unordered_map>
#include <filesystem>
#include "buffer.hpp"

class VulkanEngine;

struct GeoSurface {
    uint32_t startIndex;
    uint32_t count;
};

struct MeshAsset {
    std::string name;
   
    std::vector<GeoSurface> surfaces;
    GPUMesh meshBuffers;
};


bool loadGltfMeshes(std::filesystem::path filePath, std::vector<std::vector<Vertex>>& vertexBuffers, std::vector<std::vector<uint32_t>>& indexBuffers);


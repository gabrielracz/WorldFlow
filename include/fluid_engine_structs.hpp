#ifndef FLUID_ENGINE_STRUCTS_HPP_
#define FLUID_ENGINE_STRUCTS_HPP_

struct alignas(16) VoxelizerPushConstants
{
    glm::uvec3 gridSize;
    float gridScale;
    float time;
};

struct alignas(16) VoxelInfo
{
    glm::vec3 gridDimensions;
    float gridScale;
};

struct alignas(16) VoxelFragmentCounter
{
    uint32_t fragCount;
};

struct alignas(16) TreeInfo
{
    uint32_t nodeCounter;
};


struct alignas(16) VoxelFragment
{
    glm::vec3 position;
    uint32_t index;
};

struct alignas(16) alignas(16) VoxelNode
{
    glm::vec4 pos; // w component = depth
    uint32_t childPtr; 
};

struct alignas(16) RayTracerPushConstants
{
    glm::mat4 inverseProjection;  // Inverse projection matrix
    glm::mat4 inverseView;        // Inverse view matrix
    glm::vec3 cameraPos;         // Camera position in world space
    float nearPlane;        // Near plane distance
    glm::vec2 screenSize;        // Width and height of output image
    float maxDistance;      // Maximum ray travel distance
    float stepSize;         // Base color accumulation per step
    glm::vec3 gridSize;          // Size of the voxel grid in each dimension
    float gridScale;          // Size of the voxel grid in each dimension
    glm::vec4 lightSource;
    glm::vec4 baseColor;
};

// push constants for our mesh object draws
struct alignas(16) GraphicsPushConstants
{
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
    uint32_t padding[2];
};

struct alignas(16) TreeBuilderPushConstants
{
    VkDeviceAddress vertexBuffer;
    VkDeviceAddress indexBuffer;
};

struct alignas(16) TreeRendererPushConstants
{
    VkDeviceAddress vertexBuffer;
    VkDeviceAddress indexBuffer;
};

#endif
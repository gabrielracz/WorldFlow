#ifndef WF_STRUCTS_HPP_
#define WF_STRUCTS_HPP_

#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>

namespace wf {

namespace Constants {
	constexpr uint32_t MAX_SUBGRID_LEVELS = 4;
	static_assert(MAX_SUBGRID_LEVELS % 2 == 0 && MAX_SUBGRID_LEVELS > 0); // ensure 16 byte alignment
}

struct alignas(16) DispatchIndirectCommand
{
	uint32_t x;
	uint32_t y;
	uint32_t z;
};

struct alignas(16) WorldFlowGridGpu
{
	uint64_t subgridReferences[Constants::MAX_SUBGRID_LEVELS] = {0};
	uint32_t subgridCount {};
};

typedef glm::vec4  FluidVelocity;
typedef float      FluidDensity;
typedef float      FluidPressure;
typedef float      FluidDivergence;
typedef uint32_t   FluidFlags;
typedef glm::vec4  FluidDebug;
typedef uint32_t   FluidIndexOffsets;
typedef glm::vec4   FluidVorticity;
struct alignas(16) SubGridGpuReferences
{
	uint64_t velocityBufferReference;
	uint64_t densityBufferReference;
	uint64_t pressureBufferReference;
	uint64_t divergenceBufferReference;
	uint64_t flagsBufferReference;
	uint64_t debugBufferReference;
	uint64_t indexOffsetsBufferReference;
	uint64_t dispatchCommandReference;
	uint64_t vorticityBufferReference;
	uint64_t padding;

    glm::uvec4 resolution;
    glm::vec4 center;
    float cellSize;
	uint32_t indexCount;
};

struct alignas(16) SubgridLevelPushConstants
{
	uint32_t subgridLevel;
};

struct alignas(16) SubgridTransferPushConstants
{
	uint32_t subgridLevel;
	float alpha;
};

struct alignas(16) ProjectIncompressiblePushConstants
{
	float dt;
	uint32_t subgridLevel;
	float fluidDensity;
};

struct alignas(16) GenerateIndirectCommandPushConstants
{
	uint32_t subgridLevel;
	uint32_t groupDimensionLimit;
};

struct alignas(16) FluidPushConstants
{
	float time;
	float dt;
	uint32_t redBlack;
	uint32_t subgridLevel;
};

struct alignas(16) DiffusionPushConstants
{
	float dt;
	uint32_t redBlack;
	uint32_t subgridLevel;
	float diffusionRate;
};

struct alignas(16) AddFluidPropertiesPushConstants
{
	glm::vec4 sourcePosition;
	glm::vec4 velocity;
	glm::vec4 objectPosition;
	glm::vec4 activationWeights;
	float elapsed;
	float dt;
	float sourceRadius;
	int addVelocity;
	int addDensity;
    float density;
	uint32_t objectType;
	float objectRadius;
	float decayRate;
	int clear;
	unsigned int subgridLevel;
	float activationThreshold;
};
const auto s = sizeof(AddFluidPropertiesPushConstants);

struct alignas(16) RayTracerPushConstants
{
    glm::mat4 inverseProjection;  // Inverse projection matrix
    glm::mat4 inverseView;        // Inverse view matrix
    glm::vec3 cameraPos;         // Camera position in world space
    float nearPlane;        // Near plane distance
    glm::vec2 screenSize;        // Width and height of output image
    uint32_t maxDistance;      // Maximum ray travel distance
    float stepSize;         // Base color accumulation per step
    glm::vec3 gridSize;          // Size of the voxel grid in each dimension
    float gridScale;          // Size of the voxel grid in each dimension
    glm::vec4 lightSource;
    glm::vec4 baseColor;
    int renderType;
	unsigned int rootGridLevel;
	unsigned int subgridLimit;
};

struct alignas(16) ParticlesPushConstants
{
    glm::mat4 cameraMatrix;
	float dt;
	float elapsed;
	float maxLifetime;
};

struct alignas(16) GenerateLinesPushConstants
{
	uint64_t vertexBufferAddress;
};

struct alignas(16) DrawLinesPushConstants
{
	glm::mat4 renderMatrix;
	uint64_t vertexBufferAddress;
};

enum Timestamps : uint32_t
{
	StartFrame = 0,
	GenerateCommands,
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

// push constants for our mesh object draws
struct alignas(16) GraphicsPushConstants
{
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
    uint32_t padding[2];
};
}
#endif
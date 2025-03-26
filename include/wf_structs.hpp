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

    glm::uvec4 resolution;
    glm::vec4 center;
    float cellSize;
	uint32_t indexCount;
};

struct alignas(16) GenerateActiveOffsetsPushConstants
{
	uint32_t subgridLevel;
};

struct alignas(16) FluidPushConstants
{
	float time;
	float dt;
	uint32_t redBlack;
	uint32_t subgridLevel;
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
	int clear;
};
const auto s = sizeof(AddFluidPropertiesPushConstants);

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
}
#endif
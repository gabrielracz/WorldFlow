#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_buffer_reference : require // or GL_ARB_buffer_device_address

#include "../common/grid.comp" // Make sure WorldFlowGrid definition is here

struct Vertex {
    vec3 position;
    float uv_x;
    vec3 normal;
    float uv_y;
    vec4 color;
};

layout(buffer_reference, buffer_reference_align = 4, std430) readonly buffer VertexBuffer {
    Vertex vertices[];
};

// Assuming WorldFlowGrid contains:
// vec3 resolution;
// vec3 center;
// float cellSize;
layout(std430, binding = 0) buffer WorldFlowGridBuffer {
    WorldFlowGrid wfGrid;
};

// Outputs to Geometry Shader
layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outWorldNormal; // Changed name for clarity

layout(push_constant) uniform PushConstants {
    mat4 transform; // Model-to-World matrix
    VertexBuffer vertexBuffer;
    // uint64_t padding; // Keep if needed for alignment/offset matching CPU struct
} pc;

void main()
{
	WorldFlowSubGrid grid = wfGrid.subgrids[0].ref;
    // 1. Load vertex data
    Vertex v = pc.vertexBuffer.vertices[gl_VertexIndex];

    // 2. Model to World Space position
    vec4 posWorld = pc.transform * vec4(v.position, 1.0);

    // 3. World Space to Grid Voxel Space
    // Get grid parameters
    vec3 gridResolution = vec3(grid.resolution); // vec3(128, 64, 256)
    vec3 gridCenterWorld = grid.center.xyz;
    float gridCellSize = grid.cellSize;
    vec3 gridDimensionsWorld = gridResolution * gridCellSize;
    vec3 gridMinCornerWorld = gridCenterWorld - gridDimensionsWorld / 2.0;

    // Calculate position relative to grid min corner and scale by cell size
    vec3 posVoxel = (posWorld.xyz - gridMinCornerWorld) / gridCellSize;

    // 4. Output Voxel Space position via gl_Position
    // The GS will read this before rasterization
    gl_Position = vec4(posVoxel, 1.0);

    // 5. Transform normal to World Space
    // Use inverse transpose of the upper 3x3 model-to-world matrix
    // Note: If pc.transform has non-uniform scaling, this is needed.
    // If it only has rotation/uniform scale/translation, normalize(mat3(pc.transform) * v.normal) is faster.
    mat3 normalMatrix = transpose(inverse(mat3(pc.transform)));
    outWorldNormal = normalize(normalMatrix * v.normal);

    // 6. Pass through other attributes
    outColor = v.color.xyz;
    outUV = vec2(v.uv_x, v.uv_y);
}
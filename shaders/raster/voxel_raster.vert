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

layout(std430, binding = 0) buffer WorldFlowGridBuffer {
    WorldFlowGrid wfGrid;
};

layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outWorldNormal; 

layout(push_constant) uniform PushConstants {
    mat4 transform; // Model-to-World matrix
    VertexBuffer vertexBuffer;
    uint64_t padding; 
} pc;

void main()
{
	WorldFlowSubGrid grid = wfGrid.subgrids[wfGrid.subgridCount-1].ref;
    Vertex v = pc.vertexBuffer.vertices[gl_VertexIndex];

    vec4 posWorld = pc.transform * vec4(v.position, 1.0);

    vec3 gridResolution = vec3(grid.resolution); 
    vec3 gridCenterWorld = grid.center.xyz;
    float gridCellSize = grid.cellSize;
    vec3 gridDimensionsWorld = gridResolution * gridCellSize;
    vec3 gridMinCornerWorld = gridCenterWorld - gridDimensionsWorld / 2.0;

    // Calculate position relative to grid min corner and scale by cell size
    vec3 posVoxel = (posWorld.xyz - gridMinCornerWorld) / gridCellSize;

    gl_Position = vec4(posVoxel, 1.0);

    mat3 normalMatrix = transpose(inverse(mat3(pc.transform)));
    outWorldNormal = normalize(normalMatrix * v.normal);

    outColor = v.color.xyz;
    outUV = vec2(v.uv_x, v.uv_y);
}
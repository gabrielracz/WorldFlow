#version 450

//shader input
layout (location = 0) in vec3 inColor;
layout (location = 1) in vec2 inUV;

layout(std430, binding = 0) buffer GridBuffer {
    float grid[]; // 1D array to store the 3D grid data
};

layout(push_constant) uniform constants {
    layout(offset = 80) vec3 gridSize;
    layout(offset = 92) float gridScale;
    layout(offset = 96) float time;
} pc;

uint getVoxelIndex(vec3 pos) {
    // Convert from scaled world space to grid space (0 to gridSize-1)
    vec3 normalizedPos = pos / pc.gridScale;  // First normalize by scale
    vec3 gridPos = (normalizedPos + 0.5) * pc.gridSize;
    ivec3 index = ivec3(floor(gridPos));
    
    // Clamp to grid bounds
    index = clamp(index, ivec3(0), ivec3(pc.gridSize) - 1);
    
    // Convert to linear index using the same formula as construction
    return index.z * uint(pc.gridSize.x) * uint(pc.gridSize.y) +
           index.y * uint(pc.gridSize.x) +
           index.x;
}

uint getIndex(uint x, uint y, uint z)
{
    return z * uint(pc.gridSize.x) * uint(pc.gridSize.y) +
           y * uint(pc.gridSize.x) +
           x;
}

void main() 
{
    vec3 gridPos = (gl_FragCoord.xyz * pc.gridSize.xyz) + pc.gridSize/2.0;
    ivec3 index = clamp(ivec3(floor(gridPos)), ivec3(0), ivec3(pc.gridSize) - 1);
    // Convert to linear index using the same formula as construction
    uint ix =  index.z * uint(pc.gridSize.x) * uint(pc.gridSize.y) +
           index.y * uint(pc.gridSize.x) +
           index.x;
    // grid[getVoxelIndex(*gl_FragCoord.xyz)] = 0.5;
    // grid[ix] = 0.5;
    grid[getIndex(0, 0, 0)] = 0.5;
    grid[getIndex(128, 128, 128)] = 1.0;
    grid[getIndex(256, 0, 0)] = 1.0;
	// //return red
	// outFragColor = vec4(inColor,1.0f);
}
#version 450

//shader input
layout (location = 0) in vec3 inColor;
layout (location = 1) in vec2 inUV;
layout (location = 2) in float inDepth;
layout (location = 3) in flat int axis;

layout(std430, binding = 0) buffer GridBuffer {
    float grid[]; // 1D array to store the 3D grid data
};

layout(std140, binding = 1) uniform VoxelInfo {
    vec3 gridDimensions;
    float gridScale;
};

layout (location = 0) out vec4 outColour;

uint getVoxelIndex(vec3 pos) {
    // Convert from scaled world space to grid space (0 to gridSize-1)
    vec3 normalizedPos = pos / gridScale;  // First normalize by scale
    vec3 gridPos = (normalizedPos + 0.5) * gridDimensions;
    ivec3 index = ivec3(floor(gridPos));
    
    // Clamp to grid bounds
    index = clamp(index, ivec3(0), ivec3(gridDimensions) - 1);
    
    // Convert to linear index using the same formula as construction
    return index.z * uint(gridDimensions.x) * uint(gridDimensions.y) +
           index.y * uint(gridDimensions.x) +
           index.x;
}

uint getIndex(uvec3 pos)
{
    return pos.z * uint(gridDimensions.x) * uint(gridDimensions.y) +
           pos.y * uint(gridDimensions.x) +
           pos.x;
}

void main() 
{
    // fix the axis to use the right "depth" channel
    vec3 temp = vec3(gl_FragCoord.xy, (inDepth + 0.5) * (gridDimensions.z - 1));
    vec3 pos = vec3(0.0);

    if(axis == 0) {
        pos = vec3(temp.z, temp.y, temp.x);
    } else if(axis == 1) {
        pos = vec3(temp.x, temp.z, temp.y);
    } else {
        pos = vec3(temp.x, temp.y, temp.z);
    }

    grid[getIndex(uvec3(floor(pos)))] = 0.7;

    uint d = uint(gridDimensions.x) - 1;
    grid[getIndex(uvec3(0, 0, 0))] = 1.0;
    // grid[getIndex(uvec3(d, 0, 0))] = 1.0;
    grid[getIndex(uvec3(d, d, d))] = 1.0;
    // grid[getIndex(uvec3(0, 0, d))] = 1.0;
    outColour = vec4((inDepth + 0.5), 0.0, 0.0, 1.0);
}
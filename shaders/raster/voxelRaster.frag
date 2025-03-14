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

layout(std430, binding = 2) buffer VoxelFragmentListInfo {
    uint fragCounter;
};

struct VoxelFragment
{
    vec3 position;
    uint gridIndex;
};

layout(std430, binding = 3) buffer VoxelFragmentList {
    VoxelFragment fragList[];
};

layout (location = 0) out vec4 outColour;

uint getIndex(uvec3 pos)
{
    return pos.z * uint(gridDimensions.x) * uint(gridDimensions.y) +
           pos.y * uint(gridDimensions.x) +
           pos.x;
}

void main() 
{
    uint d = uint(gridDimensions.x) - 1;
    // fix the axis to use the right "depth" channel
    vec3 temp = vec3(gl_FragCoord.xy, ((gl_FragCoord.z)) * (gridDimensions.z));
    vec3 pos = vec3(0.0);
    
    if(axis == 0) {
        pos = vec3(temp.z, temp.y, temp.x);
    } else if(axis == 1) {
        pos = vec3(temp.x, temp.z, temp.y);
    } else {
        pos = vec3(temp.x, temp.y, temp.z);
    }

    // direct regular grid insert
    uint gridIndex = getIndex(uvec3(floor(pos)));
    grid[gridIndex] = 1.0; // TODO: use atomic here
    
    // orthographic image visualization
    outColour = vec4((1.0 - inDepth)/2.0, 0.0, 0.0, 1.0);

    // insert into voxel fragment list for octree placement
    uint fragListIndex = atomicAdd(fragCounter, 1);
    VoxelFragment voxelFrag;
    voxelFrag.position = (pos / gridDimensions);
    voxelFrag.gridIndex = gridIndex;
    fragList[fragListIndex] = voxelFrag;
}
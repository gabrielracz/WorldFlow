#version 450
#extension GL_GOOGLE_include_directive : enable
#include "../common/grid.comp"

//shader input
layout (location = 0) in vec3 inColor;
layout (location = 1) in vec2 inUV;
layout (location = 2) in float inDepth;
layout (location = 3) in flat int axis;

layout(std430, binding = 0) buffer WorldFlowGridBuffer {
	WorldFlowGrid wfGrid;
};

// struct VoxelFragment
// {
//     vec3 position;
//     uint gridIndex;
// };

// layout(std430, binding = 3) buffer VoxelFragmentList {
//     VoxelFragment fragList[];
// };

layout (location = 0) out vec4 outColor;

// uint getIndex(uvec3 pos)
// {
//     return pos.z * uint(gridDimensions.x) * uint(gridDimensions.y) +
//            pos.y * uint(gridDimensions.x) +
//            pos.x;
// }

void main() 
{

    uint fineLevel = wfGrid.subgridCount - 1;
    WorldFlowSubGrid grid = wfGrid.subgrids[fineLevel].ref;
    // uint d = uint(gridDimensions.x) - 1;
    // vec2 uv = gl_FragCoord.xy / maxDim;
    // vec2 boxCoord = uv * boxSize.xy; // Depends on projection plane

    vec3 temp = vec3(gl_FragCoord.xy, ((gl_FragCoord.z)) * grid.resolution[axis]);
    vec3 pos = vec3(0.0);
    // float depth = inDepth * ;
    
    // fix the axis to use the right "depth" channel
    outColor = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 activeColor = vec4(0.0, 0.0, 1.0 - inDepth, 1.0);
    if(axis == 0) {
        pos = vec3(temp.z, temp.y, temp.x);
        // return;
    } else if(axis == 1) {
        pos = vec3(temp.x, temp.z, temp.y);
        // return;
    } else {
        pos = vec3(temp.x, temp.y, temp.z);
        // return;
    }

    outColor = activeColor;


    uvec3 gridPos = uvec3(pos);
    uint gridIndex = getGridIndex(gridPos, grid.resolution);
    grid.flagsBuffer.data[gridIndex] |= FLAG_OCCUPIED;

    if(wfGrid.subgridCount > 1) {
        uint coarseIndex = getCoarseGridIndex(gridPos, wfGrid, fineLevel);
        wfGrid.subgrids[fineLevel-1].ref.flagsBuffer.data[coarseIndex] |= FLAG_OCCUPIED | FLAG_ACTIVE | FLAG_SOURCE;
    }
}
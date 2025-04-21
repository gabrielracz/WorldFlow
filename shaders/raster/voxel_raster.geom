#version 450
#extension GL_GOOGLE_include_directive : enable
#include "../common/grid.comp"

layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;

layout(std430, binding = 0) buffer WorldFlowGridBuffer {
    WorldFlowGrid wfGrid;
};

layout (location = 0) in vec3 inColor[];
layout (location = 1) in vec2 inUV[];
layout (location = 2) in vec3 inWorldNormal[]; 

// Output attributes to Fragment Shader
layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
layout (location = 2) out float outDepth;       
layout (location = 3) out flat int outAxis;     

// Find axis which maximized orthographic projected area of triangle
// Uses the average world space normal of the input triangle vertices
int getDominantAxisIndex(vec3 normal) 
{
    vec3 absNormal = abs(normal);
    if (absNormal.x >= absNormal.y && absNormal.x >= absNormal.z) {
        return 0; 
    } else if (absNormal.y >= absNormal.x && absNormal.y >= absNormal.z) {
        return 1; 
    } else {
        return 2;
    }
}

void main() {
    WorldFlowSubGrid grid = wfGrid.subgrids[wfGrid.subgridCount-1].ref;
    vec3 avgWorldNormal = normalize(inWorldNormal[0] + inWorldNormal[1] + inWorldNormal[2]);

    int dominantAxisIndex = getDominantAxisIndex(avgWorldNormal);
    outAxis = dominantAxisIndex;

    vec3 gridResolution = vec3(grid.resolution);
    float maxDim = max(max(gridResolution.x, gridResolution.y), gridResolution.z);

    for (int i = 0; i < 3; ++i) {
        vec3 posVoxel = gl_in[i].gl_Position.xyz;

        vec4 outPositionNDC; 

        if (dominantAxisIndex == 0) { // Project onto YZ plane (X is depth)
            outPositionNDC.x = (posVoxel.z / maxDim) * 2.0 - 1.0;
            outPositionNDC.y = (posVoxel.y / maxDim) * 2.0 - 1.0;
            outPositionNDC.z = posVoxel.x / gridResolution.x; // Normalize depth by X-res
            outPositionNDC.w = 1.0;
        } else if (dominantAxisIndex == 1) { // Project onto XZ plane (Y is depth)
            outPositionNDC.x = (posVoxel.x / maxDim) * 2.0 - 1.0;
            outPositionNDC.y = (posVoxel.z / maxDim) * 2.0 - 1.0;
            outPositionNDC.z = posVoxel.y / gridResolution.y; // Normalize depth by Y-res
            outPositionNDC.w = 1.0;
        } else { // dominantAxisIndex == 2, Project onto XY plane (Z is depth)
            outPositionNDC.x = (posVoxel.x / maxDim) * 2.0 - 1.0;
            outPositionNDC.y = (posVoxel.y / maxDim) * 2.0 - 1.0;
            outPositionNDC.z = posVoxel.z / gridResolution.z; // Normalize depth by Z-res
            outPositionNDC.w = 1.0;
        }

        outPositionNDC.z = clamp(outPositionNDC.z, 0.0, 1.0);

        gl_Position = outPositionNDC; 
        outDepth = outPositionNDC.z;  

        outColor = inColor[i];
        outUV = inUV[i];

        EmitVertex();
    }

    EndPrimitive();
}
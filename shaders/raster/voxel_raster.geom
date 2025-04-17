#version 450
#extension GL_GOOGLE_include_directive : enable
#include "../common/grid.comp" // Make sure WorldFlowGrid definition is here

layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;

// Grid info still needed for resolution and maxDim for normalization
layout(std430, binding = 0) buffer WorldFlowGridBuffer {
    WorldFlowGrid wfGrid;
};

// Input vertex attributes from Vertex Shader
// gl_Position arrives in GRID VOXEL SPACE
layout (location = 0) in vec3 inColor[];
layout (location = 1) in vec2 inUV[];
layout (location = 2) in vec3 inWorldNormal[]; // Now receiving World Space normal

// Output attributes to Fragment Shader
layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
layout (location = 2) out float outDepth;       // Normalized depth [0, 1] along dominant axis
layout (location = 3) out flat int outAxis;     // Dominant axis (0=X, 1=Y, 2=Z)

// Find axis which maximized orthographic projected area of triangle
// Uses the average WORLD SPACE normal of the input triangle vertices
int getDominantAxisIndex(vec3 normal) // Input normal should be in world space
{
    vec3 absNormal = abs(normal);
    if (absNormal.x >= absNormal.y && absNormal.x >= absNormal.z) {
        return 0; // X-axis dominant
    } else if (absNormal.y >= absNormal.x && absNormal.y >= absNormal.z) {
        return 1; // Y-axis dominant
    } else {
        return 2; // Z-axis dominant
    }
}

void main() {
    WorldFlowSubGrid grid = wfGrid.subgrids[wfGrid.subgridCount-1].ref;
    // --- 1. Calculate Dominant Axis ---
    // Use the averaged world-space vertex normals passed from the VS
    vec3 avgWorldNormal = normalize(inWorldNormal[0] + inWorldNormal[1] + inWorldNormal[2]);
    // Alternatively calculate face normal IF world positions were also passed from VS

    int dominantAxisIndex = getDominantAxisIndex(avgWorldNormal);
    outAxis = dominantAxisIndex;

    // --- 2. Get Grid Parameters (needed for normalization) ---
    vec3 gridResolution = vec3(grid.resolution);
    // Determine the maximum dimension for viewport scaling
    float maxDim = max(max(gridResolution.x, gridResolution.y), gridResolution.z);
    // Viewport should be set to maxDim x maxDim externally

    // --- 3. Process Vertices ---
    for (int i = 0; i < 3; ++i) {
        // --- 3a. Get Voxel Space position (already calculated in VS) ---
        vec3 posVoxel = gl_in[i].gl_Position.xyz;

        // --- 3b. Voxel Space to Normalized Projection Space ---
        // Project onto the dominant axis plane and normalize to [-1, 1] for X/Y
        // and [0, 1] for Z (depth).
        vec4 outPositionNDC; // Final position for rasterizer

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

        // Clamp depth to [0, 1]
        outPositionNDC.z = clamp(outPositionNDC.z, 0.0, 1.0);

        gl_Position = outPositionNDC; // Output final position for rasterizer
        outDepth = outPositionNDC.z;  // Pass normalized depth along dominant axis

        // Pass through other attributes
        outColor = inColor[i];
        outUV = inUV[i];

        EmitVertex();
    }

    EndPrimitive();
}
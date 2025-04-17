#version 450
#extension GL_GOOGLE_include_directive : enable
#include "../common/grid.comp"

layout(triangles) in;
// Still outputting a triangle strip, though just one triangle.
layout(triangle_strip, max_vertices=3) out;

layout(std430, binding = 0) buffer WorldFlowGridBuffer {
    WorldFlowGrid wfGrid; // Assuming this contains grid info
};

// Input vertex attributes from Vertex Shader
// gl_Position is expected in World Space from the VS
layout (location = 0) in vec3 inColor[];
layout (location = 1) in vec2 inUV[];
layout (location = 2) in vec3 inNormal[]; // Assuming per-vertex normal

// Output attributes to Fragment Shader
layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
// Send depth along dominant axis (normalized 0-1)
layout (location = 2) out float outDepth;
// Send the dominant axis index (0=X, 1=Y, 2=Z)
layout (location = 3) out flat int outAxis; // Flat ensures no interpolation

// Find axis which maximized orthographic projected area of triangle
// (Uses the average normal of the input triangle)
int getDominantAxisIndex(vec3 normal)
{
    // Use absolute values of the normal components
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
    // --- 1. Calculate Dominant Axis ---
    // Calculate the face normal (more robust than averaging potentially unnormalized vertex normals)
    // Or use pre-computed face normal if available
    vec3 pos0 = gl_in[0].gl_Position.xyz;
    vec3 pos1 = gl_in[1].gl_Position.xyz;
    vec3 pos2 = gl_in[2].gl_Position.xyz;
    vec3 edge1 = pos1 - pos0;
    vec3 edge2 = pos2 - pos0;
    vec3 faceNormal = normalize(cross(edge1, edge2));

    // If using averaged vertex normals (as in original code):
    // vec3 faceNormal = normalize(inNormal[0] + inNormal[1] + inNormal[2]);

    int dominantAxisIndex = getDominantAxisIndex(faceNormal);
    outAxis = dominantAxisIndex; // Pass axis to fragment shader

    // --- 2. Get Grid Parameters ---
    // Assuming wfGrid contains necessary info like resolution, center, and cellSize
    WorldFlowSubGrid grid = wfGrid.subgrids[0].ref; // Adapt if needed
    vec3 gridResolution = vec3(grid.resolution); // e.g., vec3(128, 64, 256) voxels
    vec3 gridCenterWorld = grid.center.xyz;        // World space center
    float gridCellSize = grid.cellSize;       // Size of one voxel in world units

    // Calculate grid bounds in world space
    vec3 gridDimensionsWorld = gridResolution * gridCellSize;
    vec3 gridMinCornerWorld = gridCenterWorld - gridDimensionsWorld / 2.0;

    // Determine the maximum dimension for viewport scaling
    float maxDim = max(max(gridResolution.x, gridResolution.y), gridResolution.z);
    // The viewport should be set to maxDim x maxDim externally

    // --- 3. Process Vertices ---
    for (int i = 0; i < 3; ++i) {
        vec3 posWorld = gl_in[i].gl_Position.xyz;

        // --- 3a. World Space to Voxel Index Space ---
        // Transform world position to a coordinate relative to the grid's min corner,
        // then scale by cell size to get approximate voxel indices.
        vec3 posVoxel = (posWorld - gridMinCornerWorld) / gridCellSize;

        // --- 3b. Voxel Space to Normalized Projection Space ---
        // Project onto the dominant axis plane and normalize to [-1, 1] for X/Y
        // and [0, 1] for Z (depth).
        vec4 outPosition; // Becomes gl_Position

        if (dominantAxisIndex == 0) { // Project onto YZ plane (X is depth)
            // Map voxel Y index to NDC X, voxel Z index to NDC Y
            // Normalize based on maxDim so it fits the square viewport
            outPosition.x = (posVoxel.z / maxDim) * 2.0 - 1.0;
            outPosition.y = (posVoxel.y / maxDim) * 2.0 - 1.0;
            // Depth is the X voxel index, normalized by the grid's X resolution
            outPosition.z = posVoxel.x / gridResolution.x;
            outPosition.w = 1.0;
        } else if (dominantAxisIndex == 1) { // Project onto XZ plane (Y is depth)
            // Map voxel X index to NDC X, voxel Z index to NDC Y
            outPosition.x = (posVoxel.x / maxDim) * 2.0 - 1.0;
            outPosition.y = (posVoxel.z / maxDim) * 2.0 - 1.0;
            // Depth is the Y voxel index, normalized by the grid's Y resolution
            outPosition.z = posVoxel.y / gridResolution.y;
            outPosition.w = 1.0;
        } else { // dominantAxisIndex == 2, Project onto XY plane (Z is depth)
            // Map voxel X index to NDC X, voxel Y index to NDC Y
            outPosition.x = (posVoxel.x / maxDim) * 2.0 - 1.0;
            outPosition.y = (posVoxel.y / maxDim) * 2.0 - 1.0;
            // Depth is the Z voxel index, normalized by the grid's Z resolution
            outPosition.z = posVoxel.z / gridResolution.z;
            outPosition.w = 1.0;
        }

        // Clamp depth to [0, 1] just in case vertex is slightly outside grid bounds
        outPosition.z = clamp(outPosition.z, 0.0, 1.0);

        gl_Position = outPosition; // Output position for rasterizer
        outDepth = outPosition.z;  // Pass normalized depth along dominant axis

        // Pass through other attributes
        outColor = inColor[i];
        outUV = inUV[i];

        EmitVertex(); // Emit the processed vertex
    }

    EndPrimitive(); // Finish the triangle strip
}
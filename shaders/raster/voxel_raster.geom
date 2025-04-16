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
layout (location = 2) in vec3 inNormal[];

layout (location = 0) out vec3 outColor;                       
layout (location = 1) out vec2 outUV;                       
layout (location = 2) out float outDepth;                       
layout (location = 3) out flat int outAxis;                       

// Find axis which maximized orthographic projected area of triangle
int getDominantAxisIndex(vec3 normal)
{
    float absX = abs(normal.x);
    float absY = abs(normal.y);
    float absZ = abs(normal.z);

    // determine the dominant axis based on the largest component
    if (absX > absY && absX > absZ) return 0; // x-axis
    else if (absY > absX && absY > absZ) return 1; // y-axis
    else return 2; // z-axis
}

vec4 orthographicProjection(vec3 position, int axisIndex)
{
    mat4 mvpx = mat4(
        0.000000, 0.000000, 1.000000, 0.000000,
        0.000000, 1.000000, 0.000000, 0.000000,
        1.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 0.000000, 1.000000);

    mat4 mvpy = mat4(
        1.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 1.000000, 0.000000,
        0.000000, 1.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 0.000000, 1.000000);

    mat4 mvpz = mat4(
        1.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 1.000000, 0.000000, 0.000000,
        0.000000, 0.000000, -1.000000, 0.000000,
        0.000000, 0.000000, 0.000000, 1.000000);

    mat4 proj;
    if(axisIndex == 0)  proj = mvpx;
    else if(axisIndex == 1) proj = mvpy;
    else proj = mvpz;

    return proj * vec4(position, 1.0);
}


void main() {
    vec3 normal = normalize(inNormal[0] + inNormal[1] + inNormal[2]);
    int dominantAxisIndex = getDominantAxisIndex(normal);

    for (int i = 0; i < 3; i++) {
        vec3 pos = gl_in[i].gl_Position.xyz;
        vec4 outPosition = orthographicProjection(pos, dominantAxisIndex);
        // vec4 outPosition = vec4(pos, 1.0);
        outPosition.z = ((outPosition.z + 1.0) / 2.0); // fix to range 0 - 1.0
        gl_Position = outPosition;
        outDepth = outPosition.z;
        outAxis = dominantAxisIndex;
        outColor = inColor[i];
        outUV = inUV[i];
        EmitVertex();
    }
    EndPrimitive();
}
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "../common/grid.comp"

struct Vertex {
	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
	Vertex vertices[];
};

layout(std430, binding = 0) buffer WorldFlowGridBuffer {
	WorldFlowGrid wfGrid;
};

layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outNormal;

layout(push_constant) uniform PushConstants {
	mat4 transform;
	VertexBuffer vertexBuffer;
    uint64_t padding;
} pc;

void main() 
{
	// load vertex from device address
	Vertex v = pc.vertexBuffer.vertices[gl_VertexIndex];

	//output the position of each vertex
	gl_Position = pc.transform * vec4(v.position, 1.0);
	outColor = v.color.xyz;
	outUV = vec2(v.uv_x, v.uv_y);
    outNormal = v.normal;
}

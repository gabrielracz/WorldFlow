#version 450
#extension GL_EXT_buffer_reference : require

layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outPosition;
layout (location = 3) out vec3 outNormal;

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

layout(push_constant) uniform PushConstants {
	mat4 modelViewMatrix;
	mat4 projectionMatrix;
	mat4 normalMatrix;
	VertexBuffer vertexBuffer;
} pc;

void main() 
{
	// load vertex from device address
	Vertex v = pc.vertexBuffer.vertices[gl_VertexIndex];
	vec4 position = pc.modelViewMatrix * vec4(v.position, 1.0);

	//output the position of each vertex
	gl_Position = pc.projectionMatrix * position;
	// outColor = v.color.xyz;
	outColor = vec3(0.7, 0.7, 0.7);
	outUV = vec2(v.uv_x, v.uv_y);
	outPosition = position.xyz;
	outNormal = vec3(pc.normalMatrix * vec4(v.normal, 0.0)).xyz;
}
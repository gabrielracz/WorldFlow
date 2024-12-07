#version 450
#extension GL_EXT_buffer_reference : require

layout (location = 0) out vec4 outColor;

struct Vertex {
	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
	vec3 vertices[];
};

layout (push_constant) uniform PushConstants {
    mat4 renderMatrix;
    VertexBuffer vertexBuffer;

} pc;

void main()
{
    vec3 pos = pc.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = pc.renderMatrix * vec4(pos, 1.0);
    outColor = vec4(0.40, 0.36, 1.0, 0.8);
}
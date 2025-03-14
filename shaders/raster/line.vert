#version 450
#extension GL_EXT_buffer_reference : require

layout (location = 0) out vec4 outColor;

layout(buffer_reference, std430) readonly buffer VertexBuffer {
	vec4 vertices[];
};

layout (push_constant) uniform PushConstants {
    mat4 renderMatrix;
    VertexBuffer vertexBuffer;
} pc;

void main()
{
    vec4 pos = pc.vertexBuffer.vertices[gl_VertexIndex];
    gl_Position = pc.renderMatrix * pos;
    outColor = vec4(0.40, 0.36, 1.0, 0.6);
}
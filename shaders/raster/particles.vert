#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_GOOGLE_include_directive : require
#include "../common/grid.comp"

struct Particle {
	vec4 position;
	float mass;
	float lifetime;
};

layout (location = 0) out vec4 outColor;

layout(set = 0, binding = 0) readonly buffer FluidInfo {
    FluidGridInfo gridInfo;
};

layout(set = 0, binding = 1) readonly buffer GridBuffer {
    FluidGridCell fluid[];
};


layout(set = 0, binding = 2) buffer ParticleBuffer {
	Particle particles[];
};

layout(push_constant) uniform PushConstants {
	mat4 cameraMatrix;
	float dt;
	float elapsed;
	float maxLifetime;
} pc;

float rand(float co) { return fract(sin(co*(91.3458)) * 47453.5453); }

void main() 
{
	// load vertex from device address
	Particle p = particles[gl_VertexIndex];
	if(p.lifetime > pc.maxLifetime) {
		p.position = vec4((vec3(rand(gl_VertexIndex + pc.elapsed), rand(gl_VertexIndex*2.31321 + pc.elapsed), rand(gl_VertexIndex*3.321321 + pc.elapsed)) - 0.5) * gridInfo.resolution.xyz * gridInfo.cellSize
		,1.0);
		p.lifetime = 0.0;
	}

	vec3 res = gridInfo.resolution.xyz;
	vec3 ix = p.position.xyz;
	vec3 velocity = fluid[worldToGridIndex(p.position.xyz, gridInfo.resolution, gridInfo.cellSize)].velocity;
	vec4 newPos = vec4(p.position.xyz + velocity * pc.dt * gridInfo.cellSize, 1.0);

	vec4 worldBounds = vec4(vec3(gridInfo.resolution.xyz) * gridInfo.cellSize * 0.5, 1.0);
	clamp(newPos, -worldBounds, worldBounds);
	// vec4 newPos = vec4(p.position.xyz + velocity * pc.dt * p.mass, 1.0);
	particles[gl_VertexIndex].position = newPos;
	particles[gl_VertexIndex].lifetime = p.lifetime + 1.0;

	//output the position of each vertex
	gl_Position =  pc.cameraMatrix * newPos;
	outColor = vec4(1.0, 0.0, 0.0, 1.0 * (pc.maxLifetime - particles[gl_VertexIndex].lifetime + 1) / pc.maxLifetime);
	gl_PointSize = 3;
}

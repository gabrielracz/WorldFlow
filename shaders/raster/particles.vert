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

layout(set = 0, binding = 0) readonly buffer WorldFlowGridBuffer {
    WorldFlowGrid wfGrid;
};


layout(set = 0, binding = 1) buffer ParticleBuffer {
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
	WorldFlowSubGrid grid = wfGrid.subgrids[0].ref;
	// load vertex from device address
	Particle p = particles[gl_VertexIndex];
	if(p.lifetime > pc.maxLifetime) {
		p.position = vec4((vec3(rand(gl_VertexIndex + pc.elapsed), rand(gl_VertexIndex*2.31321 + pc.elapsed), rand(gl_VertexIndex*3.321321 + pc.elapsed)) - 0.5) * grid.resolution.xyz * grid.cellSize
		,1.0);
		p.lifetime = 0.0;
	}

	vec3 res = grid.resolution.xyz;
	vec3 ix = p.position.xyz;
	vec4 velocity = grid.velocityBuffer.data[worldToGridIndex(p.position.xyz, grid.resolution, grid.cellSize)];
	vec4 newPos = vec4(p.position.xyz + velocity.xyz * pc.dt * grid.cellSize, 1.0);

	vec4 worldBounds = vec4(vec3(grid.resolution.xyz) * grid.cellSize * 0.5, 1.0);
	clamp(newPos, -worldBounds, worldBounds);
	// vec4 newPos = vec4(p.position.xyz + velocity * pc.dt * p.mass, 1.0);
	particles[gl_VertexIndex].position = newPos;
	particles[gl_VertexIndex].lifetime = p.lifetime + 1.0;

	//output the position of each vertex
	gl_Position =  pc.cameraMatrix * newPos;
	outColor = vec4(1.0, 0.0, 0.0, 1.0 * (pc.maxLifetime - particles[gl_VertexIndex].lifetime + 1) / pc.maxLifetime);
	gl_PointSize = 1;
}

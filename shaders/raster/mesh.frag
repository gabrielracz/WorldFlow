#version 450

layout (location = 0) in vec3 inColor;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inPosition;
layout (location = 3) in vec3 inNormal;

layout (location = 0) out vec4 outFragColor;

vec4 lighting(vec4 pixel, vec3 lightvec, vec3 norm)
{
	float diffuse = max(0.0, dot(norm,lightvec)); 
    vec4 light_color = vec4(1.0, 1.0, 1.0, 1.0);
    return 0.8*diffuse*light_color*pixel + 0.15*light_color*pixel;// + 0.1*spec*light_color;
}

void main() 
{
	const vec3 lightPos = vec3(500, 500, 500);
	vec3 lightVec = normalize(lightPos - inPosition);
	vec4 lightingColor = lighting(vec4(inColor, 1.0), lightVec, normalize(inNormal));
	outFragColor = vec4(lightingColor.xyz, 1.0);
	// outFragColor = vec4(inColor,1.0f);
}

#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;

layout (location = 0) in vec3 inColor[]; 
layout (location = 1) in vec2 inUV[];    
layout (location = 2) in vec3 inNormal[];


layout (location = 0) out vec3 outColor;                       
layout (location = 1) out vec2 outUV;                       

// Find axis which maximized orthographic projected area of triangle
vec3 getDominantAxis(vec3 normal)
{
    // TODO: more efficient
    float a1 = length(dot(normal, vec3(1, 0, 0)));
    float a2 = length(dot(normal, vec3(0, 1, 0)));
    float a3 = length(dot(normal, vec3(0, 0, 1)));

    if(a1 > a2 && a1 > a3) return vec3(1, 0, 0);
    else if(a2 > a1 && a2 > a3) return vec3(0, 1, 0);
    else return vec3(0, 0, 1);
}

vec3 orthographicProjection(vec3 position, vec3 dominantAxis)
{
    return position - dominantAxis * dot(position, dominantAxis);
}

void main() {
    vec3 normal = normalize(inNormal[0] + inNormal[1] + inNormal[2]);
    vec3 dominantAxis = getDominantAxis(normal);

    // Iterate through the input vertices of the primitive
    for (int i = 0; i < 3; i++) {
        // Pass attributes to the next stage
        outColor = inColor[i];
        outUV = inUV[i];

        // Set the position of the output vertex
        vec4 outPosition = vec4(orthographicProjection(gl_in[i].gl_Position.xyz, dominantAxis), 1.0);
        gl_Position = outPosition;

        // Emit the vertex
        EmitVertex();
    }

    // Complete the current primitive
    EndPrimitive();
}
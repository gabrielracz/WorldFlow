#version 450

layout(triangles) in;                  // Input primitive type
layout(triangle_strip, max_vertices=3) out; // Output primitive type and maximum vertices

in vec3 inColor[];                      // Example: color passed per vertex
in vec2 inUV[];                      // Example: color passed per vertex


out vec3 outColor;                       
out vec3 outUV;                       

void main() {
    // Iterate through the input vertices of the primitive
    for (int i = 0; i < 3; i++) {
        // Pass attributes to the next stage
        outColor = inUV[i];
        outUV = inUV[i];

        // Set the position of the output vertex
        gl_Position = gl_in[i].gl_Position;

        // Emit the vertex
        EmitVertex();
    }

    // Complete the current primitive
    EndPrimitive();
}
#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices=3) out;

layout (location = 0) in vec3 inColor[]; 
layout (location = 1) in vec2 inUV[];    
layout (location = 2) in vec3 inNormal[];


layout (location = 0) out vec3 outColor;                       
layout (location = 1) out vec2 outUV;                       
layout (location = 2) out float outDepth;                       
layout (location = 3) out flat int outAxis;                       

// Find axis which maximized orthographic projected area of triangle
vec3 getDominantAxis(vec3 normal)
{
    // TODO: more efficient
    float a1 = length(dot(normal, vec3(1, 0, 0)));
    float a2 = length(dot(normal, vec3(0, 1, 0)));
    float a3 = length(dot(normal, vec3(0, 0, 1)));

    if(a1 > a2 && a1 > a3) return vec3(1, 0, 0);
    else if(a2 > a1 && a2 > a3) return vec3(0, -1, 0);
    else return vec3(0, 0, -1);
}

int getDominantAxisIndex(vec3 normal)
{
    // TODO: more efficient
    float a1 = length(dot(normal, vec3(1, 0, 0)));
    float a2 = length(dot(normal, vec3(0, 1, 0)));
    float a3 = length(dot(normal, vec3(0, 0, 1)));

    if(a1 > a2 && a1 > a3) return 0;
    else if(a2 > a1 && a2 > a3) return 1;
    else return 2;
}

vec3 orthographicProjection(vec3 position, vec3 dominantAxis)
{
    return position - dominantAxis * dot(position, dominantAxis);
}

vec4 orthographicProjection(vec3 position, int axisIndex)
{
    mat4 mvpx = mat4x4(
        0.000000, 0.000000, 1.000000, 0.000000,
        0.000000, 2.000000, 0.000000, 0.000000,
        -2.000000, 0.000000, 0.000000, 0.000000,
        -0.000000, -0.000000, 0.000000, 1.000000);

    mat4 mvpy = mat4x4(
        2.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 1.000000, 0.000000,
        0.000000, -2.000000, 0.000000, 0.000000,
        -0.000000, -0.000000, 0.000000, 1.000000);



    mat4 mvpz = mat4x4(
        2.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 2.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 1.000000, 0.000000,
        -0.000000, -0.000000, 0.000000, 1.000000);


    mat4 proj;
    if(axisIndex == 0)  proj = mvpx;
    else if(axisIndex == 1) proj = mvpy;
    else proj = mvpz;

    return proj * vec4(position, 1.0);
}


void main() {
    vec3 normal = normalize(inNormal[0] + inNormal[1] + inNormal[2]);
    vec3 dominantAxis = getDominantAxis(normal);
    int dominantAxisIndex = getDominantAxisIndex(normal);

    // Iterate through the input vertices of the primitive
    vec3 fakePos[3];
    // fakePos[0] = vec3(-0.5, -0.5,  -0.5);
    // fakePos[1] = vec3( 0.0,  0.5,  0.5);
    // fakePos[2] = vec3( 0.5, -0.5,  -0.5);

    fakePos[0] = vec3(-0.5, -0.5,  -0.5);
    fakePos[1] = vec3( 0.0,  0.5,  0.0);
    fakePos[2] = vec3( 0.5, -0.5,  0.5);

    for (int i = 0; i < 3; i++) {
        // Pass attributes to the next stage
        outColor = inColor[i];
        outUV = inUV[i];

        // vec3 pos = fakePos[i];
        vec3 pos = gl_in[i].gl_Position.xyz;
        vec4 outPosition = vec4(orthographicProjection(pos, dominantAxis), 1.0);
        // dominantAxisIndex = 0;
        // vec4 outPosition = orthographicProjection(pos, dominantAxisIndex);
        // vec4 outPosition = vec4(2.0*pos.xy, 0.0, 1.0);
        // vec4 outPosition = orthographicProjection(pos, 2);
        // vec4 outPosition = mvpz * vec4(pos, 1.0);
        gl_Position = outPosition;
        outDepth = pos.z;
        outAxis = dominantAxisIndex;

        // Emit the vertex
        EmitVertex();
    }

    // Complete the current primitive
    EndPrimitive();
}
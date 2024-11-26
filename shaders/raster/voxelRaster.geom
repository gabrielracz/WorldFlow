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

// int getDominantAxisIndex(vec3 normal)
// {
//     // TODO: more efficient
//     float a1 = length(dot(normal, vec3(1, 0, 0)));
//     float a2 = length(dot(normal, vec3(0, 1, 0)));
//     float a3 = length(dot(normal, vec3(0, 0, 1)));

//     if(a1 > a2 && a1 > a3) return 0;
//     else if(a2 > a1 && a2 > a3) return 1;
//     else return 2;
// }

int getDominantAxisIndex(vec3 normal)
{
    // Compute the absolute values of the normal's components
    float absX = abs(normal.x);
    float absY = abs(normal.y);
    float absZ = abs(normal.z);

    // Determine the dominant axis based on the largest component
    if (absX > absY && absX > absZ) return 0; // x-axis
    else if (absY > absX && absY > absZ) return 1; // y-axis
    else return 2; // z-axis
}

// vec3 orthographicProjection(vec3 position, vec3 dominantAxis)
// {
//     return position - dominantAxis * dot(position, dominantAxis);
// }

// vec4 orthographicProjection(vec3 position, int dominantAxis)
// {
//     mat3 viewRotation;

//     // Define view rotation matrices for each dominant axis
//     if (dominantAxis == 0) {
//         // View along +X: Forward = -X, Up = +Y
//         viewRotation = mat3(
//             0.0,  0.0,  1.0,  // Right (Z)
//             0.0,  1.0,  0.0,  // Up (Y)
//            -1.0,  0.0,  0.0   // Forward (-X)
//         );
//     } else if (dominantAxis == 1) {
//         // View along +Y: Forward = -Y, Up = -Z
//         viewRotation = mat3(
//             1.0,  0.0,  0.0,  // Right (X)
//             0.0,  0.0, -1.0,  // Up (-Z)
//             0.0, -1.0,  0.0   // Forward (-Y)
//         );
//     } else {
//         // View along +Z: Forward = -Z, Up = +Y
//         viewRotation = mat3(
//             1.0,  0.0,  0.0,  // Right (X)
//             0.0,  1.0,  0.0,  // Up (Y)
//             0.0,  0.0, -1.0   // Forward (-Z)
//         );
//     }

//     // Apply view rotation
//     vec3 viewPosition = viewRotation * position;

//     // Translate the camera
//     viewPosition += vec3(0.5, 0.0, 0.0) * (dominantAxis == 0 ? 1.0 : 0.0);
//     viewPosition += vec3(0.0, 0.5, 0.0) * (dominantAxis == 1 ? 1.0 : 0.0);
//     viewPosition += vec3(0.0, 0.0, 0.5) * (dominantAxis == 2 ? 1.0 : 0.0);

//     // Apply orthographic projection (scale to [-1, 1] and clip space)
//     viewPosition = 2.0 * (viewPosition - vec3(-0.5)) - vec3(1.0);

//     // Return as clip space coordinates
//     return vec4(viewPosition, 1.0);
// }

vec4 orthographicProjection(vec3 position, int axisIndex)
{
    mat4 mvpx = mat4(
        0.000000, 0.000000, 2.000000, 0.000000,
        0.000000, 2.000000, 0.000000, 0.000000,
        -2.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 0.000000, 1.000000);

    mat4 mvpy = mat4(
        2.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 0.000000, -2.000000, 0.000000,
        0.000000, -2.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 0.000000, 1.000000);

    mat4 mvpz = mat4(
        2.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 2.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 2.000000, 0.000000,
        0.000000, 0.000000, 0.000000, 1.000000);

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
        // vec4 outPosition = vec4(orthographicProjection(pos, dominantAxis), 1.0);

        // dominantAxisIndex = 0;
        vec4 outPosition = orthographicProjection(pos, dominantAxisIndex);
        // vec4 outPosition = vec4(2.0*pos.xy, 0.0, 1.0);
        // vec4 outPosition = orthographicProjection(pos, 2);
        // vec4 outPosition = mvpz * vec4(pos, 1.0);
        gl_Position = outPosition;
        // if(dominantAxisIndex == 0) {
        //     outDepth = pos.x;
        // } else if (dominantAxisIndex == 1) {
        //     outDepth = pos.y;
        // } else {
        //     outDepth = pos.z;
        // }
        outDepth = (outPosition.z + 1.0) / 2.0;
        outAxis = dominantAxisIndex;
        // Emit the vertex
        EmitVertex();
    }

    // Complete the current primitive
    EndPrimitive();
}
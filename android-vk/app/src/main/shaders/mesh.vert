#version 450
// Each mesh is moved into the scene frame by `offset`, then transformed by the view-projection.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inCol;   // unorm8 colour from the vertex buffer

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 offset;     // xyz used
} pc;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vCol;

void main() {
    vec3 world = inPos + pc.offset.xyz;
    gl_Position = pc.viewProj * vec4(world, 1.0);
    vWorld = world;
    vCol = inCol.rgb;
}

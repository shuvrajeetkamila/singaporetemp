#version 450
// Flat pass-through with a single mat4 (view * projection) in push constants.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNrm;
layout(location = 2) in vec3 inCol;

layout(push_constant) uniform Push {
    mat4 viewProj;
} pc;

layout(location = 0) out vec3 vNrm;
layout(location = 1) out vec3 vCol;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    vNrm = inNrm;
    vCol = inCol;
}

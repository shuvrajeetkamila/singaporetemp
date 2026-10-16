#version 450
// Two-sided Lambert with a fixed sun direction and an ambient floor. The normal is per vertex
// (from the vertex buffer), so flat faces stay flat and edges between faces do not streak.
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vCol;
layout(location = 2) in vec3 vNrm;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 L = normalize(vec3(0.45, 0.80, 0.40));
    vec3 n = normalize(vNrm);
    float d = abs(dot(n, L));
    float light = 0.35 + 0.65 * d;
    outColor = vec4(vCol * light, 1.0);
}

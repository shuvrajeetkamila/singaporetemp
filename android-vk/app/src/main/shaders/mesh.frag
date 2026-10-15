#version 450
// Simple two-sided Lambert with a fixed sun direction and an ambient floor.
layout(location = 0) in vec3 vNrm;
layout(location = 1) in vec3 vCol;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 L = normalize(vec3(0.45, 0.80, 0.40));
    float d = abs(dot(vNrm, L));
    float light = 0.35 + 0.65 * d;
    outColor = vec4(vCol * light, 1.0);
}

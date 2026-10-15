#version 450
// Flat-face normal from screen-space derivatives of the world position (no normals stored).
// Two-sided Lambert with a fixed sun direction and an ambient floor.
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vCol;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 c = cross(dFdx(vWorld), dFdy(vWorld));
    vec3 L = normalize(vec3(0.45, 0.80, 0.40));
    float len = length(c);
    float d = (len > 1e-12) ? abs(dot(c, L)) / len : 0.5;
    float light = 0.35 + 0.65 * d;
    outColor = vec4(vCol * light, 1.0);
}

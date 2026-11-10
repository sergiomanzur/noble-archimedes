#version 450
layout(location = 0) out vec2 vUV;

void main() {
    // Full-screen triangle. p is (0,0), (2,0), (0,2); vUV is 0..1 across the screen.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vUV = p;
}

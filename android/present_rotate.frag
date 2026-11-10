#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 rot;  // 2x2 row-major rotation matrix
    vec4 off;  // xy translation
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = vec2(pc.rot.x * vUV.x + pc.rot.y * vUV.y,
                   pc.rot.z * vUV.x + pc.rot.w * vUV.y) + pc.off.xy;
    outColor = texture(src, uv);
}

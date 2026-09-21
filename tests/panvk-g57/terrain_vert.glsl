#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(push_constant) uniform PushConsts {
    mat4 mvp;
} push;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = push.mvp * vec4(inPosition, 1.0);
    vec3 lightDir = normalize(vec3(0.5, 0.8, 0.4));
    float diff = max(dot(inNormal, lightDir), 0.2);
    fragColor = inColor * diff;
}

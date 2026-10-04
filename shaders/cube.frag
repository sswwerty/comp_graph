#version 450
layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec3 fragment_color;
layout(location = 0) out vec4 out_color;
layout(set = 1, binding = 0, std140) uniform Model {
    mat4 model;
    mat4 rotation;
    vec4 tint;
} object;
void main() {
    vec3 light_direction = normalize(vec3(-0.5, 0.8, -1.0));
    float lighting = 0.42 + 0.58 * max(dot(normalize(fragment_normal), light_direction), 0.0);
    vec3 local_color = mix(vec3(1.0), fragment_color, object.tint.a);
    out_color = vec4(local_color * object.tint.rgb * lighting, 1.0);
}

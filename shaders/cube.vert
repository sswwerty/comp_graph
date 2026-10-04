#version 450
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;
layout(location = 0) out vec3 fragment_normal;
layout(location = 1) out vec3 fragment_color;
layout(set = 0, binding = 0, std140) uniform Scene {
    mat4 view_projection;
} scene;
layout(set = 1, binding = 0, std140) uniform Model {
    mat4 model;
    mat4 rotation;
    vec4 tint;
} object;
void main() {
    gl_Position = scene.view_projection * object.model * vec4(in_position, 1.0);
    fragment_normal = mat3(object.rotation) * in_normal;
    fragment_color = in_color;
}

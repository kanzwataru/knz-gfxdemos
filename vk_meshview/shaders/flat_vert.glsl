#version 450

layout (location = 0) in vec3 position;

layout (push_constant) uniform constants {
    mat4 model_mat;
};

layout (set = 0, binding = 0) uniform Global_Uniforms {
    mat4 view_mat;
    mat4 proj_mat;
    mat4 view_proj_mat;
} u;

void main() {
    gl_Position = u.view_proj_mat * model_mat * vec4(position, 1.0);
}

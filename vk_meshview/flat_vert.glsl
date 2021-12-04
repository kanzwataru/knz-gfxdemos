#version 450

layout (location = 0) in vec3 position;

layout (push_constant) uniform constants {
    vec4 data;
    mat4 model_matrix;
    mat4 view_proj_matrix;
};

void main() {
    gl_Position = view_proj_matrix * model_matrix * vec4(position, 1.0);
}

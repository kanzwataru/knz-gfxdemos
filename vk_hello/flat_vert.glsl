#version 450

layout (location = 0) in vec3 position;

layout (push_constant) uniform constants {
    vec4 data;
    mat4 matrix;
};

void main() {
    gl_Position = matrix * vec4(position, 1.0);
}

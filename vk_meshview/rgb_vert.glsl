#version 450

// in
layout (location = 0) in vec3 position;
layout (location = 1) in vec3 color;

// out
layout (location = 0) out vec3 out_color;

void main() {
    gl_Position = vec4(position, 1.0);
	out_color = color;
}

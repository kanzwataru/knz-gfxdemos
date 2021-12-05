#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 uv;

layout (location = 0) out vec3 out_world_normal;
layout (location = 1) out vec2 out_uv;

layout (push_constant) uniform constants {
    mat4 model_matrix;
    mat4 view_proj_matrix;
};

void main() {
    gl_Position = view_proj_matrix * model_matrix * vec4(position, 1.0);

    out_uv = uv;
    
    // NOTE: This assumes no non-uniform scaling
    out_world_normal = (model_matrix * vec4(normal, 0.0)).xyz;
    out_world_normal = normalize(out_world_normal);
}

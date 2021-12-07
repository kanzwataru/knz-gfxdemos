#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 uv;

layout (location = 0) out vec3 out_world_normal;
layout (location = 1) out vec2 out_uv;

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

    out_uv = uv;
    
    // NOTE: This assumes no non-uniform scaling
    out_world_normal = (model_mat * vec4(normal, 0.0)).xyz;
    out_world_normal = normalize(out_world_normal);
}

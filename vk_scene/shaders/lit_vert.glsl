#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 uv;

layout (location = 0) out vec3 out_world_normal;
layout (location = 1) out vec2 out_uv;

layout (set = 0, binding = 0) uniform Global_Uniforms {
    mat4 view_mat;
    mat4 proj_mat;
    mat4 view_proj_mat;
} u;

struct Instance_Data {
    mat4 model_mat;
};

layout (set = 0, binding = 1) readonly buffer Instance_Data_Buffer {
    Instance_Data instance_data[];
};

void main() {
    Instance_Data instance = instance_data[gl_InstanceIndex];

    gl_Position = u.view_proj_mat * instance.model_mat * vec4(position, 1.0);

    out_uv = uv;
    
    // NOTE: This assumes no non-uniform scaling
    out_world_normal = (instance.model_mat * vec4(normal, 0.0)).xyz;
    out_world_normal = normalize(out_world_normal);
}

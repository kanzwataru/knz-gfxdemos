#version 450

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
};

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

layout (set = 0, binding = 2) readonly buffer Vertex_Buffer {
    float vertex_data[];
};

Vertex load_vertex(uint id)
{
    // TODO: This is only like this because of struct packing rules, fix things so that this is not necessary
    Vertex v;

    v.position = vec3(vertex_data[id * 8 + 0], vertex_data[id * 8 + 1], vertex_data[id * 8 + 2]);
    v.normal = vec3(vertex_data[id * 8 + 3], vertex_data[id * 8 + 4], vertex_data[id * 8 + 5]);
    v.uv = vec2(vertex_data[id * 8 + 6], vertex_data[id * 8 + 7]);

    return v;
}

void main() {
    Instance_Data instance = instance_data[gl_InstanceIndex];
    Vertex v = load_vertex(gl_VertexIndex);

    gl_Position = u.view_proj_mat * instance.model_mat * vec4(v.position, 1.0);

    out_uv = v.uv;
    
    // NOTE: This assumes no non-uniform scaling
    out_world_normal = (instance.model_mat * vec4(v.normal, 0.0)).xyz;
    out_world_normal = normalize(out_world_normal);
}

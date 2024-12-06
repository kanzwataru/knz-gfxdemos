#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec2 uv;
layout(location = 2) in flat uint instance_id;

layout(location = 0) out vec4 out_color;

struct Instance_Data {
    mat4 model_mat;
    uint texture_id;
    uint padding_0;
    uint padding_1;
    uint padding_2;
};

layout (set = 0, binding = 1) readonly buffer Instance_Data_Buffer {
    Instance_Data instance_data[];
};

layout(set = 0, binding = 3) uniform sampler2D textures[];

void main() {
	Instance_Data instance = instance_data[instance_id];

	vec3 lit_col = vec3(0.8, 0.08, 0.05);
	vec3 dark_col = vec3(0.25, 0.01, 0.05);
	
	vec3 light_dir = normalize(vec3(1.0, 0.25, 0.0));
	float ndotl = dot(world_normal, light_dir);
	float half_diffuse = ndotl * 0.5 + 0.5;
	
	vec3 col = texture(textures[nonuniformEXT(instance.texture_id)], uv).rgb * mix(dark_col, lit_col, half_diffuse);
	out_color = vec4(col, 1.0);
}

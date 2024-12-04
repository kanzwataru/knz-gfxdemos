#version 450
layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec2 uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 3) uniform sampler2D tex_color;

void main() {
	vec3 lit_col = vec3(0.8, 0.08, 0.05);
	vec3 dark_col = vec3(0.25, 0.01, 0.05);
	
	vec3 light_dir = normalize(vec3(1.0, 0.25, 0.0));
	float ndotl = dot(world_normal, light_dir);
	float half_diffuse = ndotl * 0.5 + 0.5;
	
	vec3 col = texture(tex_color, uv).rgb * mix(dark_col, lit_col, half_diffuse);
	out_color = vec4(col, 1.0);
}

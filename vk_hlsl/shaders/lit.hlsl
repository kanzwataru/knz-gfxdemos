struct Constants {
	float4x4 model_mat;
};

[[vk::push_constant]]
Constants pc;

cbuffer GlobalUniforms : register(b0) {
	float4x4 view_mat;
	float4x4 proj_mat;
	float4x4 view_proj_mat;
};

struct VSIn {
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
};

struct VSOut {
	float4 pos : SV_POSITION;
	float3 world_normal : NORMAL;
	float2 uv : TEXCOORD0;
};

struct PSOut {
	float4 color : SV_TARGET;
};

VSOut vs_main(VSIn i)
{
	VSOut o;
	o.pos = mul(mul(view_proj_mat, pc.model_mat), float4(i.pos, 1.0f));
	o.uv = i.uv;

	// NOTE: This assumes no non-uniform scaling
	o.world_normal = normalize(mul(pc.model_mat, float4(i.normal, 0.0f)).xyz);

	return o;
}

PSOut ps_main(VSOut i)
{
	PSOut o;

	float3 lit_col = float3(0.8, 0.08, 0.05);
	float3 dark_col = float3(0.25, 0.01, 0.05);

	float3 light_dir = normalize(float3(1.0, 0.25, 0.0));
	float ndotl = dot(i.world_normal, light_dir);
	float half_diffuse = ndotl * 0.5 + 0.5;
	
	float3 col = lerp(dark_col, lit_col, half_diffuse);

	o.color = float4(col, 1.0);
	return o;
}

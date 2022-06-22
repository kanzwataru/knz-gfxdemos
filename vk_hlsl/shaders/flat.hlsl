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
};

struct VSOut {
	float4 pos : SV_POSITION;
};

struct PSOut {
	float4 color : SV_TARGET;
};

VSOut vs_main(VSIn i)
{
	VSOut o;
	o.pos = mul(mul(view_proj_mat, pc.model_mat), float4(i.pos, 1.0f));
	return o;
}

PSOut ps_main(VSOut i)
{
	PSOut o;
	o.color = float4(0.8, 0.08, 0.05, 1.0);
	return o;
}

sampler2D s_texture : register(s0);
float g_multiplier : register(c200);

float4 main(float2 texCoord : TEXCOORD0) : COLOR0
{
    float stencil = round(tex2D(s_texture, texCoord).a * 255.0f);
    clip(frac(floor(stencil * g_multiplier) / 2) - 0.25f);
    return 0;
}

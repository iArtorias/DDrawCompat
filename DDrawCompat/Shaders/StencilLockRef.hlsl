sampler2D s_texture : register(s0);
sampler2D s_lockRef : register(s1);
float g_multiplier : register(c200);

float4 main(float2 texCoord : TEXCOORD0) : COLOR0
{
    float texAlpha = tex2D(s_texture, texCoord).a;
    float refAlpha = tex2D(s_lockRef, texCoord).a;
    clip(texAlpha == refAlpha ? -1 : 1);
    float stencil = round(texAlpha * 255.0f);
    clip(frac(floor(stencil * g_multiplier) / 2) - 0.25f);
    return 0;
}

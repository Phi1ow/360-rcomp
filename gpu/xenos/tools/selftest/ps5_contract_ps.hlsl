// rcomp-stage: ps
// rcomp-expect: PASS
// Self-authored (R-comp). The PS5 profile contract: register file as one UBO
// (set 0 binding 1), shared constants UBO (set 0 binding 2), fixed-slot
// texture/sampler pairs (set 1, binding 2*slot / 2*slot+1), spec constant 0,
// dynamic (a0-relative) constant indexing clamped to the register file, kill.
[[vk::binding(1, 0)]] cbuffer PixelShaderConstants { float4 g_PixelShaderConstants[224]; };
[[vk::binding(2, 0)]] cbuffer SharedConstants
{
    uint g_Booleans;
    uint g_SwappedTexcoords;
    float2 g_HalfPixelOffset;
    float g_AlphaThreshold;
};
[[vk::constant_id(0)]] const uint g_SpecConstants = 0;
[[vk::binding(0, 1)]] Texture2D<float4> g_Texture2D_s0;
[[vk::binding(1, 1)]] SamplerState g_Sampler_s0;
[[vk::binding(2, 1)]] TextureCube<float4> g_TextureCube_s1;
[[vk::binding(3, 1)]] SamplerState g_Sampler_s1;
#define g_Palette(INDEX) select((INDEX) < 216, g_PixelShaderConstants[8 + min(INDEX, 215)], 0.0)

float4 main(float4 iPos : SV_Position, float4 iTexCoord0 : TEXCOORD0, bool iFace : SV_IsFrontFace) : SV_Target0
{
    int a0 = (int)clamp(floor(iTexCoord0.z + 0.5), -256.0, 255.0);
    float3 dirs[2];
    dirs[0] = iTexCoord0.xyz;
    dirs[1] = -iTexCoord0.xyz;
    float4 c = g_Texture2D_s0.Sample(g_Sampler_s0, iTexCoord0.xy) * g_Palette(a0);
    c += g_TextureCube_s1.Sample(g_Sampler_s1, dirs[(uint)iTexCoord0.w & 1]);
    if ((g_Booleans & (1 << 16)) != 0)
        c *= g_PixelShaderConstants[0];
    if (g_SpecConstants & 2)
        clip(c.w - g_AlphaThreshold);
    return c * (iFace ? 1.0 : 0.5);
}

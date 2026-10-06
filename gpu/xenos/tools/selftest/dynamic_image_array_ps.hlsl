// rcomp-stage: ps
// rcomp-expect: FAIL
// Self-authored (R-comp). A *bounded* descriptor array indexed by a uniform
// value -- the "halfway bindless" option. Needs
// shaderSampledImageArrayDynamicIndexing, which PS5_Vulkan leaves off
// (ps5vk_physical_device.c:51-67). See the audit for why the gate may not see
// it from capabilities alone.
[[vk::binding(2, 0)]] cbuffer SharedConstants { uint g_TextureIndex; };
[[vk::binding(0, 1)]] Texture2D<float4> g_Textures[8];
[[vk::binding(1, 1)]] SamplerState g_Sampler;
float4 main(float4 iTexCoord0 : TEXCOORD0) : SV_Target0
{
    return g_Textures[g_TextureIndex & 7].Sample(g_Sampler, iTexCoord0.xy);
}

// rcomp-stage: ps
// rcomp-expect: FAIL
// Self-authored (R-comp). A 3D texture fetch (Xenos tfetch3D). Valid SPIR-V,
// but PS5_Vulkan@9639c41 refuses non-2D/2D-array/cube views at draw time
// (driver/ps5vk_draw.c:1341-1350).
[[vk::binding(0, 1)]] Texture3D<float4> g_Texture3D_s0;
[[vk::binding(1, 1)]] SamplerState g_Sampler_s0;
float4 main(float4 iTexCoord0 : TEXCOORD0) : SV_Target0
{
    return g_Texture3D_s0.Sample(g_Sampler_s0, iTexCoord0.xyz);
}

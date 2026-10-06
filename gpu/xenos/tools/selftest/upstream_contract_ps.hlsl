// rcomp-stage: ps
// rcomp-expect: FAIL
// Self-authored (R-comp). Reproduces the *upstream* XenosRecomp SPIR-V runtime
// contract in miniature: 64-bit GPU addresses in push constants read with
// vk::RawBufferLoad, and bindless unbounded descriptor heaps indexed by values
// loaded from memory (XenosRecomp/shader_common.h:18-53). Expected to FAIL the
// PS5_Vulkan gates (BDA addressing model, Int64, RuntimeDescriptorArray).
struct PushConstants
{
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

float4 main(float4 uv : TEXCOORD0) : SV_Target0
{
    uint tex = vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0);
    uint smp = vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 192);
    float4 tint = vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 16, 0x10);
    return g_Texture2DDescriptorHeap[tex].Sample(g_SamplerDescriptorHeap[smp], uv.xy) * tint;
}

// rcomp-stage: ps
// rcomp-expect: FAIL
// Self-authored (R-comp). Two colour exports (oC0, oC1). Compiles, but
// PS5_Vulkan@9639c41 refuses a rendering with >1 colour attachment
// (driver/ps5vk_draw.c:947-955), so the draw-time gate must FAIL it.
[[vk::binding(1, 0)]] cbuffer PixelShaderConstants { float4 g_PixelShaderConstants[224]; };
struct PSOut { float4 oC0 : SV_Target0; float4 oC1 : SV_Target1; };
PSOut main(float4 iTexCoord0 : TEXCOORD0)
{
    PSOut o;
    o.oC0 = iTexCoord0 * g_PixelShaderConstants[0];
    o.oC1 = iTexCoord0 * g_PixelShaderConstants[1];
    return o;
}

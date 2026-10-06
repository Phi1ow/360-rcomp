// rcomp-stage: vs
// rcomp-expect: PASS
// Self-authored (R-comp). VS side of the PS5 profile: register-file UBO at
// set 0 binding 0, vertex inputs at explicit locations (< 16), half-pixel
// offset applied in clip space, -fvk-invert-y from the XenosRecomp flag set.
[[vk::binding(0, 0)]] cbuffer VertexShaderConstants { float4 g_VertexShaderConstants[256]; };
[[vk::binding(2, 0)]] cbuffer SharedConstants
{
    uint g_Booleans;
    uint g_SwappedTexcoords;
    float2 g_HalfPixelOffset;
    float g_AlphaThreshold;
};
struct VSOut { float4 oPos : SV_Position; float4 oTexCoord0 : TEXCOORD0; };
VSOut main([[vk::location(0)]] float4 iPosition0 : POSITION0,
           [[vk::location(4)]] float4 iTexCoord0 : TEXCOORD0,
           [[vk::location(1)]] uint4 iNormal0 : NORMAL0)
{
    VSOut o;
    o.oPos = float4(dot(iPosition0, g_VertexShaderConstants[4]), dot(iPosition0, g_VertexShaderConstants[5]),
                    dot(iPosition0, g_VertexShaderConstants[6]), dot(iPosition0, g_VertexShaderConstants[7]));
    float4 t = (g_SwappedTexcoords & 1u) != 0 ? iTexCoord0.yxwz : iTexCoord0;
    o.oTexCoord0 = t + asfloat(iNormal0) * 0.0;
    o.oPos.xy += g_HalfPixelOffset * o.oPos.w;
    return o;
}

// R-comp PS5 shader prelude for XenosRecomp output (profile XENOS_RECOMP_PS5).
//
// Derived from XenosRecomp/shader_common.h @990d03b28a27b50277ee5d8d942e1c5f873869d1
// (MIT, Copyright (c) hedge-dev / Skyth; see XenosRecomp LICENSE.md). Changes:
//   * no push-constant block of 64-bit GPU addresses and no vk::RawBufferLoad
//     (PS5_Vulkan exposes neither bufferDeviceAddress nor shaderInt64);
//   * shared constants are a plain uniform buffer at set 0, binding 2;
//   * no bindless descriptor heaps: every fetch takes the Texture/SamplerState
//     object the generator declared at a fixed (set, binding) slot;
//   * `1ull` -> `1u` (64-bit literal forced OpCapability Int64).
// It is passed to XenosRecomp as its third argument ("shader common header").
// Target: DXC -spirv (SPIR-V 1.0, vulkan1.0), consumed by PS5_Vulkan only.

#ifndef SHADER_COMMON_PS5_H_INCLUDED
#define SHADER_COMMON_PS5_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

// Set 0 binding 2, shared by both stages. Layout (std140/dx layout, 32 bytes):
//   0 g_Booleans (VS bits 0-15, PS bits 16-31)   4 g_SwappedTexcoords
//   8 g_HalfPixelOffset                          16 g_AlphaThreshold
[[vk::binding(2, 0)]] cbuffer SharedConstants
{
    uint g_Booleans;
    uint g_SwappedTexcoords;
    float2 g_HalfPixelOffset;
    float g_AlphaThreshold;
};

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;
#define g_SpecConstants() g_SpecConstants

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D_ps5(Texture2D<float4> texture, SamplerState samplerState, float2 texCoord, float2 offset)
{
    return texture.Sample(samplerState, texCoord + offset / getTexture2DDimensions(texture));
}

float2 getWeights2D_ps5(Texture2D<float4> texture, SamplerState samplerState, float2 texCoord, float2 offset)
{
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float4 tfetch3D_ps5(Texture3D<float4> texture, SamplerState samplerState, float3 texCoord)
{
    return texture.Sample(samplerState, texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube_ps5(TextureCube<float4> texture, SamplerState samplerState, float3 texCoord, inout CubeMapData cubeMapData)
{
    return texture.Sample(samplerState, cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

#endif

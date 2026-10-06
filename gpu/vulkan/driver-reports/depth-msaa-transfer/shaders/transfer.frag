#version 450
#extension GL_ARB_shader_stencil_export : require
layout(set=0,binding=0) uniform sampler2DMS source_depth;
layout(set=0,binding=1) uniform usampler2DMS source_stencil;
void main() {
    ivec2 d=ivec2(gl_FragCoord.xy);
    int source_sample=(d.x&1)|((gl_SampleID^1)<<1);
    ivec2 source_pixel=ivec2(d.x>>1,d.y);
    gl_FragDepth=texelFetch(source_depth,source_pixel,source_sample).r;
    gl_FragStencilRefARB=int(texelFetch(source_stencil,source_pixel,source_sample).r);
}

#version 450
#extension GL_ARB_shader_stencil_export : require
layout(push_constant) uniform Push { uint mode,width,height,samples; float yscale,yoffset,xoffset; uint tag; } p;
layout(location=0) out vec4 color;
float pattern_depth(uint x,uint y,uint s) { return 0.125+float((x*3u+y*5u+s*7u)&7u)*0.0625; }
void main() {
    uint x=uint(gl_FragCoord.x),y=uint(gl_FragCoord.y),s=uint(gl_SampleID);
    float depth=0.75; uint stencil=165u; color=vec4(0);
    if(p.mode==0u) { depth=pattern_depth(x,y,s); stencil=(x*13u+y*7u+s*29u)&255u; }
    else if(p.mode==2u) { depth=pattern_depth(x>>1u,y,(x&1u)|((s^1u)<<1u)); }
    else if(p.mode==3u || p.mode==4u) { depth=0.25; stencil=p.tag; }
    if(p.mode==4u) { color=vec4(p.tag==0x82u ? 1.0:0.0,p.tag==0x81u ? 1.0:0.0,p.tag==8u ? 1.0:0.0,1.0); }
    gl_FragDepth=depth; gl_FragStencilRefARB=int(stencil);
}

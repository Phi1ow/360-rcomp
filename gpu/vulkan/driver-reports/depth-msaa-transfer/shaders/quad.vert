#version 450
layout(push_constant) uniform Push { uint mode,width,height,samples; float yscale,yoffset,xoffset; uint tag; } p;
const vec2 positions[6]=vec2[6](vec2(-1.02,-1.05),vec2(1.02,-1.05),vec2(-1.02,1.05),vec2(-1.02,1.05),vec2(1.02,-1.05),vec2(1.02,1.05));
void main() { vec2 v=positions[gl_VertexIndex]; gl_Position=vec4(v.x+p.xoffset,v.y*p.yscale+p.yoffset,0.0,1.0); }

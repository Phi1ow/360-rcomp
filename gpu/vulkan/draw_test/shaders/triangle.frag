#version 450
// R-comp Agent 4 draw test: flat colour from one uniform buffer (set 0, binding 0).
layout(set = 0, binding = 0, std140) uniform Colour {
    vec4 colour;
} ubo;
layout(location = 0) out vec4 out_colour;
void main()
{
    out_colour = ubo.colour;
}

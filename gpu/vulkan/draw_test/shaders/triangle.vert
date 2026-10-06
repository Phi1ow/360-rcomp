#version 450
// R-comp Agent 4 draw test: position only, no transforms. SPIR-V 1.0 / Vulkan 1.0.
layout(location = 0) in vec2 in_position;
void main()
{
    gl_Position = vec4(in_position, 0.0, 1.0);
}

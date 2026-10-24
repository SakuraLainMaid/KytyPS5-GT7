#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

// A single-sample colour image into every sample of a multisample image of its size, the EQAA
// fallback's temporary (BlitHelper::ReplicateColorToMs).
// ponytail: float-typed fetch; integer formats need a utexture2D/itexture2D variant if they show up.
layout(binding = 0, set = 0) uniform texture2D color;

layout(location = 0) out vec4 result;

void main() {
	result = texelFetch(color, ivec2(gl_FragCoord.xy), 0);
}

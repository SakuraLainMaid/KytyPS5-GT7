#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

// Round 124: a multisample depth/stencil target's stencil, first sample, into an R8_UINT image,
// for single-sample texture reads of the stencil (BlitHelper::CopyMsStencilSample0).
layout(binding = 0, set = 0) uniform utexture2DMS stencil;

layout(location = 0) out uint result;

void main() {
	result = texelFetch(stencil, ivec2(gl_FragCoord.xy), 0).r;
}

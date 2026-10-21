#include "graphics/shader/shader.h"

#include "common/assert.h"

namespace Libs::Graphics {

namespace {

constexpr uint32_t PsInputOffsetMask = 0x0000001fu;
constexpr uint32_t PsInputFlatShade  = 0x00000400u;

} // namespace

uint32_t ShaderPixelParameterMappedLocation(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num ? info.interpolator_settings[input] & PsInputOffsetMask : input;
}

// Every alias of a vertex output reads that output's own location, flat and smooth ones too: a
// vertex shader writes each parameter once, at its index, and knows nothing of the pixel shader's
// aliases, so a location of their own was never written (a crowd shader's flat alias of a smooth
// UV export read garbage for its light probe index). A location read both flat and smooth is
// read per vertex (ShaderInfoCollection), the flat alias taking vertex zero.
uint32_t ShaderPixelParameterLocation(const ShaderPixelInputInfo& info,
                                      std::span<const uint32_t> /*active_inputs*/, uint32_t input) {
	return ShaderPixelParameterMappedLocation(info, input);
}

bool ShaderPixelParameterIsFlat(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < info.input_num && (info.interpolator_settings[input] & PsInputFlatShade) != 0 &&
	       !ShaderPixelParameterIsCustom(info, input);
}

bool ShaderPixelParameterIsCustom(const ShaderPixelInputInfo& info, uint32_t input) {
	return input < 32u && (info.custom_interpolation_mask & (1u << input)) != 0;
}

} // namespace Libs::Graphics

#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_

#include <array>
#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Descriptor set 1 of a pipeline that samples bindless images (the host side is BindlessTable).
inline constexpr uint32_t BindlessDescriptorSet  = 1;
inline constexpr uint32_t BindlessImages2D       = 0;
inline constexpr uint32_t BindlessImages2DArray  = 1;
inline constexpr uint32_t BindlessImagesCube     = 2;
inline constexpr uint32_t BindlessImages3D       = 3;
inline constexpr uint32_t BindlessTranslation    = 4;
inline constexpr uint32_t BindlessFeedback       = 5;
// Samplers mirrored from guest sampler heaps (16-byte S# records); slot 0 is the default sampler.
inline constexpr uint32_t BindlessSamplers       = 6;
// Images whose T# the shader loads at run time (ImageResource::runtime). The table maps a T#
// to its translation entry: RuntimeImageEntries entries of RuntimeImageEntryWords words, the
// eight T# dwords, the translation index + 1 (0: empty) and the binding; the host fills it. A T#
// the table lacks is requested in the request buffer: word 0 counts requests, a bitset of one bit
// per table entry (its home entry) lets one invocation request each, then RuntimeImageRequests
// records of the eight dwords and the binding + 1.
inline constexpr uint32_t BindlessRuntimeTable     = 7;
inline constexpr uint32_t BindlessRuntimeRequests  = 8;
inline constexpr uint32_t RuntimeImageEntries      = 32768;
inline constexpr uint32_t RuntimeImageEntryWords   = 12;
inline constexpr uint32_t RuntimeImageProbes       = 4;
inline constexpr uint32_t RuntimeImageRequests     = 1024;
inline constexpr uint32_t RuntimeImageRequestWords = 9;
inline constexpr uint32_t RuntimeImageBitsOffset   = 16; // words
inline constexpr uint32_t RuntimeImageRingOffset =
    RuntimeImageBitsOffset + RuntimeImageEntries / 32u;
inline constexpr uint32_t RuntimeImageRequestBufferWords =
    RuntimeImageRingOffset + RuntimeImageRequests * RuntimeImageRequestWords;
// The home entry of a T# in the table, as the shader computes it (FNV-1a over the dwords and the
// binding, then a final mix).
[[nodiscard]] constexpr uint32_t RuntimeImageHash(const uint32_t* dwords, uint32_t binding) {
	uint32_t hash = 0x811c9dc5u;
	for (uint32_t i = 0; i < 8u; i++) {
		hash = (hash ^ dwords[i]) * 0x01000193u;
	}
	hash = (hash ^ binding) * 0x01000193u;
	hash ^= hash >> 15u;
	return hash & (RuntimeImageEntries - 1u);
}
// A translation entry for a key whose texture is not resident yet; the shader samples slot 0.
inline constexpr uint32_t BindlessPending        = 0xffffffffu;
// The S# a sampler without a usable heap entry gets: wrap on every axis, trilinear, full LOD range.
inline constexpr std::array<uint32_t, 4> BindlessDefaultSampler {
    0x00000000u,                             // wrap on every axis
    0xfffu << 12u,                           // max_lod 255.9
    (1u << 20u) | (1u << 22u) | (2u << 26u), // bilinear mag/min, linear mip
    0x00000000u};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_

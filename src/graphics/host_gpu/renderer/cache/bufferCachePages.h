#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERCACHEPAGES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERCACHEPAGES_H_

#include <cstddef>
#include <cstdint>

// The buffer cache's page layout as the shaders see it (BDA page table, fault records), and the
// shader trap record (faultManager.h), without
// the rest of bufferCache.h. The SPIR-V emitter includes this instead of the buffer cache: every
// header the recompiler includes is part of the shader disk cache key (shader_cache_version.cmake),
// and bufferCache.h brought in the memory tracker, the page manager and kernel/memory.h, so any
// change there threw away every shader cache. bufferCache.h checks these against its own values.
namespace Libs::Graphics::BufferCachePages {

inline constexpr uint32_t kPageBits           = 14;
inline constexpr uint64_t kPageSize           = uint64_t {1} << kPageBits;
// LOWER_ADDRESS_SIZE (regionDefinitions.h) and the extended memory window (kernel/memory.h).
inline constexpr uint64_t kLowerAddressSize   = uint64_t {1} << 40u;
inline constexpr uint64_t kExtendedMemoryBase = 0x080000000000ull;
inline constexpr uint64_t kExtendedMemorySize = 512ull * 1024 * 1024 * 1024;
inline constexpr uint64_t kNumPages           = (kLowerAddressSize + kExtendedMemorySize) >> kPageBits;

} // namespace Libs::Graphics::BufferCachePages

namespace Libs::Graphics {

// Appended after the page-fault bitset. The first invocation claiming the record
// writes its payload; the host reads it only after the submission completes.
struct ShaderTrapRecord {
	uint32_t claimed          = 0;
	uint32_t shader_hash_low  = 0;
	uint32_t shader_hash_high = 0;
	uint32_t pc               = 0;
	uint32_t code             = 0;
	uint32_t reserved[3] {};
};
static_assert(sizeof(ShaderTrapRecord) == 32);

// Follows the trap record: the shader-call variants the expansion trapped on (missed), as an
// open-addressed set. A missing invocation claims a slot by its key (a hash of the target and
// data words with bit 0 set, so never zero), then writes the rest; the host reads it after the
// submission.
struct ShaderCallMissSlot {
	uint32_t key              = 0;
	uint32_t target_low       = 0;
	uint32_t target_high      = 0;
	uint32_t data0            = 0;
	uint32_t data1            = 0;
	uint32_t shader_hash_low  = 0;
	uint32_t shader_hash_high = 0;
	uint32_t reserved         = 0;
};
inline constexpr uint32_t kShaderCallMissSlots  = 256;
inline constexpr uint32_t kShaderCallMissProbes = 8;
static_assert(sizeof(ShaderCallMissSlot) == 32);

// Diagnostics (KYTY_PROBE, Translate.cpp): register values a shader recorded at a pc. Each of up
// to kShaderProbePoints probe points counts the active lanes that reach it in its probe_count,
// and a lane's hit number picks its record among the point's kShaderProbeSlotsPerPoint: the first
// kShaderProbeEarly hits, then the hits numbered by a power of 4 (4, 16, ... 4^12), so the samples
// spread over every draw of the submission. The host logs them after the submission.
struct ShaderProbeRecord {
	uint32_t shader_hash_low  = 0;
	uint32_t shader_hash_high = 0;
	uint32_t pc               = 0;
	uint32_t count            = 0; // the values recorded
	uint32_t hit              = 0; // the lane's hit number at its point
	uint32_t reserved[3] {};
	uint32_t values[12] {};
};
inline constexpr uint32_t kShaderProbePoints        = 8;
inline constexpr uint32_t kShaderProbeSlotsPerPoint = 16;
inline constexpr uint32_t kShaderProbeEarly         = 4;
inline constexpr uint32_t kShaderProbeSlots  = kShaderProbePoints * kShaderProbeSlotsPerPoint;
inline constexpr uint32_t kShaderProbeValues = 12;
static_assert(sizeof(ShaderProbeRecord) == 80);

struct ShaderTrapArea {
	ShaderTrapRecord   record;
	ShaderCallMissSlot misses[kShaderCallMissSlots];
	uint32_t           probe_count[kShaderProbePoints] {};
	// The faulted pages a ray tracing shader (ShaderInfo::uses_bvh) recorded first: its rays
	// reached BVH memory not mapped yet. The host maps them as usual, and when they are all the
	// window's faults, does not count them as residency changing under the dispatches recovery
	// trusts (FaultManager).
	uint32_t           ray_faults = 0;
	uint32_t           probe_reserved[15 - kShaderProbePoints] {};
	ShaderProbeRecord  probes[kShaderProbeSlots];
};
static_assert(offsetof(ShaderTrapArea, misses) == sizeof(ShaderTrapRecord));
static_assert(offsetof(ShaderTrapArea, probes) ==
              offsetof(ShaderTrapArea, probe_count) + 16 * sizeof(uint32_t));

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERCACHEPAGES_H_

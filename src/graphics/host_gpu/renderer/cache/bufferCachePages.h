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
// Round 99: 512 (was 256): a frame of the ray tracing mode reports a hundred targets or more,
// the partitioned shaders' (ShaderCallPartition) each frame again.
inline constexpr uint32_t kShaderCallMissSlots  = 512;
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

// The rounds the waves of ray tracing shaders run in their bounded loops (LoopBudgetTick,
// EmitRayLoopRounds in spirvEmitterMemory.cpp). A shader claims a slot by its key (a hash of its
// shader hash with bit 0 set). Its loops take rows by the order the emission meets their
// headers (the last row takes every later loop), labeled by the lowest pc of their code, and a
// last row counts the rounds of all its loops together, which the loop budget bounds. A row's
// bucket 0 counts the waves that ran the loop at all, bucket b the waves that reached 2^(8+b)
// rounds in it (512, 1024, ... 2^20). One lane of a host subgroup (one wave of the guest's) counts.
// The host logs them (FaultManager).
inline constexpr uint32_t kRayLoopSlots            = 4;
inline constexpr uint32_t kRayLoopRows             = 6;
inline constexpr uint32_t kRayLoopBuckets          = 13;
inline constexpr uint32_t kRayLoopFirstPowerBucket = 9; // bucket 1 is 2^9 rounds
struct RayLoopSlot {
	uint32_t key              = 0;
	uint32_t shader_hash_low  = 0;
	uint32_t shader_hash_high = 0;
	uint32_t cap              = 0;   // the loop budget, rounds a wave
	uint32_t pc[kRayLoopRows] {};    // each row's loop label (its lowest pc) plus 1 (0: no loop)
	uint32_t spent[kRayLoopRows] {}; // the waves whose budget ran out in the row's loop
	uint32_t waves[kRayLoopRows + 1][kRayLoopBuckets] {}; // the last row: all loops together
	uint32_t reserved[5] {};
};
static_assert(sizeof(RayLoopSlot) == 112 * sizeof(uint32_t));

// Diagnostics (KYTY_PROBE <hash>@stuck:<registers>): the registers of a wave of a ray tracing
// shader when its rounds in one of its bounded loops reach kStuckProbeRounds (waves that finish
// stay far below). One lane of the wave claims a sample by the area's stuck_count; the host logs
// the samples (FaultManager) and reads the guest memory they point at (KYTY_PROBE_LIST).
inline constexpr uint32_t kStuckProbeRounds  = 1024;
inline constexpr uint32_t kStuckProbeSamples = 32;
struct StuckProbeSample {
	uint32_t shader_hash_low  = 0;
	uint32_t shader_hash_high = 0;
	uint32_t loop_pc          = 0; // the loop's label (RayLoopSlot::pc)
	uint32_t point            = 0; // the probe point
	uint32_t count            = 0; // the values recorded
	uint32_t reserved[3] {};
	uint32_t values[kShaderProbeValues] {};
};
static_assert(sizeof(StuckProbeSample) == 80);

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
	RayLoopSlot        ray_loops[kRayLoopSlots];
	uint32_t           stuck_count = 0;
	uint32_t           stuck_reserved[3] {};
	StuckProbeSample   stuck[kStuckProbeSamples];
};
static_assert(offsetof(ShaderTrapArea, misses) == sizeof(ShaderTrapRecord));
static_assert(offsetof(ShaderTrapArea, probes) ==
              offsetof(ShaderTrapArea, probe_count) + 16 * sizeof(uint32_t));

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERCACHEPAGES_H_
